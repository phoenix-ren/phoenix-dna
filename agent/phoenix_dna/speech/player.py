# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Playback: one long-lived PipeWire sink fed raw PCM.

The sink is a ``pw-cat --playback`` subprocess reading s16le from its
stdin — started lazily, kept alive across sentences and utterances for
the whole session, and killed only by an interrupt or teardown. That is
the difference between a conversation and a series of beeps: a fresh
PipeWire stream per sentence would pop and gap at every boundary, and
"do not shell out per sentence" is the rule.

An ``ffmpeg`` resampler sits in front of the sink when an engine's
sample rate differs from the sink's (the local engine speaks 24 kHz,
the API engine 32 kHz); when the rates match, PCM flows straight into
``pw-cat``. Writes block when the sink's buffer is full — that is the
pacing, and it happens on the speaker's worker thread, never on the
D-Bus main loop.
"""

from __future__ import annotations

import logging
import subprocess
import threading

log = logging.getLogger("phoenix.speech.player")

PW_CAT = "/usr/bin/pw-cat"
FFMPEG = "/usr/bin/ffmpeg"


class PlaybackStopped(RuntimeError):
    """The sink went away (interrupted, or PipeWire dropped it)."""


class Player:
    """Owns the ffmpeg→pw-cat chain; safe to call from any thread."""

    def __init__(
        self,
        *,
        rate: int = 32000,
        channels: int = 1,
        pwcat_bin: str = PW_CAT,
        ffmpeg_bin: str = FFMPEG,
    ):
        self.rate = rate
        self.channels = channels
        self.pwcat_bin = pwcat_bin
        self.ffmpeg_bin = ffmpeg_bin
        self._lock = threading.Lock()
        self._sink: subprocess.Popen | None = None
        self._resampler: subprocess.Popen | None = None
        self._chain_rate: int | None = None
        self._closed = False

    # -- public API ---------------------------------------------------------

    def start(self, source_rate: int) -> None:
        """Ensure the chain runs and accepts PCM at ``source_rate`` Hz."""
        with self._lock:
            if self._closed:
                raise PlaybackStopped("player is closed")
            if self._sink is not None and source_rate == self._chain_rate:
                if self._sink.poll() is None:
                    return
                log.warning("pw-cat died; restarting the sink")
                self._teardown_locked()
            self._spawn_locked(source_rate)

    def write(self, pcm: bytes) -> None:
        """Feed PCM into the chain; blocks as the sink paces itself."""
        with self._lock:
            sink = self._sink
            resampler = self._resampler
            if self._closed or sink is None or sink.poll() is not None:
                raise PlaybackStopped("the playback sink is gone")
            target = resampler.stdin if resampler is not None else sink.stdin
        if target is None:  # pragma: no cover - spawned with PIPE always
            raise PlaybackStopped("the playback pipe is missing")
        try:
            target.write(pcm)
        except (BrokenPipeError, OSError) as exc:
            raise PlaybackStopped(f"the playback pipe broke: {exc}") from exc

    def stop(self) -> None:
        """Interrupt now: kill the chain. The next start() respawns."""
        with self._lock:
            self._teardown_locked()

    def close(self) -> None:
        """Tear down for good (idempotent)."""
        with self._lock:
            self._closed = True
            self._teardown_locked()

    # -- internals ----------------------------------------------------------

    def _spawn_locked(self, source_rate: int) -> None:
        sink = subprocess.Popen(
            [
                self.pwcat_bin,
                "--playback",
                "--raw",
                "--rate",
                str(self.rate),
                "--channels",
                str(self.channels),
                "--format",
                "s16",
                "-",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        resampler = None
        if source_rate != self.rate:
            # ffmpeg resamples the engine's rate into the sink's; its
            # stdout is piped straight into pw-cat's stdin.
            resampler = subprocess.Popen(
                [
                    self.ffmpeg_bin,
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-f",
                    "s16le",
                    "-ar",
                    str(source_rate),
                    "-ac",
                    str(self.channels),
                    "-i",
                    "pipe:0",
                    "-f",
                    "s16le",
                    "-ar",
                    str(self.rate),
                    "-ac",
                    str(self.channels),
                    "pipe:1",
                ],
                stdin=subprocess.PIPE,
                stdout=sink.stdin,
                stderr=subprocess.DEVNULL,
            )
        self._sink = sink
        self._resampler = resampler
        self._chain_rate = source_rate
        log.debug(
            "playback chain up (source %d Hz, sink %d Hz, resampled=%s)",
            source_rate,
            self.rate,
            resampler is not None,
        )

    def _teardown_locked(self) -> None:
        # SIGKILL, not SIGTERM: an interrupted sentence must fall silent
        # *now*; a graceful drain would finish speaking what the user
        # asked to stop. The pipes are closed first — a BufferedWriter
        # finalized with bytes still buffered would try to flush into
        # the corpse and complain from the garbage collector.
        for process in (self._resampler, self._sink):
            if process is None:
                continue
            for stream in (process.stdin, process.stdout):
                if stream is not None:
                    try:
                        stream.close()
                    except OSError:
                        pass
            try:
                process.kill()
            except Exception:  # pragma: no cover - already gone
                pass
        for process in (self._resampler, self._sink):
            if process is None:
                continue
            try:
                process.wait(timeout=0.5)
            except Exception:  # pragma: no cover - already gone
                pass
        self._sink = None
        self._resampler = None
        self._chain_rate = None
