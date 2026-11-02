# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""D-Bus service integration test.

Requires a session bus. Run either inside your desktop session:

    pytest tests/test_service_dbus.py

or hermetically:

    dbus-run-session -- python -m pytest tests/test_service_dbus.py -q
"""

from __future__ import annotations

import os
import threading
import time

import pytest

pytest.importorskip("dbus", reason="python3-dbus not installed")

pytestmark = pytest.mark.skipif(
    not os.environ.get("DBUS_SESSION_BUS_ADDRESS"),
    reason="no session bus; run tests under: dbus-run-session -- python -m pytest",
)


@pytest.fixture(scope="module")
def service(tmp_path_factory):
    from dbus.mainloop.glib import DBusGMainLoop

    DBusGMainLoop(set_as_default=True)
    import dbus
    from gi.repository import GLib

    from phoenix_ai.config import Config
    from phoenix_ai.daemon import AgentDaemon
    from phoenix_ai.privileges import StaticPasswordProvider
    from phoenix_ai.provider import FakeProvider
    from phoenix_ai.service import BUS_NAME, PhoenixService

    tmp = tmp_path_factory.mktemp("dbus")
    config = Config(
        provider="fake",
        config_dir=tmp / "config",
        data_dir=tmp / "state",
    )
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["D-Bus fake answer, 42."]),
        # Empty provider: SetMode("root") pre-auth must never open a real
        # kdialog during the test run.
        password_provider=StaticPasswordProvider([]),
    )
    bus = dbus.SessionBus()
    bus.request_name(BUS_NAME)
    svc = PhoenixService(daemon, bus)
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    yield svc
    loop.quit()
    daemon.shutdown()
    bus.release_name(BUS_NAME)


@pytest.fixture()
def client(service):
    import dbus

    from phoenix_ai.service import BUS_NAME, INTERFACE, OBJECT_PATH

    # private=True: dbus.SessionBus() is a process-wide singleton; reusing
    # the server's connection for the client deadlocks (call_blocking vs.
    # dispatch on the same connection). The plasmoid is a separate process,
    # so this quirk is test-only.
    bus = dbus.SessionBus(private=True)
    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    return bus, dbus.Interface(proxy, INTERFACE)


def test_ping_and_version(client):
    _bus, iface = client
    assert iface.Ping() == "0.1.0"


def test_get_modes(client):
    _bus, iface = client
    modes = iface.GetModes()
    assert [m[0] for m in modes] == ["root", "user", "web"]
    assert all(isinstance(m[1], str) and m[1] for m in modes)
    assert modes[0][2] == "/"  # root working dir


def test_full_run_over_dbus(client):
    bus, iface = client
    from phoenix_ai.service import INTERFACE

    received = {"deltas": [], "finished": None, "tools": [], "auth": []}
    done = threading.Event()

    def on_delta(run_id, text):
        received["deltas"].append(text)

    def on_tool_started(run_id, call_id, tool, args_json):
        received["tools"].append(tool)

    def on_finished(run_id, status, final_text, turns, tin, tout):
        received["finished"] = {
            "run_id": str(run_id),
            "status": str(status),
            "final_text": str(final_text),
            "turns": int(turns),
        }
        done.set()

    bus.add_signal_receiver(on_delta, signal_name="TextDelta", dbus_interface=INTERFACE)
    bus.add_signal_receiver(
        on_tool_started, signal_name="ToolCallStarted", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_finished, signal_name="RunFinished", dbus_interface=INTERFACE
    )

    run_id = str(iface.SubmitPrompt("what is the answer?", "user", ""))
    assert run_id.startswith("run_")
    assert done.wait(15), "RunFinished signal never arrived"
    assert received["finished"]["status"] == "completed"
    assert received["finished"]["final_text"] == "D-Bus fake answer, 42."
    assert "".join(received["deltas"]) == "D-Bus fake answer, 42."

    # state settles back to idle
    deadline = time.time() + 5
    while time.time() < deadline:
        if str(iface.GetState()[0]) == "idle":
            break
        time.sleep(0.05)
    state, current_run, queue_len = iface.GetState()
    assert (str(state), str(current_run), int(queue_len)) == ("idle", "", 0)


def test_invalid_mode_raises_dbus_error(client):
    import dbus

    _bus, iface = client
    with pytest.raises(dbus.exceptions.DBusException, match="unknown mode"):
        iface.SubmitPrompt("hello", "bogus", "")


def test_set_mode_round_trip(client):
    _bus, iface = client
    assert iface.SetMode("root") == "root"
    assert iface.SetMode(" WEB ") == "web"  # normalized like SubmitPrompt


def test_set_mode_invalid_raises_dbus_error(client):
    import dbus

    _bus, iface = client
    with pytest.raises(dbus.exceptions.DBusException, match="unknown mode"):
        iface.SetMode("bogus")


def test_run_started_carries_session_id(client):
    bus, iface = client
    from phoenix_ai.service import INTERFACE

    seen = {}
    done = threading.Event()

    def on_started(run_id, mode, session_id):
        seen.update(run_id=str(run_id), mode=str(mode), session_id=str(session_id))

    def on_finished(*_a):
        done.set()

    bus.add_signal_receiver(
        on_started, signal_name="RunStarted", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_finished, signal_name="RunFinished", dbus_interface=INTERFACE
    )
    run_id = str(iface.SubmitPrompt("session probe", "web", ""))
    assert done.wait(15)
    assert seen["run_id"] == run_id
    assert seen["mode"] == "web"
    assert seen["session_id"]
