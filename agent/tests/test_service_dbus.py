# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""D-Bus service integration test.

The test runs on its own private D-Bus daemon, not on the session bus: the
production daemon (phoenix.service) owns ``dna.phoenix.Agent`` on the
session bus for as long as the desktop session runs, and the test must not
fight it for the name - nor receive its signals.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import threading
import time

import pytest

pytest.importorskip("dbus", reason="python3-dbus not installed")


@pytest.fixture(scope="module")
def bus_address():
    """The address of a private D-Bus daemon, private to this test run."""
    try:
        result = subprocess.run(
            ["dbus-daemon", "--session", "--print-address", "--print-pid", "--fork"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        pytest.skip(f"cannot start a private dbus-daemon: {exc}")
    if result.returncode != 0:
        pytest.skip(f"cannot start a private dbus-daemon: {result.stderr.strip()}")
    address, _, pid = result.stdout.strip().partition("\n")
    yield address.strip()
    try:
        os.kill(int(pid.strip()), signal.SIGTERM)
    except (OSError, ValueError):
        pass


@pytest.fixture(scope="module")
def service(bus_address, tmp_path_factory):
    from dbus.mainloop.glib import DBusGMainLoop

    DBusGMainLoop(set_as_default=True)
    import dbus
    import dbus.bus
    from gi.repository import GLib

    from phoenix_dna.config import Config
    from phoenix_dna.daemon import AgentDaemon
    from phoenix_dna.privileges import StaticPasswordProvider
    from phoenix_dna.provider import FakeProvider
    from phoenix_dna.service import BUS_NAME, PhoenixService

    tmp = tmp_path_factory.mktemp("dbus")
    config = Config(
        provider="fake",
        config_dir=tmp / "config",
        data_dir=tmp / "state",
    )
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["D-Bus fake answer, 42."]),
        # Empty provider: a command that asks for administrator rights
        # must never open a real kdialog during the test run.
        password_provider=StaticPasswordProvider([]),
    )
    bus = dbus.bus.BusConnection(bus_address)
    assert bus.request_name(BUS_NAME) == dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER
    svc = PhoenixService(daemon, bus)
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    yield svc
    loop.quit()
    daemon.shutdown()
    bus.release_name(BUS_NAME)


@pytest.fixture(scope="module")
def client(service, bus_address):
    import dbus
    import dbus.bus

    from phoenix_dna.service import BUS_NAME, INTERFACE, OBJECT_PATH

    # A second connection to the private bus: the plasmoid is a separate
    # process, and sharing one connection between server and client would
    # deadlock (call_blocking vs. dispatch on the same connection).
    #
    # One connection for the whole module, not one per test: a fresh
    # connection is created while the service's main loop runs in
    # another thread, and the two of them fight over the default GLib
    # main context - which crashes the interpreter now and then, in the
    # middle of the next test's setup. The tests share one connection
    # instead, and the signal receivers they add to it keep working:
    # each of them waits for its own event, and an event that is set
    # twice is still set.
    bus = dbus.bus.BusConnection(bus_address)
    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    return bus, dbus.Interface(proxy, INTERFACE)


def test_ping_and_version(client):
    _bus, iface = client
    assert iface.Ping() == "0.1.0"


def test_full_run_over_dbus(client):
    bus, iface = client
    from phoenix_dna.service import INTERFACE

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

    run_id = str(iface.SubmitPrompt("what is the answer?", ""))
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


def test_submit_prompt_with_attachments(client, tmp_path):
    """The prompt and the files attached to it travel over D-Bus."""
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    image = tmp_path / "shot.png"
    image.write_bytes(b"\x89PNG\r\n\x1a\n")
    payload = json.dumps(
        [{"name": "shot.png", "path": str(image), "mimeType": "image/png"}]
    )

    finished = threading.Event()

    def on_finished(*_a):
        finished.set()

    bus.add_signal_receiver(
        on_finished, signal_name="RunFinished", dbus_interface=INTERFACE
    )

    run_id = str(iface.SubmitPromptWithAttachments("what is this?", "", payload))
    assert run_id.startswith("run_")
    assert finished.wait(15), "RunFinished signal never arrived"


def test_run_started_carries_session_id(client):
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    seen = {}
    done = threading.Event()

    def on_started(run_id, session_id):
        seen.update(run_id=str(run_id), session_id=str(session_id))

    def on_finished(*_a):
        done.set()

    bus.add_signal_receiver(
        on_started, signal_name="RunStarted", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_finished, signal_name="RunFinished", dbus_interface=INTERFACE
    )
    run_id = str(iface.SubmitPrompt("session probe", ""))
    assert done.wait(15)
    assert seen["run_id"] == run_id
    assert seen["session_id"]


def test_search_sessions_over_dbus(client):
    """The sessions of a user are listed, and the query narrows them down."""
    bus, iface = client
    from phoenix_dna.sessions import _local_username

    user = _local_username()
    finished = threading.Event()
    bus.add_signal_receiver(
        lambda *_a: finished.set(), signal_name="RunFinished", dbus_interface=None
    )
    iface.SubmitPrompt("how much disk is left?", "")
    assert finished.wait(15)

    sessions = json.loads(str(iface.SearchSessions(user, "")))
    assert isinstance(sessions, list)
    summaries = [s["summary"] for s in sessions]
    assert any("how much disk is left?" in summary for summary in summaries)

    # The query is matched fuzzily against the summary names.
    matches = json.loads(str(iface.SearchSessions(user, "disk")))
    assert matches
    assert all("disk" in s["summary"].lower() for s in matches)
    assert json.loads(str(iface.SearchSessions(user, "zzz"))) == []


