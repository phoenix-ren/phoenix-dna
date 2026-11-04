#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Local TTS worker: run by the py3.13 venv, never by the daemon.

The daemon (Python 3.14) cannot import kokoro-onnx / onnxruntime (they
cap below 3.14), so this script is executed by the venv's interpreter
and speaks the stdio protocol described in local_engine.py:

    stdin  : JSON lines - {"op": "hello", "model": …} first, then
             {"op": "synthesize", "text": …, "voice": …} requests and
             {"op": "stop"} to abandon the synthesis in flight
    stdout : one JSON line answering the hello (engine, sample rate,
             voices — or {"error": …}), then 4-byte little-endian
             length-prefixed s16le PCM frames per synthesis; a
             zero-length frame ends the request

Kokoro-82M (kokoro-onnx) is the engine; piper-tts is the accepted
fallback. The model stays loaded for the life of the process — loading
takes seconds, and per-sentence respawns would make local speech
unusable.
"""

from __future__ import annotations

import io
import json
import select
import struct
import sys
import wave
from pathlib import Path

_FRAME_HEADER = struct.Struct("<I")
_FRAME_SIZE = 4096

_stop = False
# Lines peeked mid-synthesis that were not the stop request: the main
# loop must still see them (a stop can be followed immediately by the
# next synthesize).
_held_lines: list[str] = []


def _write_frame(pcm: bytes) -> bool:
    """One PCM frame; False when a stop was requested meanwhile."""
    if _stop or not pcm:
        return not _stop
    sys.stdout.buffer.write(_FRAME_HEADER.pack(len(pcm)) + pcm)
    return True


def _end_frame() -> None:
    global _stop
    _stop = False
    sys.stdout.buffer.write(_FRAME_HEADER.pack(0))
    sys.stdout.buffer.flush()


def _check_stop() -> bool:
    """Non-blocking stdin peek; holds back lines that are not stops."""
    global _stop
    ready, _, _ = select.select([sys.stdin], [], [], 0)
    if ready:
        line = sys.stdin.readline()
        if not line:
            _stop = True  # the daemon went away
        else:
            try:
                request = json.loads(line)
                if request.get("op") == "stop":
                    _stop = True
                else:
                    _held_lines.append(line)
            except ValueError:
                pass
    return _stop


class Kokoro:
    """kokoro-onnx behind the worker protocol."""

    def __init__(self, model: str, voices_file: str):
        import numpy as np
        from kokoro_onnx import KokoroONNX

        self._numpy = np
        self._tts = KokoroONNX(model_path=model, voices_path=voices_file)
        self.sample_rate = getattr(self._tts, "sample_rate", 24000) or 24000
        available = getattr(self._tts, "voices", None)
        self.voices = list(available) if available else ["af_heart", "am_michael"]

    def synthesize(self, text: str, voice: str) -> None:
        if voice not in self.voices:
            voice = self.voices[0] if self.voices else "af_heart"
        stream = getattr(self._tts, "create_stream", None)
        if stream is not None:
            for chunk, _rate in stream(text, voice=voice, speed=1.0, lang="en-us"):
                if _check_stop() or not _write_frame(_to_s16(chunk)):
                    break
        else:
            samples, _rate = self._tts.create(text, voice=voice, speed=1.0, lang="en-us")
            pcm = _to_s16(samples)
            while pcm and not _check_stop():
                frame, pcm = pcm[:_FRAME_SIZE], pcm[_FRAME_SIZE:]
                if not _write_frame(frame):
                    break
        _end_frame()


class Piper:
    """piper-tts behind the worker protocol."""

    def __init__(self, model: str):
        from piper import PiperVoice

        self._voice = PiperVoice.load(model)
        self.sample_rate = self._voice.config.sample_rate
        self.voices = []

    def synthesize(self, text: str, voice: str) -> None:
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(self.sample_rate)
            self._voice.synthesize(text, wav)
        payload = buffer.getvalue()[44:]  # past the canonical WAV header
        while payload and not _check_stop():
            frame, payload = payload[:_FRAME_SIZE], payload[_FRAME_SIZE:]
            if not _write_frame(frame):
                break
        _end_frame()


def _to_s16(chunk) -> bytes:
    """float32 samples [-1, 1] → s16le bytes."""
    import numpy as np

    return (np.clip(chunk, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()


def _build_engine(model_path: str):
    """Kokoro when its model and voices file are there, else Piper."""
    if model_path:
        model = Path(model_path).expanduser()
        if model.is_file():
            kokoro_voices = None
            for candidate in (
                model.with_name("voices.json"),
                model.with_suffix(".json"),
            ):
                if candidate.is_file():
                    kokoro_voices = str(candidate)
                    break
            if kokoro_voices is not None:
                try:
                    return Kokoro(str(model), kokoro_voices)
                except ImportError:
                    pass  # fall through to piper
            try:
                return Piper(str(model))
            except ImportError as exc:
                raise RuntimeError(
                    "neither kokoro-onnx nor piper-tts is installed in the "
                    f"venv ({exc})"
                ) from exc
    # No (or unusable) model path: try Kokoro's default download names
    # next to the worker's cwd, else give up with instructions.
    raise RuntimeError(
        "tts_voice_local does not point at a model file (set it to the "
        "Kokoro .onnx — with voices.json beside it — or a Piper .onnx)"
    )


def _answer_hello(request: dict) -> int:
    try:
        engine = _build_engine(str(request.get("model", "")))
    except Exception as exc:  # a broken setup must not hang the daemon
        sys.stdout.write(json.dumps({"error": str(exc)}) + "\n")
        sys.stdout.flush()
        return 1
    sys.stdout.write(
        json.dumps(
            {
                "engine": type(engine).__name__.lower(),
                "sample_rate": engine.sample_rate,
                "voices": engine.voices,
            }
        )
        + "\n"
    )
    sys.stdout.flush()
    _serve(engine)
    return 0


def _serve(engine) -> None:
    while True:
        line = _held_lines.pop(0) if _held_lines else sys.stdin.readline()
        if not line:
            return
        try:
            request = json.loads(line)
        except ValueError:
            continue
        if request.get("op") == "synthesize":
            try:
                engine.synthesize(str(request.get("text", "")), str(request.get("voice", "")))
            except Exception as exc:  # reported, not raised: keep serving
                print(f"tts_worker: synthesis failed: {exc}", file=sys.stderr)
                sys.stderr.flush()
                _end_frame()
        elif request.get("op") == "stop":
            global _stop
            _stop = True


def main() -> int:
    for line in sys.stdin:
        try:
            request = json.loads(line)
        except ValueError:
            continue
        if request.get("op") == "hello":
            return _answer_hello(request)
        # Anything before the hello is meaningless; the daemon always
        # speaks hello first.
    return 0


if __name__ == "__main__":
    sys.exit(main())
