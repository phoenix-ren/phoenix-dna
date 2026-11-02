# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""AgentDaemon: transport-independent core behind the D-Bus service.

Responsibilities:
* accept submitted prompts (mode + optional session id) and run them
  sequentially on a worker thread;
* keep a follow-up queue (prompts spoken while the agent is busy are run,
  not rejected);
* own the root-mode sudo credential cache so authentication carries over
  across prompts ("multi-prompt root session");
* forward every agent event to ``emitter`` (called from the worker thread -
* the D-Bus layer marshals events onto the main loop).
"""

from __future__ import annotations

import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from uuid import uuid4

from . import events
from .config import Config
from .loop import AgentLoop
from .modes import MODE_PROFILES, Mode, ModeProfile, build_toolset
from .privileges import KDialogPasswordProvider, SudoCredentialCache
from .prompts import build_system_prompt, load_context_file
from .provider import LLMProvider, build_provider
from .sessions import SessionManager, SessionStore

log = logging.getLogger("phoenix.daemon")


@dataclass
class PendingPrompt:
    run_id: str
    prompt: str
    mode: Mode
    session_id: str


class AgentDaemon:
    def __init__(
        self,
        config: Config,
        *,
        provider: LLMProvider | None = None,
        password_provider=None,
        emitter=None,
    ):
        self.config = config
        self._provider = provider or build_provider(config)
        self._password_provider = password_provider  # None -> KDialog lazily
        self._emitter = emitter or (lambda event: None)
        self._sessions = SessionManager(SessionStore(config.data_dir / "sessions"))
        self._root_cache: SudoCredentialCache | None = None
        self._mode: Mode = Mode.USER

        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._pending: list[PendingPrompt] = []
        self._current: PendingPrompt | None = None
        self._current_cancel: threading.Event | None = None
        self._stop = False
        self._thread = threading.Thread(
            target=self._work, name="phoenix-dna-agent-worker", daemon=True
        )
        self._thread.start()

    # -- frontend API -------------------------------------------------------

    def set_emitter(self, emitter) -> None:
        self._emitter = emitter

    def _emit(self, event: dict) -> None:
        self._emitter(event)

    def modes(self) -> list[ModeProfile]:
        return list(MODE_PROFILES.values())

    def mode(self) -> Mode:
        """The mode the widget currently has selected."""
        return self._mode

    def set_mode(self, mode_name: str) -> str:
        """Record the widget's mode selection; returns the normalized name.

        Selecting root pre-authenticates straight away: the KDE password
        dialog appears when the mode button is pressed, not later in the
        middle of the first root prompt, and the validated credential is
        cached for the prompts that follow. Leaving root drops the cached
        password.
        """
        try:
            mode = Mode(mode_name.strip().lower())
        except ValueError:
            valid = ", ".join(m.value for m in Mode)
            raise ValueError(f"unknown mode '{mode_name}' (valid: {valid})") from None

        with self._lock:
            previous, self._mode = self._mode, mode

        if mode is Mode.ROOT:
            self._preauthenticate_root()
        elif previous is Mode.ROOT and self._root_cache is not None:
            self._root_cache.invalidate()
        return mode.value

    def submit(self, prompt: str, mode_name: str, session_id: str = "") -> str:
        try:
            mode = Mode(mode_name.strip().lower())
        except ValueError:
            valid = ", ".join(m.value for m in Mode)
            raise ValueError(f"unknown mode '{mode_name}' (valid: {valid})") from None
        prompt = (prompt or "").strip()
        if not prompt:
            raise ValueError("prompt must not be empty")
        job = PendingPrompt(
            run_id="run_" + uuid4().hex[:12],
            prompt=prompt,
            mode=mode,
            session_id=session_id.strip(),
        )
        with self._cond:
            self._pending.append(job)
            self._cond.notify_all()
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

    def state(self) -> dict:
        with self._lock:
            return {
                "state": "running" if self._current is not None else "idle",
                "current_run_id": self._current.run_id if self._current else "",
                "queue_length": len(self._pending),
            }

    def shutdown(self, timeout: float = 2.0) -> None:
        with self._cond:
            self._stop = True
            if self._current_cancel is not None:
                self._current_cancel.set()
            self._cond.notify_all()
        self._thread.join(timeout=timeout)

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

    def _execute(self, job: PendingPrompt) -> None:
        profile = MODE_PROFILES[job.mode]
        cancel = self._current_cancel

        credential_cache = None
        if profile.use_sudo:
            credential_cache = self._root_credential_cache()
            credential_cache.notifier.on_started = lambda: self._emitter(
                events.make(
                    events.AUTH_REQUESTED,
                    run_id=job.run_id,
                    message="Administrator password required",
                )
            )
            credential_cache.notifier.on_finished = lambda ok: self._emitter(
                events.make(events.AUTH_FINISHED, run_id=job.run_id, success=bool(ok))
            )

        tools = build_toolset(profile, self.config, credential_cache=credential_cache)
        conversation = self._sessions.get(job.mode.value, job.session_id)
        system_prompt = build_system_prompt(
            profile, extra_context=load_context_file(self.config)
        )

        loop = AgentLoop(
            run_id=job.run_id,
            mode_name=job.mode.value,
            provider=self._provider,
            tools=tools,
            system_prompt=system_prompt,
            conversation=conversation,
            session_manager=self._sessions,
            event_sink=self._emitter,
            max_turns=self.config.max_turns,
            cancel_event=cancel,
        )
        loop.run(job.prompt)

    def _preauthenticate_root(self) -> None:
        """Show the password dialog now, off the D-Bus main loop.

        Runs on its own thread because kdialog blocks for up to its timeout,
        and the GLib loop must keep serving D-Bus while the dialog is open.
        The events reuse the auth_requested/auth_finished kinds with an
        empty run_id (no run is in flight). The notifier callbacks are the
        cache's shared ones: a root prompt starting while this dialog is
        open re-wires them, so the events may be attributed to that run.
        """
        cache = self._root_credential_cache()
        cache.notifier.on_started = lambda: self._emit(
            events.make(
                events.AUTH_REQUESTED,
                run_id="",
                message="Administrator password required to enable root mode",
            )
        )
        cache.notifier.on_finished = lambda ok: self._emit(
            events.make(events.AUTH_FINISHED, run_id="", success=bool(ok))
        )
        threading.Thread(
            target=cache.get_password, name="phoenix-preauth", daemon=True
        ).start()

    def _root_credential_cache(self) -> SudoCredentialCache:
        # Created once and shared across runs: this is what makes a root
        # session "multi-prompt" - the password is asked for once.
        with self._lock:
            if self._root_cache is None:
                provider = self._password_provider or KDialogPasswordProvider(
                    kdialog_bin=self.config.kdialog_bin
                )
                self._root_cache = SudoCredentialCache(
                    provider,
                    sudo_bin=self.config.sudo_bin,
                    ttl_seconds=self.config.credential_ttl_seconds,
                )
            return self._root_cache
