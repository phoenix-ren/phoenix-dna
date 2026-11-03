# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""MiniMax speech synthesis over the streaming HTTP endpoint.

``POST /v1/t2a_v2`` with ``"stream": true`` answers with SSE chunks
whose ``data.audio`` field carries hex-encoded audio — the same
urllib-plus-SSE approach the LLM provider uses (provider.py), so no new
dependency. The WebSocket endpoint is deliberately not used: sentence
segmentation happens locally anyway, and a plain request/response per
sentence is easier to interrupt and to reason about.

Encoded audio is not PCM: each sentence's stream is pushed through one
ffmpeg subprocess (``-f opus -i pipe:0 -f s16le …``) that is torn down
when the sentence's request ends. A decoder shared across sentences
would have to survive concatenated independent streams, whose frame
boundaries are not visible in the decoded PCM — and would keep decoding
a sentence the user just interrupted. The *sink* (player.py) is what
stays alive across sentences; that is what keeps playback gapless.

``opus`` is the preferred format (an order of magnitude smaller on the
wire than PCM); if the service rejects it, the request is retried once
as mp3.
"""

from __future__ import annotations

import json
import logging
import subprocess
import threading
import urllib.error
import urllib.request
from typing import Iterator

log = logging.getLogger("phoenix.speech.minimax")

DEFAULT_ENDPOINT = "https://api.minimax.io/v1/t2a_v2"
DEFAULT_MODEL = "speech-2.8-turbo"
DEFAULT_VOICE = "English_expressive_narrator"

#: Formats the endpoint accepts, and ffmpeg's demuxer name for each.
_FORMATS = {"opus": "opus", "mp3": "mp3", "flac": "flac", "pcm": None}
_FALLBACK_FORMAT = "mp3"


class SynthesisError(RuntimeError):
    """A synthesis request failed; the message is fit for the log."""


class MinimaxEngine:
    """Streams MiniMax TTS audio as PCM chunks, engine interface."""

    def __init__(
        self,
        *,
        api_key: str,
        model: str = DEFAULT_MODEL,
        endpoint: str = DEFAULT_ENDPOINT,
        voice: str = DEFAULT_VOICE,
        speed: float = 1.0,
        vol: float = 1.0,
        pitch: float = 0.0,
        sample_rate: int = 32000,
        bitrate: int = 128_000,
        audio_format: str = "opus",
        timeout: float = 30.0,
        ffmpeg_bin: str = "/usr/bin/ffmpeg",
    ):
        self.api_key = api_key
        self.model = model
        self.endpoint = endpoint
        self.voice = voice
        self.speed = speed
        self.vol = vol
        self.pitch = pitch
        self.sample_rate = sample_rate
        self.bitrate = bitrate
        self.audio_format = audio_format if audio_format in _FORMATS else "opus"
        self.timeout = timeout
        self.ffmpeg_bin = ffmpeg_bin
        self._lock = threading.Lock()
        self._generation = 0
        self._response = None
        self._decoder = None

    # -- engine interface ---------------------------------------------------

    def voices(self) -> list[str]:
        """The configured voice; the catalog is not needed to speak."""
        return [self.voice]

    def stop(self) -> None:
        """Abandon the in-flight synthesis: kill decoder and response."""
        with self._lock:
            self._generation += 1
            response, self._response = self._response, None
            decoder, self._decoder = self._decoder, None
        if response is not None:
            try:
                response.close()
            except Exception:  # pragma: no cover - defensive
                pass
        if decoder is not None:
            _kill(decoder)

    def close(self) -> None:
        self.stop()

    def synthesize(self, text: str) -> Iterator[bytes]:
        """Yield mono s16le PCM chunks for one sentence of text."""
        with self._lock:
            self._generation += 1
            generation = self._generation
        response, chosen_format = self._open(text, generation)
        if chosen_format == "pcm":
            # The service did the decoding; the hex payload is PCM.
            try:
                yield from self._consume_raw(response, generation)
            finally:
                _close_quietly(response)
                with self._lock:
                    if self._response is response:
                        self._response = None
            return
        decoder = self._spawn_decoder(chosen_format)
        with self._lock:
            if generation != self._generation:
                _kill(decoder)
                _close_quietly(response)
                return
            self._response = response
            self._decoder = decoder
        try:
            yield from self._decode(response, decoder, generation)
        finally:
            _close_quietly(response)
            _kill(decoder)
            with self._lock:
                if self._decoder is decoder:
                    self._decoder = None
                if self._response is response:
                    self._response = None

    # -- internals ----------------------------------------------------------

    def _payload(self, text: str, audio_format: str) -> dict:
        return {
            "model": self.model,
            "text": text,
            "stream": True,
            "language_boost": "auto",
            "voice_setting": {
                "voice_id": self.voice,
                "speed": self.speed,
                "vol": self.vol,
                "pitch": self.pitch,
            },
            "audio_setting": {
                "sample_rate": self.sample_rate,
                "bitrate": self.bitrate,
                "format": audio_format,
                "channel": 1,
            },
            "text_normalization_mode": "basic",
        }

    def _open(self, text: str, generation: int):
        """Open the SSE stream, falling back to mp3 when opus is refused."""
        formats = [self.audio_format]
        if self.audio_format != _FALLBACK_FORMAT:
            formats.append(_FALLBACK_FORMAT)
        last_error: Exception | None = None
        for audio_format in formats:
            request = urllib.request.Request(
                self.endpoint,
                data=json.dumps(self._payload(text, audio_format)).encode(),
                headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {self.api_key}",
                    "Accept": "text/event-stream",
                },
                method="POST",
            )
            try:
                response = urllib.request.urlopen(request, timeout=self.timeout)
            except urllib.error.HTTPError as exc:
                body = exc.read(300) or b""
                last_error = SynthesisError(
                    f"t2a_v2 HTTP {exc.code} ({audio_format}): "
                    f"{body.decode(errors='replace')}"
                )
                if exc.code in (400, 415) and audio_format != formats[-1]:
                    log.warning("%s; retrying as %s", last_error, formats[-1])
                    continue
                raise last_error from exc
            except (urllib.error.URLError, OSError) as exc:
                raise SynthesisError(f"cannot reach {self.endpoint}: {exc}") from exc
            with self._lock:
                if generation != self._generation:
                    _close_quietly(response)
                    return _StoppedResponse(), audio_format
                self._response = response
            return response, audio_format
        raise last_error or SynthesisError("t2a_v2: no format was accepted")

    def _sse_audio(self, response) -> Iterator[bytes]:
        """Hex-decoded audio frames from the SSE body."""
        for raw_line in response:
            line = raw_line.strip()
            if not line.startswith(b"data:"):
                continue
            payload = line[5:].strip()
            if not payload or payload == b"[DONE]":
                continue
            try:
                chunk = json.loads(payload)
            except ValueError:
                continue
            status = chunk.get("base_resp") or {}
            code = status.get("status_code", 0)
            if code not in (0, None):
                raise SynthesisError(
                    f"t2a_v2 error {code}: {status.get('status_msg', '')}"
                )
            audio = (chunk.get("data") or {}).get("audio")
            if audio:
                try:
                    yield bytes.fromhex(audio)
                except ValueError:
                    continue

    def _consume_raw(self, response, generation: int) -> Iterator[bytes]:
        for frame in self._sse_audio(response):
            if generation != self._generation:
                return
            yield frame

    def _spawn_decoder(self, audio_format: str):
        """ffmpeg: encoded audio on stdin → s16le PCM on stdout."""
        demuxer = _FORMATS[audio_format]
        return subprocess.Popen(
            [
                self.ffmpeg_bin,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                demuxer,
                "-i",
                "pipe:0",
                "-f",
                "s16le",
                "-ar",
                str(self.sample_rate),
                "-ac",
                "1",
                "pipe:1",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def _decode(self, response, decoder, generation: int) -> Iterator[bytes]:
        """Pump the HTTP audio through the decoder; yield its PCM."""
        pump_error: list[Exception] = []

        def pump() -> None:
            try:
                assert decoder.stdin is not None
                for frame in self._sse_audio(response):
                    if generation != self._generation:
                        break
                    decoder.stdin.write(frame)
                decoder.stdin.close()
            except (BrokenPipeError, OSError, SynthesisError) as exc:
                pump_error.append(exc)
            except Exception as exc:  # pragma: no cover - defensive
                pump_error.append(exc)
                log.exception("audio pump failed")
            finally:
                # The decoder's stdin must reach EOF however the pump
                # dies, or the reader below waits forever on its stdout.
                try:
                    if decoder.stdin is not None and not decoder.stdin.closed:
                        decoder.stdin.close()
                except OSError:  # pragma: no cover - already gone
                    pass

        thread = threading.Thread(
            target=pump, name="phoenix-tts-pump", daemon=True
        )
        thread.start()
        produced = False
        try:
            assert decoder.stdout is not None
            while True:
                chunk = decoder.stdout.read(4096)
                if not chunk:
                    break
                if generation != self._generation:
                    return
                produced = True
                yield chunk
        finally:
            thread.join(timeout=2.0)
            if not produced and pump_error:
                raise SynthesisError(f"t2a_v2 synthesis failed: {pump_error[0]}")


class _StoppedResponse:
    """A stand-in response that yields nothing (stopped before it began)."""

    def __iter__(self):
        return iter(())

    def close(self) -> None:
        return None


def _close_quietly(response) -> None:
    try:
        response.close()
    except Exception:  # pragma: no cover - defensive
        pass


def _kill(process) -> None:
    try:
        process.kill()
        process.wait(timeout=1.0)
    except Exception:  # pragma: no cover - already gone
        pass
