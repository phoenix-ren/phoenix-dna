# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""D-Bus service: exposes AgentDaemon to the plasmoid.

Bus name / object path / interface:
    dna.phoenix.Agent  /dna/phoenix/Agent  dna.phoenix.Agent

The daemon runs prompts on a worker thread; events are pushed onto a queue
and drained on the GLib main loop, so all D-Bus traffic stays on the main
thread (dbus-python connections are not thread-safe).
"""

from __future__ import annotations

import json
import logging
import queue

log = logging.getLogger("phoenix.service")

BUS_NAME = "dna.phoenix.Agent"
OBJECT_PATH = "/dna/phoenix/Agent"
INTERFACE = "dna.phoenix.Agent"
_ERROR = "dna.phoenix.Agent.Error"

try:
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
except ImportError:  # tests that do not touch D-Bus
    dbus = None


# --- event -> signal mapping (pure, testable without D-Bus) -----------------
#
# kind -> (emitter key, payload keys in order)

_SIGNAL_MAP = {
    "run_started": ("run_started", ("run_id", "session_id")),
    "text_delta": ("text_delta", ("run_id", "text")),
    "thinking_delta": ("thinking_delta", ("run_id", "text")),
    "tool_call_started": (
        "tool_call_started",
        ("run_id", "call_id", "tool", "args_json"),
    ),
    "tool_call_finished": (
        "tool_call_finished",
        ("run_id", "call_id", "tool", "status", "output"),
    ),
    "wire_bytes": (
        "wire_bytes",
        ("run_id", "turn", "bytes_sent", "bytes_received"),
    ),
    "tool_process_started": (
        "tool_process_started",
        ("run_id", "call_id", "command", "pid"),
    ),
    "tool_process_finished": (
        "tool_process_finished",
        ("run_id", "call_id", "pid"),
    ),
    "auth_requested": ("auth_requested", ("run_id", "message")),
    "auth_finished": ("auth_finished", ("run_id", "success")),
    "question_asked": ("question_asked", ("run_id", "ask_id", "question_json")),
    "question_answered": (
        "question_answered",
        ("run_id", "ask_id", "ask_status", "answer_json"),
    ),
    "state_changed": ("state_changed", ("state", "current_run_id", "queue_length")),
    "run_finished": (
        "run_finished",
        ("run_id", "status", "final_text", "turns", "tokens_in", "tokens_out"),
    ),
    "cleanup_finished": (
        "transcript_cleaned",
        ("cleanup_id", "source", "cleaned", "status"),
    ),
    "session_title_changed": (
        "session_title_changed",
        ("session_id", "title"),
    ),
    "speech_state_changed": ("speech_state_changed", ("state",)),
}


_DEFAULTS = {
    "run_id": "",
    "session_id": "",
    "call_id": "",
    "tool": "",
    "status": "",
    "output": "",
    "command": "",
    "pid": 0,
    "turn": 0,
    "bytes_sent": 0,
    "bytes_received": 0,
    "text": "",
    "final_text": "",
    "message": "",
    "state": "",
    "current_run_id": "",
    "queue_length": 0,
    "turns": 0,
    "tokens_in": 0,
    "tokens_out": 0,
    "cleanup_id": "",
    "source": "",
    "cleaned": "",
    "title": "",
    "ask_id": "",
    "ask_status": "",
    "question": {},
    "answer": {},
}


class SignalBridge:
    """Maps daemon event dicts onto named emitters (D-Bus signals or fakes)."""

    def __init__(self, emitters: dict):
        self.emitters = emitters

    def dispatch(self, event: dict) -> None:
        kind = event.get("kind")
        mapping = _SIGNAL_MAP.get(kind)
        if mapping is None:
            log.debug("no signal mapping for event %r", kind)
            return
        emitter_key, keys = mapping
        emitter = self.emitters.get(emitter_key)
        if emitter is None:
            return
        payload = {}
        for key in keys:
            if key == "args_json":
                payload[key] = json.dumps(event.get("args", {}), ensure_ascii=False)
            elif key == "question_json":
                payload[key] = json.dumps(
                    event.get("question", {}), ensure_ascii=False
                )
            elif key == "answer_json":
                payload[key] = json.dumps(
                    event.get("answer", {}), ensure_ascii=False
                )
            elif key == "success":
                payload[key] = bool(event.get("success", False))
            else:
                payload[key] = event.get(key, _DEFAULTS.get(key, ""))
        emitter(*[payload[k] for k in keys])


def build_bridge(service) -> SignalBridge:
    return SignalBridge(
        {
            "run_started": service.RunStarted,
            "text_delta": service.TextDelta,
            "thinking_delta": service.ThinkingDelta,
            "tool_call_started": service.ToolCallStarted,
            "tool_call_finished": service.ToolCallFinished,
            "wire_bytes": service.WireBytes,
            "tool_process_started": service.ToolProcessStarted,
            "tool_process_finished": service.ToolProcessFinished,
            "auth_requested": service.AuthRequested,
            "auth_finished": service.AuthFinished,
            "question_asked": service.QuestionAsked,
            "question_answered": service.QuestionAnswered,
            "state_changed": service.StateChanged,
            "run_finished": service.RunFinished,
            "cleanup_finished": service.TranscriptCleaned,
            "session_title_changed": service.SessionTitleChanged,
            "speech_state_changed": service.SpeechStateChanged,
        }
    )


if dbus is not None:

    class PhoenixService(dbus.service.Object):
        def __init__(self, daemon, bus=None):
            self._daemon = daemon
            self._queue: queue.Queue = queue.Queue()
            self._bridge = build_bridge(self)
            daemon.set_emitter(self._queue.put)
            bus = bus or dbus.SessionBus()
            super().__init__(conn=bus, object_path=OBJECT_PATH)
            GLib.timeout_add(50, self._drain)

        def _drain(self) -> bool:
            while True:
                try:
                    event = self._queue.get_nowait()
                except queue.Empty:
                    break
                try:
                    self._bridge.dispatch(event)
                except Exception:
                    log.exception("failed to emit event")
            return True  # keep the timeout source alive

        # -- signals ---------------------------------------------------------

        @dbus.service.signal(INTERFACE, signature="ss")
        def RunStarted(self, run_id, session_id): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def TextDelta(self, run_id, text): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def ThinkingDelta(self, run_id, text): ...

        @dbus.service.signal(INTERFACE, signature="ssss")
        def ToolCallStarted(self, run_id, call_id, tool, args_json): ...

        @dbus.service.signal(INTERFACE, signature="sssss")
        def ToolCallFinished(self, run_id, call_id, tool, status, output): ...

        # The wire traffic of one turn: the bytes of the request that went
        # out and the bytes of the answer that came in. The UI meters the
        # rate of the connection from these.
        @dbus.service.signal(INTERFACE, signature="suuu")
        def WireBytes(self, run_id, turn, bytes_sent, bytes_received): ...

        # The bash tool's process began: pid is the command's session
        # leader (= process-group id), which the UI meters via /proc
        # while the call runs.
        @dbus.service.signal(INTERFACE, signature="sssu")
        def ToolProcessStarted(self, run_id, call_id, command, pid): ...

        # That process ended — normally, by timeout or by cancellation.
        @dbus.service.signal(INTERFACE, signature="ssu")
        def ToolProcessFinished(self, run_id, call_id, pid): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def AuthRequested(self, run_id, message): ...

        @dbus.service.signal(INTERFACE, signature="sb")
        def AuthFinished(self, run_id, success): ...

        @dbus.service.signal(INTERFACE, signature="sss")
        def QuestionAsked(self, run_id, ask_id, question_json): ...

        @dbus.service.signal(INTERFACE, signature="ssss")
        def QuestionAnswered(self, run_id, ask_id, ask_status, answer_json): ...

        @dbus.service.signal(INTERFACE, signature="ssu")
        def StateChanged(self, state, current_run_id, queue_length): ...

        @dbus.service.signal(INTERFACE, signature="sssuuu")
        def RunFinished(
            self, run_id, status, final_text, turns, tokens_in, tokens_out
        ): ...

        @dbus.service.signal(INTERFACE, signature="ssss")
        def TranscriptCleaned(self, cleanup_id, source, cleaned, status): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def SessionTitleChanged(self, session_id, title): ...

        @dbus.service.signal(INTERFACE, signature="s")
        def SpeechStateChanged(self, state): ...

        # -- methods -----------------------------------------------------------

        @dbus.service.method(INTERFACE, in_signature="ss", out_signature="s")
        def SubmitPrompt(self, prompt, session_id):
            """Submit a prompt; an empty session id starts a new session."""
            try:
                return self._daemon.submit(str(prompt), str(session_id))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )

        @dbus.service.method(INTERFACE, in_signature="sss", out_signature="s")
        def SubmitPromptWithAttachments(self, prompt, session_id, attachments):
            """Submit a prompt together with the files attached to it.

            ``attachments`` is a JSON array of ``{"name", "path",
            "mimeType"}`` objects - the files the user added to the prompt
            in the widget. The daemon copies them into its own attachment
            directory and shows them to the model.
            """
            try:
                return self._daemon.submit(
                    str(prompt), str(session_id), str(attachments)
                )
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )

        @dbus.service.method(INTERFACE, in_signature="s", out_signature="s")
        def SubmitTranscriptCleanup(self, text):
            """Queue the single-shot cleanup of a transcribed clip.

            Returns the cleanup id the eventual TranscriptCleaned signal
            carries; empty when cleanup is not configured.
            """
            return self._daemon.submit_cleanup(str(text))

        @dbus.service.method(INTERFACE, in_signature="s", out_signature="b")
        def Cancel(self, run_id):
            return self._daemon.cancel(str(run_id))

        @dbus.service.method(INTERFACE, in_signature="ss", out_signature="b")
        def AnswerQuestion(self, ask_id, answer_json):
            """Answer a question the running prompt asked (see QuestionAsked).

            ``answer_json`` is ``{"selected": [...], "text": "..."}``:
            ``selected`` entries are 0-based option indices or exact
            labels, ``text`` is the user's own words (either may carry the
            answer alone). Returns False when the question is not pending
            any more, or the payload does not resolve against it - the
            question keeps waiting and the caller may retry.
            """
            return bool(self._daemon.answer_question(str(ask_id), str(answer_json)))

        @dbus.service.method(INTERFACE, in_signature="ss", out_signature="s")
        def SearchSessions(self, user, query):
            """The user's sessions whose summary name matches the query.

            Returns a JSON array of session summaries: ``id``, ``mode``,
            ``title``, ``summary`` (the name the session is listed
            under), ``preview``, ``createdAt``, ``updatedAt`` and
            ``messageCount``. The query is matched fuzzily against the
            summary names; an empty query lists every session of the
            user, newest first.
            """
            try:
                sessions = self._daemon.search_sessions(str(user), str(query))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )
            return json.dumps(sessions, ensure_ascii=False)

        @dbus.service.method(INTERFACE, in_signature="ss", out_signature="s")
        def GetSessionTranscript(self, user, session_id):
            """One session's queries and responses, as a JSON array.

            Every entry carries the message's ``role`` (``user`` or
            ``assistant``), its ``content`` and the ``ts`` it was stored
            with.
            """
            try:
                messages = self._daemon.session_transcript(str(user), str(session_id))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )
            return json.dumps(messages, ensure_ascii=False)

        @dbus.service.method(INTERFACE, in_signature="ss", out_signature="b")
        def DeleteSession(self, user, session_id):
            """Delete a session from the user's history.

            The session's queries and responses go with it, and so does
            the audit trail of the runs it holds. Returns whether there
            was a session to delete: False means it was already gone.
            """
            try:
                return bool(self._daemon.delete_session(str(user), str(session_id)))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )

        @dbus.service.method(INTERFACE, in_signature="", out_signature="(ssu)")
        def GetState(self):
            state = self._daemon.state()
            return (state["state"], state["current_run_id"], state["queue_length"])

        @dbus.service.method(INTERFACE, in_signature="", out_signature="s")
        def Ping(self):
            import phoenix_dna

            return phoenix_dna.__version__

        @dbus.service.method(INTERFACE, in_signature="s", out_signature="")
        def SpeakText(self, text):
            """Speak an arbitrary string on demand.

            The daemon normalizes and segments the text exactly as it
            does an answer (math included); StopSpeaking interrupts.
            A no-op when speech is disabled.
            """
            self._daemon.speak_text(str(text))

        @dbus.service.method(INTERFACE, in_signature="", out_signature="")
        def StopSpeaking(self):
            """Interrupt playback immediately and drop what is queued."""
            self._daemon.stop_speaking()

        @dbus.service.method(INTERFACE, in_signature="", out_signature="bs")
        def GetSpeechState(self):
            """Whether speech is enabled, and "speaking" or "idle".

            Two plain out-arguments (not a struct) so the plasmoid's
            QDBusPendingReply<bool, QString> reads them directly.
            """
            state = self._daemon.speech_state()
            return (bool(state["enabled"]), str(state["state"]))

        @dbus.service.method(INTERFACE, in_signature="", out_signature="")
        def ReloadSpeech(self):
            """Re-read the configuration and rebuild the speech pipeline.

            What the settings window asks for after saving: a flipped
            read-aloud toggle (or voice, or read mode) applies at once,
            without restarting the service — and disabling mid-utterance
            falls silent.
            """
            self._daemon.reload_speech()


def run_service(daemon) -> None:
    """Own the bus name and run until SIGINT/SIGTERM."""
    if dbus is None:
        raise RuntimeError("dbus-python is not available (dnf install python3-dbus)")
    DBusGMainLoop(set_as_default=True)
    import signal as _signal

    bus = dbus.SessionBus()
    result = bus.request_name(BUS_NAME)
    if result != dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER:
        raise RuntimeError(
            f"could not acquire bus name {BUS_NAME} "
            f"(reply={result}); is another phoenix-dna-agent running?"
        )
    PhoenixService(daemon, bus)
    from gi.repository import GLib

    loop = GLib.MainLoop()

    def _quit(signum, frame):
        loop.quit()

    _signal.signal(_signal.SIGINT, _quit)
    _signal.signal(_signal.SIGTERM, _quit)
    log.info("phoenix-dna-agent %s ready on D-Bus as %s", phoenix_version(), BUS_NAME)
    try:
        loop.run()
    finally:
        daemon.shutdown()


def phoenix_version() -> str:
    import phoenix_dna

    return phoenix_dna.__version__
