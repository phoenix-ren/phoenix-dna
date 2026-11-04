# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""OpenAI-compatible speech synthesis over the HTTP endpoint.

``POST /v1/audio/speech`` with ``{"model": …, "input": …, "voice": …}``
answers with the audio itself — a plain body, not the hex-encoded SSE
stream MiniMax uses (see minimax_engine.py). The request is a JSON
document and the reply is a file, so this engine is the simpler of the
two: one request per sentence, the bytes straight into ffmpeg.

OpenAI's endpoint is the shape most speech servers imitate, so this
engine is what a user of another provider usually wants: point
``tts_endpoint`` at their server and it works the same way. The one
thing that is not portable is the *format* — the reply is MP3 by
default, and the other formats are named differently by different
servers — so the format is negotiated through the request and a
failure falls back to MP3, as with MiniMax's engine.

Encoded audio is not PCM, so it is pushed through one ffmpeg subprocess
per sentence, exactly as the MiniMax engine does; the reasoning for
one decoder per sentence is in that module's docstring.
"""

from __future__ import annotations

import json
import logging
import subprocess
import threading
import urllib.error
import urllib.request
from typing import Iterator

from ..http import headers as http_headers

log = logging.getLogger("phoenix.speech.openai")

DEFAULT_ENDPOINT = "https://api.openai.com/v1/audio/speech"
DEFAULT_MODEL = "gpt-4o-mini-tts"
DEFAULT_VOICE = "alloy"

#: The formats the endpoint accepts, and ffmpeg's demuxer for each.
#: "pcm" is raw 24 kHz 16-bit little-endian samples without a header,
#: so ffmpeg is told the rate as well (see _spawn_decoder).
_FORMATS = {
    "mp3": "mp3",
    "opus": "opus",
    "aac": "aac",
    "flac": "flac",
    "wav": "wav",
    "pcm": "s16le",
}

#: The format every server understands, and the one to fall back to.
_FALLBACK_FORMAT = "mp3"

#: The sample rate OpenAI's raw PCM comes in.
_PCM_SAMPLE_RATE = 24000


class SynthesisError(RuntimeError):
    """A synthesis request failed; the message is fit for the log."""


class OpenAiEngine:
    """Streams OpenAI TTS audio as PCM chunks, engine interface."""

    def __init__(
        self,
        *,
        api_key: str,
        model: str = DEFAULT_MODEL,
        endpoint: str = DEFAULT_ENDPOINT,
        voice: str = DEFAULT_VOICE,
        speed: float = 1.0,
        instructions: str = "",
        sample_rate: int = 32000,
        audio_format: str = "mp3",
        timeout: float = 30.0,
        ffmpeg_bin: str = "/usr/bin/ffmpeg",
    ):
        self.api_key = api_key
        self.model = model
        self.endpoint = endpoint
        self.voice = voice
        self.speed = speed
        #: Free-form direction for the voice ("Speak cheerfully"). Only
        #: the newer models understand it; the older ones refuse the
        #: field, so it is left out unless it was asked for.
        self.instructions = instructions
        self.sample_rate = sample_rate
        self.audio_format = audio_format if audio_format in _FORMATS else _FALLBACK_FORMAT
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

        The configured format is tried first; when an attempt fails
        *before any audio arrived* — an HTTP refusal, a stream that
        yields nothing — the whole request is retried once as MP3 (the
        format every server understands). A failure after audio was
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
                    "audio/speech failed as %s (%s); retrying as %s",
                    audio_format,
                    exc,
                    attempts[-1],
                )

    def _synthesize_with(
        self, text: str, audio_format: str, generation: int
    ) -> Iterator[bytes]:
        """One synthesis attempt in one audio format."""
        response = self._open(text, audio_format, generation)
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
        payload = {
            "model": self.model,
            "input": text,
            "voice": self.voice,
            "response_format": audio_format,
        }
        # The speed is a plain factor, like everywhere else. Servers
        # that do not know it may refuse the whole request, so it is
        # only sent when it actually differs from the default.
        if self.speed and float(self.speed) != 1.0:
            payload["speed"] = float(self.speed)
        if self.instructions:
            payload["instructions"] = self.instructions
        return payload

    def _open(self, text: str, audio_format: str, generation: int):
        """Open the response for one format; raises SynthesisError on
        transport-level refusals (an in-band error surfaces later,
        while the body is read)."""
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(self._payload(text, audio_format)).encode(),
            headers=http_headers(
                {
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {self.api_key}",
                    "Accept": "audio/*",
                }
            ),
            method="POST",
        )
        try:
            response = urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as exc:
            body = exc.read(300) or b""
            raise SynthesisError(
                f"audio/speech HTTP {exc.code} ({audio_format}): "
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

    def _spawn_decoder(self, audio_format: str):
        """ffmpeg: encoded audio on stdin → s16le PCM on stdout.

        Even the raw PCM format goes through ffmpeg: it arrives at
        24 kHz, while the engine promises its callers a single rate
        (self.sample_rate), so the samples have to be converted. Passing
        them on untouched would play the sentence fast and too high.
        """
        demuxer = _FORMATS[audio_format]
        command = [
            self.ffmpeg_bin,
            "-hide_banner",
            "-loglevel",
            "error",
        ]
        if audio_format == "pcm":
            # Raw PCM has no header, so the rate and the layout have to
            # be given. OpenAI's raw PCM is 24 kHz, 16-bit, mono.
            command += [
                "-f",
                demuxer,
                "-ar",
                str(_PCM_SAMPLE_RATE),
                "-ac",
                "1",
                "-i",
                "pipe:0",
            ]
        else:
            command += ["-f", demuxer, "-i", "pipe:0"]
        command += [
            "-f",
            "s16le",
            "-ar",
            str(self.sample_rate),
            "-ac",
            "1",
            "pipe:1",
        ]
        return subprocess.Popen(
            command,
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
                while True:
                    chunk = response.read(4096)
                    if not chunk:
                        break
                    if generation != self._generation:
                        break
                    decoder.stdin.write(chunk)
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
                raise SynthesisError(f"speech synthesis failed: {pump_error[0]}")
            if exhausted and not produced:
                # A stream that ran dry without a single PCM byte is a
                # silent failure — the worst kind.
                raise SynthesisError("audio/speech returned no audio")


class _StoppedResponse:
    """A stand-in response that yields nothing (stopped before it began)."""

    def __iter__(self):
        return iter(())

    def read(self, size: int = -1) -> bytes:
        return b""

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
