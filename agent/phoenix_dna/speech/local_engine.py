# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Local speech synthesis: Kokoro-82M (or Piper) in a separate venv.

The daemon runs Python 3.14; ``kokoro-onnx`` caps below 3.14, so the
local engine *cannot* be imported in-process. Instead a small worker
script (``tts_worker.py`` in this package) is executed by the venv's
interpreter and speaks a tiny stdio protocol:

* the daemon sends JSON lines — ``{"op": "hello"}`` first, then one
  ``{"op": "synthesize", "text": …}`` per request;
* the worker answers the hello with its sample rate and voices, and
  each synthesis with length-prefixed PCM frames (4-byte little-endian
  length, then the bytes; a zero-length frame ends the request);
* ``{"op": "stop"}`` abandons the synthesis in progress — the model
  stays loaded, so an interrupt costs nothing.

One worker process serves the whole session: loading Kokoro takes
seconds, and per-sentence respawns would make local speech unusable.
The heavy imports live entirely in the worker, so the daemon stays
stdlib-only and the engine's absence is never an import error — a
missing venv surfaces as a clear message on the first request.
"""

from __future__ import annotations

import json
import logging
import struct
import subprocess
import threading
from pathlib import Path
from typing import Iterator

log = logging.getLogger("phoenix.speech.local")

_FRAME_HEADER = struct.Struct("<I")

_WORKER = Path(__file__).with_name("tts_worker.py")


class LocalEngineError(RuntimeError):
    """The local engine is not usable; the message says how to fix it."""


class LocalEngine:
    """Kokoro/Piper synthesis through a persistent worker process
    (implements the SynthesisEngine interface of synth.py)."""

    def __init__(
        self,
        *,
        venv_dir: Path | str = "",
        voice_model: Path | str = "",
        voice: str = "",
    ):
        self.venv_dir = _path_or_empty(venv_dir)
        self.voice_model = _path_or_empty(voice_model)
        self.voice = voice
        self.sample_rate = 0  # learned from the worker's hello
        self._lock = threading.Lock()
        self._process: subprocess.Popen | None = None
        self._hello: dict = {}
        self._generation = 0

    # -- engine interface ---------------------------------------------------

    def voices(self) -> list[str]:
        """The worker's voice list (empty until it has been started)."""
        with self._lock:
            hello = self._hello_locked()
            return list(hello.get("voices", [])) if hello else []

    def stop(self) -> None:
        """Abandon the in-flight synthesis (the worker stays alive)."""
        with self._lock:
            self._generation += 1
            process = self._process
        if process is not None and process.poll() is None and process.stdin:
            try:
                process.stdin.write(b'{"op": "stop"}\n')
                process.stdin.flush()
            except OSError:
                pass  # dead worker: the next request respawns it

    def close(self) -> None:
        with self._lock:
            process, self._process = self._process, None
        if process is not None:
            try:
                if process.stdin:
                    process.stdin.close()
                process.wait(timeout=2.0)
            except Exception:  # pragma: no cover - defensive
                process.kill()

    def synthesize(self, text: str) -> Iterator[bytes]:
        """Yield mono s16le PCM chunks for one sentence of text."""
        with self._lock:
            hello = self._hello_locked()
            if hello is None:
                raise LocalEngineError(_missing_venv_message(self.venv_dir))
            self._generation += 1
            generation = self._generation
            process = self._process
            assert process is not None and process.stdin and process.stdout
            voice = self.voice if self.voice in hello.get("voices", []) else ""
            try:
                request = {"op": "synthesize", "text": text}
                if voice:
                    request["voice"] = voice
                payload = json.dumps(request, ensure_ascii=False).encode() + b"\n"
                process.stdin.write(payload)
                process.stdin.flush()
            except OSError as exc:
                raise LocalEngineError(f"local TTS worker died: {exc}") from exc
        self.sample_rate = int(hello.get("sample_rate", 24000))
        while True:
            header = _read_exact(process.stdout, _FRAME_HEADER.size)
            if header is None:
                raise LocalEngineError("local TTS worker closed its stream")
            (length,) = _FRAME_HEADER.unpack(header)
            if length == 0:
                return
            frame = _read_exact(process.stdout, length)
            if frame is None:
                raise LocalEngineError("local TTS stream ended mid-frame")
            if generation != self._generation:
                # Stopped mid-sentence: drain the worker's remaining
                # frames so the next request starts aligned.
                continue
            yield frame

    # -- internals ----------------------------------------------------------

    def _hello_locked(self) -> dict | None:
        """Ensure the worker runs; its hello, or None when it cannot.

        Called with the lock held. A dead worker is respawned here, so
        a crash costs one restart, not a broken engine.
        """
        if self._process is not None and self._process.poll() is None:
            return self._hello
        if self._process is not None:
            self._process.kill()
            self._process = None
        python = venv_python(self.venv_dir)
        if python is None:
            return None
        if self.voice_model == Path():
            log.warning("tts_voice_local is not set; the worker picks a default")
        try:
            process = subprocess.Popen(
                [str(python), str(_WORKER)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            raise LocalEngineError(f"cannot start the local TTS worker: {exc}") from exc
        try:
            assert process.stdin and process.stdout
            process.stdin.write(
                json.dumps(
                    {"op": "hello", "model": str(self.voice_model)}
                ).encode()
                + b"\n"
            )
            process.stdin.flush()
            line = process.stdout.readline()
        except OSError as exc:
            process.kill()
            raise LocalEngineError(f"local TTS worker is mute: {exc}") from exc
        if not line:
            reason = b""
            try:
                reason = process.stderr.read(400) if process.stderr else b""
            except Exception:  # pragma: no cover - defensive
                pass
            process.kill()
            log.error(
                "local TTS worker failed to start: %s", reason.decode(errors="replace")
            )
            return None
        try:
            hello = json.loads(line)
        except ValueError:
            process.kill()
            log.error("local TTS worker spoke garbage: %r", line[:200])
            return None
        if hello.get("error"):
            process.kill()
            log.error("local TTS worker refused to start: %s", hello["error"])
            return None
        self._process = process
        self._hello = hello
        return hello


def venv_python(venv_dir: Path) -> Path | None:
    """The venv's interpreter, or None with a helpful log when absent."""
    if venv_dir == Path():
        log.warning("tts_local_venv is not configured")
        return None
    python = venv_dir / "bin" / "python"
    if not python.is_file():
        log.warning("no interpreter at %s (is tts_local_venv a venv?)", python)
        return None
    return python


def _missing_venv_message(venv_dir: Path) -> str:
    return (
        "the local speech engine is not installed: create a Python 3.13 "
        "venv with 'pip install kokoro-onnx onnxruntime' (or piper-tts), "
        "download the Kokoro model, and point tts_local_venv / "
        f"tts_voice_local at them in agent.toml (venv now: {venv_dir or 'unset'})"
    )


def _path_or_empty(value: Path | str) -> Path:
    text = str(value).strip() if value is not None else ""
    return Path(text).expanduser() if text else Path()


def _read_exact(stream, count: int) -> bytes | None:
    chunks = []
    remaining = count
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)
