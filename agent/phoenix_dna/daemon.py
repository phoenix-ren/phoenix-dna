# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""AgentDaemon: transport-independent core behind the D-Bus service.

Responsibilities:
* accept submitted prompts (with an optional session id) and run them
  sequentially on a worker thread;
* keep a follow-up queue (prompts spoken while the agent is busy are run,
  not rejected);
* own the sudo credential cache so a command that needs administrator
  rights asks for the password once and the credential carries over
  across prompts;
* clean up voice transcripts with a single LLM call on a separate auxiliary
  pool, so a slow cleanup never delays the prompt queue;
* (re)name every session with a single LLM call after a completed run, so
  a follow-on that shifts the conversation renames it;
* forward every agent event to ``emitter`` (called from the worker thread -
  the D-Bus layer marshals events onto the main loop);
* feed those same events to the optional Speaker (see speech/speaker.py),
  which reads the answers aloud. The Speaker is a constructor argument so
  tests can inject a fake; by default it is built from the config and is
  disabled unless ``tts_enabled`` is set, which keeps the no-TTS behaviour
  byte-for-byte what it always was.
"""

from __future__ import annotations

import concurrent.futures
import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from uuid import uuid4

from . import events
from .attachments import (
    parse_payload as parse_attachments,
    stage as stage_attachments,
    vision_enabled,
)
from .config import Config, load_config
from .loop import AgentLoop
from .privileges import KDialogPasswordProvider, SudoCredentialCache
from .prompts import (
    build_cleanup_messages,
    build_system_prompt,
    build_title_messages,
    load_context_file,
)
from .provider import LLMProvider, build_provider
from .search import match_sessions
from .sessions import SessionManager, SessionStore
from .speech.speaker import Speaker, build_speaker
from .toolset import build_toolset
from .tools.ask import AskCoordinator

log = logging.getLogger("phoenix.daemon")

#: The single auxiliary worker finishes within this window or the cleanup is
#: abandoned. Transcription itself is expected in a few seconds; the cleanup
#: is one short LLM round-trip and must not lag the overlay for long.
CLEANUP_TIMEOUT_SECONDS = 20.0


def _strip_wrapper_quotes(text: str) -> str:
    """Remove a single level of symmetric wrapping quotes, if present.

    Models occasionally wrap short answers in quotes despite instructions;
    the cleaned transcript goes straight into an editable field, so a quote
    pair that the model added (rather than the speaker having said it) must
    not survive.
    """
    for left, right in (('"', '"'), ("'", "'"), ("“", "”"), ("„", "“")):
        if len(text) >= 2 and text.startswith(left) and text.endswith(right):
            inner = text[1:-1].strip()
            if inner:
                return inner
    return text


def _close_speaker(speaker) -> None:
    """Retire a replaced speaker on the auxiliary pool; never raises."""
    try:
        speaker.close()
    except Exception:  # pragma: no cover - defensive
        log.exception("closing a replaced speaker failed")


@dataclass
class PendingPrompt:
    run_id: str
    prompt: str
    session_id: str
    #: The files the user attached to the prompt: the payload as the
    #: widget sent it (a JSON array, or the JSON text of one). They are
    #: staged into the daemon's attachment directory when the run starts.
    attachments: object = None


class AgentDaemon:
    def __init__(
        self,
        config: Config,
        *,
        provider: LLMProvider | None = None,
        cleanup_provider: LLMProvider | None = None,
        title_provider: LLMProvider | None = None,
        password_provider=None,
        emitter=None,
        speaker: Speaker | None = None,
    ):
        self.config = config
        self._provider = provider or build_provider(config)
        # Transcript cleanup and session titles are separate, optional
        # single-shot calls: dedicated providers (not the run provider) so
        # the scripted test provider's call sequence stays untouched.
        # None disables the respective feature.
        self._cleanup_provider = cleanup_provider
        self._title_provider = title_provider
        # One auxiliary worker serves both single-shot calls: a slow
        # cleanup or title must neither delay the prompt queue nor block
        # the D-Bus main loop.
        self._aux_pool = concurrent.futures.ThreadPoolExecutor(
            max_workers=1, thread_name_prefix="phoenix-aux"
        )
        self._password_provider = password_provider  # None -> KDialog lazily
        self._emitter = emitter or (lambda event: None)
        # The Speaker sees every event through _emit and reads the
        # answers aloud. Built from the config (disabled unless
        # tts_enabled) or injected — a test's fake keeps the call
        # sequence deterministic. Its state-change events come back
        # through this daemon's _emit, so they ride the same queue to
        # D-Bus as everything else.
        self._speaker: Speaker = speaker if speaker is not None else build_speaker(
            config, emitter=self._emit
        )
        # Pending ask_user questions: the blocked tool waits here while the
        # answer comes in over D-Bus (answer_question). One coordinator
        # serves every run - runs are sequential, one question at a time.
        self._asks = AskCoordinator(emit=self._emit)
        # One SQLite database per user (the local login name). The
        # legacy JSONL store, if present, is imported into it once.
        self._sessions = SessionManager(
            SessionStore(config.data_dir / "users", config.data_dir / "sessions")
        )
        # The sudo credential cache: what lets a command that needs
        # administrator rights ask the user for their password.
        self._credential_cache: SudoCredentialCache | None = None

        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._pending: list[PendingPrompt] = []
        self._current: PendingPrompt | None = None
        self._current_cancel: threading.Event | None = None
        # The tool call the worker is executing right now (empty between
        # calls). Tool execution is sequential, so the bash tool's process
        # events can name their call by reading this - the tool itself
        # never learns its call id.
        self._current_call_id = ""
        self._stop = False
        self._thread = threading.Thread(
            target=self._work, name="phoenix-dna-agent-worker", daemon=True
        )
        self._thread.start()

    # -- frontend API -------------------------------------------------------

    def set_emitter(self, emitter) -> None:
        self._emitter = emitter

    def _emit(self, event: dict) -> None:
        # The Speaker first: its retraction logic wants to see the event
        # before the UI renders it, and it never blocks (buffer + queue).
        # speech_state_changed comes back in here too and is ignored.
        self._speaker.handle_event(event)
        # Remember which tool call is in flight: the bash tool reports its
        # process through a notifier that only knows the pid, and the call
        # id is attached here (events flow through this funnel in order,
        # tool_call_started always preceding the process it announces).
        if event.get("kind") == events.TOOL_CALL_STARTED:
            self._current_call_id = event.get("call_id", "")
        elif event.get("kind") == events.TOOL_CALL_FINISHED:
            self._current_call_id = ""
        self._emitter(event)

    def speak_text(self, text: str) -> None:
        """Speak an arbitrary string on demand (the SpeakText D-Bus method).

        The text goes through the same normalization and segmentation
        as an answer, on the Speaker's own thread; an empty or absent
        speaker (TTS off) makes this a no-op.
        """
        self._speaker.speak(text)

    def stop_speaking(self) -> None:
        """Interrupt playback immediately (the StopSpeaking D-Bus method)."""
        self._speaker.stop()

    def speech_state(self) -> dict:
        """The GetSpeechState answer: {"enabled": bool, "state": str}."""
        return self._speaker.speech_state()

    def reload_speech(self) -> None:
        """Re-read the configuration and rebuild the speech pipeline.

        The settings window writes keys.json and asks for this, so
        flipping read-aloud takes effect at once — no service restart,
        and a disable mid-utterance falls silent through the old
        speaker's close. The fresh configuration replaces self.config
        wholesale; nothing else in it can have changed since the daemon
        started (the window writes only speech settings).
        """
        config = load_config(self.config.config_dir, self.config.data_dir)
        new_speaker = build_speaker(config, emitter=self._emit)
        old, self._speaker = self._speaker, new_speaker
        self.config = config
        if old is not None:
            # Off the D-Bus main loop: closing may join the worker and
            # kill the playback chain (bounded, but not instant).
            self._aux_pool.submit(_close_speaker, old)

    def submit(
        self,
        prompt: str,
        session_id: str = "",
        attachments=None,
    ) -> str:
        prompt = (prompt or "").strip()
        if not prompt:
            raise ValueError("prompt must not be empty")
        job = PendingPrompt(
            run_id="run_" + uuid4().hex[:12],
            prompt=prompt,
            session_id=session_id.strip(),
            attachments=attachments,
        )
        with self._cond:
            self._pending.append(job)
            self._cond.notify_all()
        # The queue grew: the prompt waits its turn if another run is
        # busy, and the widget shows that wait. The state is announced
        # here rather than only at the run's edges, so the count is
        # current the moment a prompt joins the queue.
        self._emit_state()
        return job.run_id

    def cancel(self, run_id: str) -> bool:
        with self._cond:
            if self._current is not None and self._current.run_id == run_id:
                if self._current_cancel is not None:
                    self._current_cancel.set()
                return True
            for i, job in enumerate(self._pending):
                if job.run_id == run_id:
                    del self._pending[i]
                    self._emit(
                        events.make(
                            events.RUN_FINISHED,
                            run_id=run_id,
                            status="cancelled",
                            final_text="cancelled before execution",
                            turns=0,
                            tokens_in=0,
                            tokens_out=0,
                        )
                    )
                    return True
        return False

    def answer_question(self, ask_id: str, answer_json: str) -> bool:
        """Answer a question the running prompt asked the user (ask_user).

        ``False`` when there is no such pending question (unknown id,
        already answered, timed out or cancelled) or the payload does not
        resolve against the question - the question keeps waiting, and
        the frontend can retry.
        """
        return self._asks.answer(ask_id, answer_json)

    def submit_cleanup(self, text: str) -> str:
        """Queue a single-shot LLM cleanup of a voice transcript.

        Returns a cleanup id the frontend pairs with the later
        ``cleanup_finished`` event; an empty id means cleanup is not
        configured (no dedicated provider) and nothing will run. Runs on
        the auxiliary pool: a slow cleanup must never delay the prompt
        queue, and the D-Bus method must not block the main loop.
        """
        text = (text or "").strip()
        if not text or self._cleanup_provider is None:
            return ""
        cleanup_id = "cleanup_" + uuid4().hex[:12]
        future = self._aux_pool.submit(self._run_cleanup, cleanup_id, text)
        future.add_done_callback(
            lambda f: f.exception()
            and log.error("cleanup task failed: %s", f.exception())
        )
        return cleanup_id

    def _run_cleanup(self, cleanup_id: str, text: str) -> None:
        """The single cleanup LLM call; emits exactly one event."""
        status, cleaned = "completed", ""
        try:
            messages = build_cleanup_messages(text)
            parts: list[str] = []
            for item in self._cleanup_provider.stream(messages, tools=[]):
                if item.get("type") == "text_delta":
                    parts.append(item["text"])
            cleaned = _strip_wrapper_quotes("".join(parts).strip())
            if not cleaned:
                status, cleaned = "error", ""
        except Exception as exc:  # defensive: cleanup must never crash the daemon
            log.warning("transcript cleanup %s failed: %s", cleanup_id, exc)
            status, cleaned = "error", ""
        self._emit(
            events.make(
                events.CLEANUP_FINISHED,
                cleanup_id=cleanup_id,
                source=text,
                cleaned=cleaned,
                status=status,
            )
        )

    def _update_title(self, session_id: str) -> None:
        """(Re)name a session from its conversation so far.

        Runs after every completed run, so a follow-on that pulls the
        conversation onto new ground renames the session (unlike most
        providers, whose titles are frozen at the first message). A
        failure keeps the previous title; naming must never disturb the
        conversation itself.
        """
        if self._title_provider is None or not session_id:
            return
        try:
            conversation = self._sessions.get(session_id)
            exchanges = [
                (
                    str(message.get("role") or ""),
                    (
                        message.get("content")
                        if isinstance(message.get("content"), str)
                        else ""
                    ),
                )
                for message in conversation.messages
                if message.get("role") in ("user", "assistant")
            ]
            parts: list[str] = []
            for item in self._title_provider.stream(
                build_title_messages(exchanges), tools=[]
            ):
                if item.get("type") == "text_delta":
                    parts.append(item["text"])
            # Collapse whitespace (models like to wrap titles in newlines),
            # drop wrapping quotes and a trailing period.
            title = _strip_wrapper_quotes(" ".join("".join(parts).split()))
            title = title.rstrip(".").strip()
            if not title:
                return
            self._sessions.set_title(conversation, title)
            self._emit(
                events.make(
                    events.SESSION_TITLE_CHANGED,
                    session_id=session_id,
                    title=title,
                )
            )
        except Exception as exc:  # defensive: naming must never crash the daemon
            log.warning("title update for session %s failed: %s", session_id, exc)

    def state(self) -> dict:
        with self._lock:
            return {
                "state": "running" if self._current is not None else "idle",
                "current_run_id": self._current.run_id if self._current else "",
                "queue_length": len(self._pending),
            }

    # -- session search ------------------------------------------------------

    def search_sessions(self, user: str, query: str = "") -> list[dict]:
        """The user's sessions whose summary name matches the query.

        Every session of the user, newest first, narrowed down to the
        ones whose name matches the query: the match is fuzzy, so "dt"
        finds "Disk tuning" and every further character drops the
        sessions that no longer match.
        """
        sessions = self._sessions.store.list_sessions(user)
        return match_sessions(query, sessions)

    def session_transcript(self, user: str, session_id: str) -> list[dict]:
        """The queries and responses of one session, oldest first."""
        return self._sessions.store.session_messages(user, session_id)

    def delete_session(self, user: str, session_id: str) -> bool:
        """Delete a session from the user's history.

        The session's queries and responses go with it, and so does the
        audit trail of the runs it holds: what is left behind would be
        an orphan nobody ever sees again. Returns whether there was a
        session to delete.
        """
        return self._sessions.delete(user, session_id)

    def session_context(self, user: str, session_id: str) -> dict:
        """One session's context window, as the context view shows it.

        The session's own numbers (see SessionStore.session_context)
        and the model's: which provider and model are in force, how
        much that model holds at once, and what the session's tokens
        cost.

        The cost is only known when the model is priced: the prices are
        the user's own (price_input and price_output), for a provider
        that quotes them per million tokens. Without them the view
        says the price is unknown rather than quoting a price of
        nothing.
        """
        context = self._sessions.store.session_context(user, session_id)
        if not context:
            return {}

        config = self.config
        limit = int(config.context_window or 0)
        total = int(context.get("totalTokens", 0))
        priced = bool(config.price_input or config.price_output)
        cost = 0.0
        if priced:
            cost = (
                int(context.get("tokensIn", 0)) * float(config.price_input)
                + int(context.get("tokensOut", 0)) * float(config.price_output)
                + int(context.get("cacheTokens", 0)) * float(config.price_input)
            ) / 1_000_000.0

        context.update(
            {
                "provider": config.llm_provider,
                "model": config.model,
                "contextLimit": limit,
                "usage": round(total / limit * 100.0, 1) if limit > 0 else 0.0,
                "priced": priced,
                # Rounded to keep the float out of the answer's noise,
                # but far enough down to keep a price of a fraction of
                # a cent: the view does the rounding that is seen.
                "cost": round(cost, 10),
            }
        )
        return context

    def shutdown(self, timeout: float = 2.0) -> None:
        with self._cond:
            self._stop = True
            if self._current_cancel is not None:
                self._current_cancel.set()
            self._cond.notify_all()
        self._thread.join(timeout=timeout)
        self._aux_pool.shutdown(wait=False, cancel_futures=True)
        self._speaker.close()

    # -- worker ----------------------------------------------------------------

    def _work(self) -> None:
        while True:
            with self._cond:
                while not self._stop and not self._pending:
                    self._cond.wait(timeout=0.5)
                if self._stop:
                    return
                job = self._pending.pop(0)
                self._current = job
                self._current_cancel = threading.Event()
            self._emit_state()
            try:
                self._execute(job)
            except Exception:
                log.exception("run %s crashed in daemon", job.run_id)
                self._emit(
                    events.make(
                        events.RUN_FINISHED,
                        run_id=job.run_id,
                        status="error",
                        final_text="internal daemon error",
                        turns=0,
                        tokens_in=0,
                        tokens_out=0,
                    )
                )
            finally:
                with self._cond:
                    self._current = None
                    self._current_cancel = None
                self._emit_state()

    def _emit_state(self) -> None:
        state = self.state()
        self._emit(
            events.make(
                events.STATE_CHANGED,
                state=state["state"],
                current_run_id=state["current_run_id"],
                queue_length=state["queue_length"],
            )
        )

    def _bash_process_notifier(self, run_id: str):
        """The callback the bash tool reports its processes through.

        Runs on the worker thread (inside the tool, around the
        subprocess); the event rides the same queue to D-Bus as
        everything else. The pid is the command's session leader, so the
        UI can meter the whole process group from /proc. The call id is
        whatever call is in flight - tool calls are sequential, so it is
        exactly this command's.
        """

        def notify(state: str, pid: int, command: str) -> None:
            kind = (
                events.TOOL_PROCESS_STARTED
                if state == "started"
                else events.TOOL_PROCESS_FINISHED
            )
            self._emit(
                events.make(
                    kind,
                    run_id=run_id,
                    call_id=self._current_call_id,
                    pid=int(pid),
                    command=command,
                )
            )

        return notify

    def _execute(self, job: PendingPrompt) -> None:
        cancel = self._current_cancel

        # The credential cache: a command that needs administrator rights
        # asks the user for their password through it (the KDE dialog),
        # and the validated password is reused for the commands that
        # follow.
        credential_cache = self._sudo_credential_cache()
        credential_cache.notifier.on_started = lambda: self._emit(
            events.make(
                events.AUTH_REQUESTED,
                run_id=job.run_id,
                message="Administrator password required",
            )
        )
        credential_cache.notifier.on_finished = lambda ok: self._emit(
            events.make(events.AUTH_FINISHED, run_id=job.run_id, success=bool(ok))
        )

        tools = build_toolset(
            self.config,
            credential_cache=credential_cache,
            run_id=job.run_id,
            ask_coordinator=self._asks,
            process_notifier=self._bash_process_notifier(job.run_id),
        )
        conversation = self._sessions.get(job.session_id)
        system_prompt = build_system_prompt(
            tool_names=tuple(tools),
            extra_context=load_context_file(self.config),
        )

        # The files the user attached to the prompt are copied into the
        # daemon's attachment directory: the agent's tools can read them,
        # and they stay put for the whole run.
        staged = stage_attachments(
            parse_attachments(job.attachments),
            self.config.data_dir / "attachments" / job.run_id,
        )

        loop = AgentLoop(
            run_id=job.run_id,
            provider=self._provider,
            tools=tools,
            system_prompt=system_prompt,
            conversation=conversation,
            session_manager=self._sessions,
            # _emit (not the bare emitter) so the Speaker sees the run's
            # events too — the read policy lives on that funnel.
            event_sink=self._emit,
            max_turns=self.config.max_turns,
            cancel_event=cancel,
            vision=vision_enabled(self.config),
        )
        summary = loop.run(job.prompt, staged)

        # A run that produced an answer advanced the conversation: its
        # title is regenerated from the session's new shape (async, on the
        # auxiliary pool — the title arrives as its own event). Cancelled
        # and failed runs name nothing: their half-conversations would
        # only produce misleading titles.
        if summary.get("status") in ("completed", "max_turns") and summary.get(
            "final_text"
        ):
            self._aux_pool.submit(self._update_title, conversation.id)

    def _sudo_credential_cache(self) -> SudoCredentialCache:
        # Created once and shared across runs: this is what makes a
        # privileged session "multi-prompt" - the password is asked for
        # once.
        with self._lock:
            if self._credential_cache is None:
                provider = self._password_provider or KDialogPasswordProvider(
                    kdialog_bin=self.config.kdialog_bin
                )
                self._credential_cache = SudoCredentialCache(
                    provider,
                    sudo_bin=self.config.sudo_bin,
                    ttl_seconds=self.config.credential_ttl_seconds,
                )
            return self._credential_cache
