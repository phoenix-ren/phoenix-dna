# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""D-Bus service: exposes AgentDaemon to the plasmoid.

Bus name / object path / interface:
    ai.phoenix.Agent  /ai/phoenix/Agent  ai.phoenix.Agent

The daemon runs prompts on a worker thread; events are pushed onto a queue
and drained on the GLib main loop, so all D-Bus traffic stays on the main
thread (dbus-python connections are not thread-safe).
"""

from __future__ import annotations

import json
import logging
import queue

log = logging.getLogger("phoenix.service")

BUS_NAME = "ai.phoenix.Agent"
OBJECT_PATH = "/ai/phoenix/Agent"
INTERFACE = "ai.phoenix.Agent"
_ERROR = "ai.phoenix.Agent.Error"

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
    "run_started": ("run_started", ("run_id", "mode", "session_id")),
    "text_delta": ("text_delta", ("run_id", "text")),
    "tool_call_started": (
        "tool_call_started",
        ("run_id", "call_id", "tool", "args_json"),
    ),
    "tool_call_finished": (
        "tool_call_finished",
        ("run_id", "call_id", "tool", "status", "output"),
    ),
    "auth_requested": ("auth_requested", ("run_id", "message")),
    "auth_finished": ("auth_finished", ("run_id", "success")),
    "state_changed": ("state_changed", ("state", "current_run_id", "queue_length")),
    "run_finished": (
        "run_finished",
        ("run_id", "status", "final_text", "turns", "tokens_in", "tokens_out"),
    ),
}


_DEFAULTS = {
    "run_id": "",
    "mode": "",
    "session_id": "",
    "call_id": "",
    "tool": "",
    "status": "",
    "output": "",
    "text": "",
    "final_text": "",
    "message": "",
    "state": "",
    "current_run_id": "",
    "queue_length": 0,
    "turns": 0,
    "tokens_in": 0,
    "tokens_out": 0,
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
            "tool_call_started": service.ToolCallStarted,
            "tool_call_finished": service.ToolCallFinished,
            "auth_requested": service.AuthRequested,
            "auth_finished": service.AuthFinished,
            "state_changed": service.StateChanged,
            "run_finished": service.RunFinished,
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

        @dbus.service.signal(INTERFACE, signature="sss")
        def RunStarted(self, run_id, mode, session_id): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def TextDelta(self, run_id, text): ...

        @dbus.service.signal(INTERFACE, signature="ssss")
        def ToolCallStarted(self, run_id, call_id, tool, args_json): ...

        @dbus.service.signal(INTERFACE, signature="sssss")
        def ToolCallFinished(self, run_id, call_id, tool, status, output): ...

        @dbus.service.signal(INTERFACE, signature="ss")
        def AuthRequested(self, run_id, message): ...

        @dbus.service.signal(INTERFACE, signature="sb")
        def AuthFinished(self, run_id, success): ...

        @dbus.service.signal(INTERFACE, signature="ssu")
        def StateChanged(self, state, current_run_id, queue_length): ...

        @dbus.service.signal(INTERFACE, signature="sssuuu")
        def RunFinished(
            self, run_id, status, final_text, turns, tokens_in, tokens_out
        ): ...

        # -- methods -----------------------------------------------------------

        @dbus.service.method(INTERFACE, in_signature="sss", out_signature="s")
        def SubmitPrompt(self, prompt, mode, session_id):
            try:
                return self._daemon.submit(str(prompt), str(mode), str(session_id))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )

        @dbus.service.method(INTERFACE, in_signature="s", out_signature="b")
        def Cancel(self, run_id):
            return self._daemon.cancel(str(run_id))

        @dbus.service.method(INTERFACE, in_signature="s", out_signature="s")
        def SetMode(self, mode):
            """Record the widget's mode selection (and pre-auth root)."""
            try:
                return self._daemon.set_mode(str(mode))
            except ValueError as exc:
                raise dbus.exceptions.DBusException(
                    f"{_ERROR}.InvalidArgument", str(exc)
                )

        @dbus.service.method(INTERFACE, in_signature="", out_signature="a(sss)")
        def GetModes(self):
            return [
                (p.mode.value, p.description, str(p.working_dir(self._daemon.config)))
                for p in self._daemon.modes()
            ]

        @dbus.service.method(INTERFACE, in_signature="", out_signature="(ssu)")
        def GetState(self):
            state = self._daemon.state()
            return (state["state"], state["current_run_id"], state["queue_length"])

        @dbus.service.method(INTERFACE, in_signature="", out_signature="s")
        def Ping(self):
            import phoenix_ai

            return phoenix_ai.__version__


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
            f"(reply={result}); is another phoenix-agent running?"
        )
    PhoenixService(daemon, bus)
    from gi.repository import GLib

    loop = GLib.MainLoop()

    def _quit(signum, frame):
        loop.quit()

    _signal.signal(_signal.SIGINT, _quit)
    _signal.signal(_signal.SIGTERM, _quit)
    log.info("phoenix-agent %s ready on D-Bus as %s", phoenix_version(), BUS_NAME)
    try:
        loop.run()
    finally:
        daemon.shutdown()


def phoenix_version() -> str:
    import phoenix_ai

    return phoenix_ai.__version__