def test_session_transcript_over_dbus(client):
    """One session's queries and responses travel over D-Bus."""
    bus, iface = client
    from phoenix_dna.sessions import _local_username

    user = _local_username()
    finished = threading.Event()
    bus.add_signal_receiver(
        lambda *_a: finished.set(), signal_name="RunFinished", dbus_interface=None
    )
    iface.SubmitPrompt("transcript probe", "")
    assert finished.wait(15)

    sessions = json.loads(str(iface.SearchSessions(user, "transcript")))
    assert sessions
    session = sessions[0]

    messages = json.loads(str(iface.GetSessionTranscript(user, session["id"])))
    assert [m["role"] for m in messages] == ["user", "assistant"]
    assert messages[0]["content"] == "transcript probe"
    assert messages[1]["content"] == "D-Bus fake answer, 42."


def test_session_context_over_dbus(client):
    """What a session spent of the model's context window travels over D-Bus."""
    bus, iface = client
    from phoenix_dna.sessions import _local_username

    user = _local_username()
    finished = threading.Event()
    bus.add_signal_receiver(
        lambda *_a: finished.set(), signal_name="RunFinished", dbus_interface=None
    )
    iface.SubmitPrompt("context probe", "")
    assert finished.wait(15)

    sessions = json.loads(str(iface.SearchSessions(user, "context probe")))
    assert sessions
    session_id = sessions[0]["id"]

    context = json.loads(str(iface.GetSessionContext(user, session_id)))
    # The session's own shape...
    assert context["id"] == session_id
    assert context["userMessages"] == 1
    assert context["assistantMessages"] == 1
    # ...the tokens its run spent (the fake provider spends 11 in, 7 out)...
    assert context["tokensIn"] == 11
    assert context["tokensOut"] == 7
    assert context["totalTokens"] == 18
    # ...and the model in force with the size of its context window.
    assert context["contextLimit"] >= 0
    assert "provider" in context
    assert "model" in context

    # A session that is not there has no context to report.
    assert json.loads(str(iface.GetSessionContext(user, "no-such-session"))) == {}


def test_search_rejects_an_invalid_user(client):
    import dbus

    _bus, iface = client
    with pytest.raises(dbus.exceptions.DBusException, match="invalid user"):
        iface.SearchSessions("../etc", "")


def test_delete_session_over_dbus(client):
    """A session is deleted from the user's history, and stays deleted."""
    bus, iface = client
    from phoenix_dna.sessions import _local_username

    user = _local_username()
    finished = threading.Event()
    bus.add_signal_receiver(
        lambda *_a: finished.set(), signal_name="RunFinished", dbus_interface=None
    )
    iface.SubmitPrompt("doomed session", "")
    assert finished.wait(15)

    sessions = json.loads(str(iface.SearchSessions(user, "doomed")))
    assert sessions
    session_id = sessions[0]["id"]

    assert bool(iface.DeleteSession(user, session_id)) is True
    # The session is gone from the history...
    assert json.loads(str(iface.SearchSessions(user, "doomed"))) == []
    # ...and its transcript with it.
    assert json.loads(str(iface.GetSessionTranscript(user, session_id))) == []
    # Deleting it again says there was nothing to delete.
    assert bool(iface.DeleteSession(user, session_id)) is False


def test_delete_session_rejects_an_invalid_user(client):
    import dbus

    _bus, iface = client
    with pytest.raises(dbus.exceptions.DBusException, match="invalid user"):
        iface.DeleteSession("../etc", "whatever")


def test_rename_session_over_dbus(client):
    """A session can be named by the user, and the name is what it is listed under."""
    bus, iface = client
    from phoenix_dna.sessions import _local_username

    user = _local_username()
    finished = threading.Event()
    bus.add_signal_receiver(
        lambda *_a: finished.set(), signal_name="RunFinished", dbus_interface=None
    )
    iface.SubmitPrompt("session to rename", "")
    assert finished.wait(15)

    sessions = json.loads(str(iface.SearchSessions(user, "session to rename")))
    assert sessions
    session_id = sessions[0]["id"]

    entry = json.loads(str(iface.RenameSession(user, session_id, "My own name")))
    assert entry["id"] == session_id
    assert entry["title"] == "My own name"
    assert entry["summary"] == "My own name"

    # The list shows the session under its new name.
    listed = json.loads(str(iface.SearchSessions(user, "My own")))
    assert [s["id"] for s in listed] == [session_id]

    # A session that is not there cannot be named.
    assert str(iface.RenameSession(user, "missing", "A name")) == ""


def test_rename_session_rejects_an_invalid_user(client):
    import dbus

    _bus, iface = client
    with pytest.raises(dbus.exceptions.DBusException, match="invalid user"):
        iface.RenameSession("../etc", "whatever", "A name")
