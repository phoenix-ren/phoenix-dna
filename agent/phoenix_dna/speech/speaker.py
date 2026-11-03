# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The Speaker: decides *what* is read aloud, then reads it.

The read policy in one paragraph. A run is a multi-turn tool loop;
every turn streams its own text, and only the *last* turn's text is the
answer (``final_text``) — a "Let me check the disk." preamble arrives
as ordinary TEXT_DELTA of an earlier turn. A turn is indistinguishable
from a final turn until it ends (TOOL_CALL_STARTED, or the run ends),
so **guaranteeing "final answer only" and speaking before generation
finishes are mutually exclusive**. The two read modes are therefore not
fast-and-slow variants of one thing:

* ``"final"`` (default): speak only ``final_text`` from RUN_FINISHED,
  and only for a completed run — or the error text for a failed one,
  and ``final_text`` for ``max_turns`` (the loop fills it with the last
  text or the "too many tool turns" message). Correct by construction:
  the preamble is not in ``final_text``, so it cannot be spoken. What
  is deferred is the *start*, not the delivery — the answer still comes
  out sentence by sentence as it synthesizes.
* ``"stream"``: speak while the answer is generated, holding back
  ``holdback_sentences`` complete sentences; if TOOL_CALL_STARTED
  arrives for the current turn, the held text is discarded and any
  sentence of that turn still playing is cut off. Text already spoken
  cannot be recalled — the known cost of this mode, and the reason it
  is not the default. (The system prompt's "final answer only - no
  preamble" makes the leak rare, not impossible.)

Reasoning (THINKING_DELTA) and tool output are never spoken.

Threading: ``handle_event`` runs on the agent worker thread, ``speak``/
``stop`` on the D-Bus main loop; both only buffer and enqueue. All
synthesis and playback happen on this class's own worker thread, so
neither the GLib main loop nor the agent is ever blocked. State
changes are announced through ``emitter`` as ``speech_state_changed``
events (ignored by this class when they come back around).
"""

from __future__ import annotations

import logging
import queue
import re
import threading

from .. import events
from .normalize import normalize_marked
from .player import PlaybackStopped, Player
from .segment import segment
from .synth import build_engine

log = logging.getLogger("phoenix.speech.speaker")

READ_MODES = ("final", "stream")

#: A cheap pre-check before re-normalizing the streamed buffer: a turn
#: without sentence-ending punctuation cannot have a new complete
#: sentence, so the per-delta cost stays trivial while the answer runs.
_SENTENCEISH = re.compile(r"[.!?…][\"')]?\s|\n\n")


class Speaker:
    """Consumes daemon events, maintains the turn buffer, drives playback."""

    def __init__(
        self,
        *,
        engine=None,
        player: Player | None = None,
        read_mode: str = "final",
        holdback_sentences: int = 1,
        enabled: bool = True,
        emitter=None,
        min_chunk: int = 40,
        max_chunk: int = 300,
    ):
        self.enabled = bool(enabled and engine is not None)
        self.read_mode = read_mode if read_mode in READ_MODES else "final"
        self.holdback_sentences = max(0, int(holdback_sentences))
        self._engine = engine
        self._player = player
        self._emitter = emitter or (lambda event: None)
        self._min_chunk = min_chunk
        self._max_chunk = max_chunk

        self._lock = threading.Lock()
        self._turn = 0
        self._run_id = ""
        self._buffer: list[str] = []
        self._sent_count = 0  # segments already queued this turn (stream)
        self._token = 0  # speech generation; a bump invalidates the queue
        self._jobs: queue.Queue = queue.Queue()
        self._thread: threading.Thread | None = None
        self._state = "idle"
        self._closed = False

    # -- wiring ---------------------------------------------------------------

    def set_emitter(self, emitter) -> None:
        """Where speech_state_changed events go (AgentDaemon._emit)."""
        self._emitter = emitter

    # -- event intake (agent worker thread) -----------------------------------

    def handle_event(self, event: dict) -> None:
        """Fold one daemon event into the read policy. Never blocks."""
        if not self.enabled:
            return
        kind = event.get("kind")
        if kind == events.RUN_STARTED:
            # A new question supersedes whatever is still being read.
            with self._lock:
                self._run_id = event.get("run_id", "")
                self._turn = 0
                self._buffer = []
                self._sent_count = 0
            self._invalidate()
        elif kind == events.TURN_STARTED:
            with self._lock:
                self._turn = int(event.get("turn") or 0)
                self._buffer = []
                self._sent_count = 0
        elif kind == events.TEXT_DELTA:
            with self._lock:
                self._buffer.append(event.get("text", ""))
                if self.read_mode != "stream":
                    return
                buffer = "".join(self._buffer)
            if _sentenceish(buffer):
                self._stream_some(buffer)
        elif kind == events.TOOL_CALL_STARTED:
            # This turn called tools: its text was working notes, not
            # the answer. Retract what has not been spoken, and cut off
            # whatever of it is still playing.
            if self.read_mode == "stream":
                self._invalidate()
            with self._lock:
                self._buffer = []
                self._sent_count = 0
        elif kind == events.RUN_FINISHED:
            self._finish(event)

    def _stream_some(self, buffer: str) -> None:
        """Queue complete sentences beyond the holdback window.

        Stream mode segments at sentence granularity (the holdback
        window counts sentences): each synthesis request is one
        sentence, so retraction and interrupts stay granular.
        """
        try:
            sentences = segment(
                normalize_marked(buffer),
                min_length=1,
                max_length=self._max_chunk,
            )
        except Exception:  # pragma: no cover - normalization is total
            log.exception("speech normalization failed; holding text")
            return
        # The final sentence of the buffer may still be incomplete (the
        # answer keeps streaming), so only the ones followed by another
        # are certainly whole — and the newest ``holdback`` of those
        # stay held, in case the turn turns out to call tools.
        eligible = len(sentences) - 1 if sentences else 0
        send_up_to = eligible - self.holdback_sentences
        if send_up_to <= 0:
            return
        with self._lock:
            if send_up_to <= self._sent_count:
                return
            fresh = sentences[self._sent_count : send_up_to]
            self._sent_count = send_up_to
            token = self._token
        for chunk in fresh:
            self._enqueue(chunk, token)

    def _finish(self, event: dict) -> None:
        """RUN_FINISHED: flush in stream mode, speak final_text in final mode."""
        status = event.get("status", "")
        final_text = event.get("final_text", "") or ""
        if status == "cancelled":
            self._invalidate()
            with self._lock:
                self._buffer = []
                self._sent_count = 0
            return
        if self.read_mode == "stream":
            with self._lock:
                buffer = "".join(self._buffer)
                sent = self._sent_count
                token = self._token
            if sent == 0 and not buffer.strip():
                # Nothing was ever streamed (deltas lost, or the loop
                # substituted its "too many tool turns" message): the
                # final-mode behaviour is the only correct one left.
                if final_text:
                    self._enqueue_text(final_text, token)
                return
            self._flush(buffer)
            if status == "error" and final_text:
                # The error text was never in the buffer; say it once.
                self._enqueue_text(final_text, token)
            return
        # "final": final_text is authoritative, verbatim.
        if final_text:
            with self._lock:
                token = self._token
            self._enqueue_text(final_text, token)

    def _flush(self, buffer: str) -> None:
        """Speak the held tail of a streamed answer (nothing re-spoken).

        Sentence granularity, matching ``_stream_some``: the indices
        count sentences, so the already-spoken prefix is skipped and
        exactly the remainder goes out.
        """
        try:
            sentences = segment(
                normalize_marked(buffer),
                min_length=1,
                max_length=self._max_chunk,
            )
        except Exception:  # pragma: no cover - normalization is total
            log.exception("speech normalization failed; dropping held text")
            return
        with self._lock:
            already = self._sent_count
            self._sent_count = len(sentences)
            token = self._token
        for chunk in sentences[already:]:
            self._enqueue(chunk, token)

    def _invalidate(self) -> None:
        """Drop everything queued and cut off what is playing."""
        with self._lock:
            self._token += 1
            token = self._token
        while True:
            try:
                self._jobs.get_nowait()
            except queue.Empty:
                break
        if self._engine is not None:
            try:
                self._engine.stop()
            except Exception:  # pragma: no cover - defensive
                log.exception("engine stop failed")
        if self._player is not None:
            try:
                self._player.stop()
            except Exception:  # pragma: no cover - defensive
                log.exception("player stop failed")
        self._set_state("idle")

    # -- public API (D-Bus main loop) ------------------------------------------

    def speak(self, text: str) -> None:
        """Speak an arbitrary string on demand (the SpeakText method)."""
        if not self.enabled or self._closed:
            return
        with self._lock:
            token = self._token
        self._enqueue_text(text, token)

    def stop(self) -> None:
        """Interrupt immediately: silence and drop the queue."""
        self._invalidate()

    def state(self) -> str:
        """Whether the speaker is sounding: "speaking" or "idle"."""
        with self._lock:
            return self._state

    def speech_state(self) -> dict:
        """The GetSpeechState answer: enabled plus speaking/idle."""
        return {"enabled": self.enabled, "state": self.state()}

    def close(self) -> None:
        """Stop and release everything (idempotent)."""
        self._closed = True
        self._invalidate()
        thread = self._thread
        if thread is not None and thread.is_alive():
            self._jobs.put(None)
            thread.join(timeout=2.0)
        if self._engine is not None:
            try:
                self._engine.close()
            except Exception:  # pragma: no cover - defensive
                log.exception("engine close failed")
        if self._player is not None:
            try:
                self._player.close()
            except Exception:  # pragma: no cover - defensive
                log.exception("player close failed")

    # -- internals ----------------------------------------------------------------

    def _enqueue_text(self, text: str, token: int) -> None:
        """Normalize + segment free text (final_text, SpeakText) and queue it."""
        try:
            chunks = segment(
                normalize_marked(text),
                min_length=self._min_chunk,
                max_length=self._max_chunk,
            )
        except Exception:  # pragma: no cover - normalization is total
            log.exception("speech normalization failed; text not spoken")
            return
        for chunk in chunks:
            self._enqueue(chunk, token)

    def _enqueue(self, chunk: str, token: int) -> None:
        self._ensure_thread()
        self._jobs.put((chunk, token))

    def _ensure_thread(self) -> None:
        with self._lock:
            if self._thread is None or not self._thread.is_alive():
                self._thread = threading.Thread(
                    target=self._work, name="phoenix-tts", daemon=True
                )
                self._thread.start()

    def _set_state(self, state: str) -> None:
        with self._lock:
            if self._state == state:
                return
            self._state = state
        try:
            self._emitter(events.make(events.SPEECH_STATE_CHANGED, state=state))
        except Exception:  # pragma: no cover - defensive
            log.exception("emitting speech state failed")

    def _work(self) -> None:
        while True:
            job = self._jobs.get()
            if job is None:
                return
            chunk, token = job
            with self._lock:
                if token != self._token or self._closed:
                    continue
            self._set_state("speaking")
            try:
                self._speak_chunk(chunk, token)
            except Exception:  # a broken chunk must not kill the thread
                log.exception("speaking %r failed", chunk[:80])
            with self._lock:
                current = self._token
            if not self._has_pending(current):
                # The buffered tail may still be draining through the
                # sink; pw-cat reports nothing back, so "idle" trails
                # by at most that sub-second tail.
                self._set_state("idle")

    def _has_pending(self, token: int) -> bool:
        with self._jobs.mutex:
            return any(
                job is not None and job[1] == token for job in self._jobs.queue
            )

    def _speak_chunk(self, chunk: str, token: int) -> None:
        assert self._engine is not None
        player = self._player
        started = False
        try:
            for pcm in self._engine.synthesize(chunk):
                with self._lock:
                    if token != self._token or self._closed:
                        return
                if not started and player is not None:
                    player.start(self._engine.sample_rate)
                    started = True
                if player is not None:
                    player.write(pcm)
        except PlaybackStopped:
            raise
        except Exception as exc:
            log.warning("synthesis failed: %s", exc)


def _sentenceish(buffer: str) -> bool:
    """Whether the buffer could contain a completed sentence yet."""
    return _SENTENCEISH.search(buffer) is not None


def build_speaker(config, emitter=None) -> Speaker:
    """The configured Speaker (disabled when tts_enabled is false).

    Never raises: without an engine (no key, unknown provider, feature
    off) the Speaker exists but is disabled, and every one of its entry
    points is a no-op — existing behaviour, untouched.
    """
    engine = build_engine(config) if config.tts_enabled else None
    player = Player(rate=32000) if engine is not None else None
    return Speaker(
        engine=engine,
        player=player,
        read_mode=config.tts_read_mode,
        holdback_sentences=config.tts_holdback_sentences,
        enabled=config.tts_enabled,
        emitter=emitter,
    )
