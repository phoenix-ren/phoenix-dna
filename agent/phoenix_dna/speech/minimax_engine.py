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


def _voice_number(value: float) -> int | float:
    """Voice parameters as the service's schema wants them: integers.

    The t2a_v2 schema types speed/vol/pitch as integers, and a JSON 1.0
    is refused outright ("invalid params", status 2013) — so whole
    numbers are sent whole and only true fractions stay fractional.
    """
    value = float(value)
    return int(value) if value.is_integer() else value

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
        #: The format actually in use — the configured one until an
        #: attempt fails, then the one that last succeeded (see
        #: synthesize).
        self._active_format = self.audio_format
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
        """Yield mono s16le PCM chunks for one sentence of text.

        The preferred format is tried first; when an attempt fails
        *before any audio arrived* — an HTTP refusal, an in-band
        ``base_resp`` error, a stream that yields nothing — the whole
        request is retried once as mp3 (the opus encoder path is
        currently refused outright by the service for some accounts,
        with an opaque "unknown error"). A failure after audio was
        yielded is not retried: the words would be spoken twice.
        Whichever format last succeeded is remembered, so only the
        first sentence of a session pays the probe.
        """
        with self._lock:
            self._generation += 1
            generation = self._generation
            first_choice = self._active_format
        attempts = [first_choice]
        if first_choice != _FALLBACK_FORMAT:
            attempts.append(_FALLBACK_FORMAT)
        for audio_format in attempts:
            produced = False
            try:
                for chunk in self._synthesize_with(text, audio_format, generation):
                    produced = True
                    yield chunk
                with self._lock:
                    self._active_format = audio_format
                return
            except SynthesisError as exc:
                if produced or audio_format == attempts[-1]:
                    raise
                if generation != self._generation:
                    return
                log.warning(
                    "t2a_v2 failed as %s (%s); retrying as %s",
                    audio_format,
                    exc,
                    attempts[-1],
                )

    def _synthesize_with(
        self, text: str, audio_format: str, generation: int
    ) -> Iterator[bytes]:
        """One synthesis attempt in one audio format."""
        response = self._open(text, audio_format, generation)
        if audio_format == "pcm":
            # The service did the decoding; the hex payload is PCM.
            try:
                yield from self._consume_raw(response, generation)
            finally:
                _close_quietly(response)
                with self._lock:
                    if self._response is response:
                        self._response = None
            return
        decoder = self._spawn_decoder(audio_format)
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
                # The service's schema types these as integers; a JSON
                # 1.0 is rejected with "invalid params" (status 2013),
                # so whole numbers are sent whole.
                "speed": _voice_number(self.speed),
                "vol": _voice_number(self.vol),
                "pitch": _voice_number(self.pitch),
            },
            "audio_setting": {
                "sample_rate": self.sample_rate,
                "bitrate": self.bitrate,
                "format": audio_format,
                "channel": 1,
            },
            "text_normalization_mode": "basic",
        }

    def _open(self, text: str, audio_format: str, generation: int):
        """Open the SSE stream for one format; raises SynthesisError on
        transport-level refusals (in-band errors surface later, while
        the stream is read)."""
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
            raise SynthesisError(
                f"t2a_v2 HTTP {exc.code} ({audio_format}): "
                f"{body.decode(errors='replace')}"
            ) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise SynthesisError(f"cannot reach {self.endpoint}: {exc}") from exc
        with self._lock:
            if generation != self._generation:
                _close_quietly(response)
                return _StoppedResponse()
            self._response = response
        return response

    def _sse_audio(self, response) -> Iterator[bytes]:
        """The clip's mp3/opus bytes from the SSE body.

        The stream's shape matters: chunks with ``data.status`` 1 carry
        the audio as a *prefix feed* (segments that sum to slightly less
        than the whole), and the final ``status`` 2 chunk carries the
        **complete clip again** — feeding both would speak every
        sentence twice. So the final chunk is the payload, and the
        incremental feed is only a fallback for streams that end
        without one (or whose final chunk is just the last segment, so
        small that concatenation is the only sensible reading).

        A failure can arrive three ways, and all three must be heard:
        an SSE chunk carrying a non-zero ``base_resp.status_code``; a
        plain-JSON error body in place of the stream (HTTP 200,
        content-type application/json, no ``data:`` lines — exactly
        what "invalid params" looks like); and a stream that yields no
        audio at all (raised by the caller).
        """
        segments: list[bytes] = []
        for raw_line in response:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith(b"data:"):
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
                data = chunk.get("data") or {}
                audio = data.get("audio")
                if not audio:
                    continue
                try:
                    frame = bytes.fromhex(audio)
                except ValueError:
                    continue
                if data.get("status") == 2:
                    if sum(len(s) for s in segments) <= len(frame):
                        # The complete clip: the segments before it were
                        # the prefix feed of this very audio.
                        yield frame
                        return
                    # A final chunk smaller than what the segments
                    # already hold is the last segment, not the whole.
                    segments.append(frame)
                    yield from segments
                    return
                segments.append(frame)
            elif line.startswith(b"{"):
                # Not an SSE stream at all: the whole body is one JSON
                # error object. Skipping it would fail in silence.
                try:
                    chunk = json.loads(line)
                except ValueError:
                    continue
                status = chunk.get("base_resp") or {}
                code = status.get("status_code", 0)
                if code not in (0, None):
                    raise SynthesisError(
                        f"t2a_v2 error {code}: {status.get('status_msg', '')}"
                    )
        # The stream ended without a final chunk: the incremental feed
        # is all there is.
        yield from segments

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
        exhausted = False
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
            exhausted = True
        except BaseException:
            # Abandoned mid-stream (a stop, or the consumer gone):
            # finalization must not turn into a complaint.
            thread.join(timeout=2.0)
            raise
        finally:
            thread.join(timeout=2.0)
            if pump_error:
                # The stream died — possibly after audio flowed, which
                # makes it a half-spoken sentence, not a success. The
                # caller decides whether a retry is safe (it is not,
                # once words were heard).
                raise SynthesisError(f"t2a_v2 synthesis failed: {pump_error[0]}")
            if exhausted and not produced:
                # A stream that ran dry without a single PCM byte is a
                # silent failure — the worst kind.
                raise SynthesisError("t2a_v2 returned no audio")


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
    # Close the pipes before killing: a BufferedWriter finalized with
    # buffered bytes would flush into the corpse and complain from the
    # garbage collector (same reasoning as Player's teardown).
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass
    try:
        process.kill()
        process.wait(timeout=1.0)
    except Exception:  # pragma: no cover - already gone
        pass
