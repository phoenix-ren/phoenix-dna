# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The bash tool's process reports over a private D-Bus.

The daemon reports the command's session-leader pid with
ToolProcessStarted / ToolProcessFinished while a bash tool call runs, so
the UI can meter its CPU usage from /proc. These tests run one scripted
bash call through the whole stack: loop, tool, daemon queue, bridge,
dbus-python signal, separate client connection.
"""

from __future__ import annotations

import os
import signal
import subprocess
import threading
import time

import pytest


@pytest.fixture(scope="module")
def bus_address():
    result = subprocess.run(
        ["dbus-daemon", "--session", "--print-address", "--print-pid", "--fork"],
        capture_output=True,
        text=True,
        check=True,
    )
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

    tmp = tmp_path_factory.mktemp("dbus-process")
    config = Config(
        provider="fake",
        config_dir=tmp / "config",
        data_dir=tmp / "state",
    )
    daemon = AgentDaemon(
        config,
        # One turn that runs a real command, one turn that answers: the
        # loop calls the bash tool between them, which is what these
        # tests listen for.
        provider=FakeProvider(
            [
                {"tool": "bash", "args": {"command": "sleep 0.4"}},
                {"text": "the command ran"},
            ]
        ),
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


@pytest.fixture()
def client(service, bus_address):
    import dbus
    import dbus.bus

    from phoenix_dna.service import BUS_NAME, INTERFACE, OBJECT_PATH

    bus = dbus.bus.BusConnection(bus_address)
    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    return bus, dbus.Interface(proxy, INTERFACE)


def test_bash_call_reports_its_pid_over_dbus(client):
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    received = {"started": None, "finished": None, "call_started": None}
    done = threading.Event()

    def on_tool_started(run_id, call_id, tool, args_json):
        received["call_started"] = str(call_id)

    def on_process_started(run_id, call_id, command, pid):
        received["started"] = {
            "run_id": str(run_id),
            "call_id": str(call_id),
            "command": str(command),
            "pid": int(pid),
        }

    def on_process_finished(run_id, call_id, pid):
        received["finished"] = {
            "run_id": str(run_id),
            "call_id": str(call_id),
            "pid": int(pid),
        }
        done.set()

    bus.add_signal_receiver(
        on_tool_started, signal_name="ToolCallStarted", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_process_started, signal_name="ToolProcessStarted", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_process_finished,
        signal_name="ToolProcessFinished",
        dbus_interface=INTERFACE,
    )

    run_id = str(iface.SubmitPrompt("run the command", ""))
    assert run_id.startswith("run_")

    deadline = time.time() + 15
    while time.time() < deadline and received["call_started"] is None:
        time.sleep(0.05)
    assert received["call_started"] is not None, "ToolCallStarted never arrived"
    assert done.wait(15), "ToolProcessFinished never arrived"

    started, finished = received["started"], received["finished"]
    # The pid is the command's session leader: a real process that just
    # ran (and by now has ended — the C++ side samples /proc with it).
    assert started["pid"] > 1
    assert started["pid"] == finished["pid"]
    assert started["command"] == "sleep 0.4"
    # The report names the call it belongs to: exactly the tool call the
    # trail shows (ToolCallStarted's id), in the run that was submitted.
    assert started["call_id"] == received["call_started"]
    assert started["run_id"] == run_id
    assert finished["call_id"] == received["call_started"]
    assert finished["run_id"] == run_id


def test_the_turn_reports_its_wire_traffic(client):
    """The bytes of a turn travel over D-Bus, so the UI can meter them."""
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    received = []
    done = threading.Event()

    def on_wire_bytes(run_id, turn, bytes_sent, bytes_received):
        received.append(
            {
                "run_id": str(run_id),
                "turn": int(turn),
                "bytes_sent": int(bytes_sent),
                "bytes_received": int(bytes_received),
            }
        )
        done.set()

    bus.add_signal_receiver(
        on_wire_bytes, signal_name="WireBytes", dbus_interface=INTERFACE
    )

    run_id = str(iface.SubmitPrompt("talk to me", ""))
    assert done.wait(15), "WireBytes never arrived"

    traffic = received[-1]
    assert traffic["run_id"] == run_id
    # The first turn of the run.
    assert traffic["turn"] == 1
    # The counts are whole numbers of bytes, and the fake provider's
    # answer was not empty.
    assert traffic["bytes_received"] > 0
