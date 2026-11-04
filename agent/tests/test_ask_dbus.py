# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""End-to-end ask_user over D-Bus, on a private bus (like test_service_dbus):
QuestionAsked arrives, AnswerQuestion releases the blocked run, and the
answer travels back in QuestionAnswered."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import threading
import time

import pytest

pytest.importorskip("dbus", reason="python3-dbus not installed")

FORM_ARGS = {
    "questions": [
        {
            "question": "Which disk should the backup use?",
            "header": "Backup disk",
            "options": [
                {"label": "/dev/sda", "description": "the SSD"},
                {"label": "/dev/sdb", "description": "the spinning one"},
            ],
        },
        {
            "question": "Encrypt the backup?",
            "options": [{"label": "Yes"}, {"label": "No"}],
        },
    ],
    "goal": "backup setup",
}


@pytest.fixture(scope="module")
def bus_address():
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
    from phoenix_dna.provider import FakeProvider
    from phoenix_dna.service import BUS_NAME, PhoenixService

    tmp = tmp_path_factory.mktemp("dbus-ask")
    config = Config(
        provider="fake",
        config_dir=tmp / "config",
        data_dir=tmp / "state",
        ask_timeout_seconds=60.0,
    )
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(
            [
                {"tool": "ask_user", "args": FORM_ARGS},
                "Backing up to /dev/sdb.",
            ]
        ),
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

    # a second connection: the plasmoid is a separate process, and sharing
    # one connection between server and client deadlocks
    bus = dbus.bus.BusConnection(bus_address)
    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    return bus, dbus.Interface(proxy, INTERFACE)


def test_question_round_trip_over_dbus(client):
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    seen = {"asked": None, "answered": None}
    asked_event = threading.Event()
    answered_event = threading.Event()
    finished = threading.Event()

    def on_asked(run_id, ask_id, question_json):
        seen["asked"] = {
            "run_id": str(run_id),
            "ask_id": str(ask_id),
            "question": json.loads(str(question_json)),
        }
        asked_event.set()

    def on_answered(run_id, ask_id, ask_status, answer_json):
        seen["answered"] = {
            "run_id": str(run_id),
            "ask_id": str(ask_id),
            "ask_status": str(ask_status),
            "answer": json.loads(str(answer_json)),
        }
        answered_event.set()

    def on_finished(*_a):
        finished.set()

    bus.add_signal_receiver(
        on_asked, signal_name="QuestionAsked", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_answered, signal_name="QuestionAnswered", dbus_interface=INTERFACE
    )
    bus.add_signal_receiver(
        on_finished, signal_name="RunFinished", dbus_interface=INTERFACE
    )

    run_id = str(iface.SubmitPrompt("back up my disk", ""))
    assert asked_event.wait(15), "QuestionAsked signal never arrived"

    assert seen["asked"]["run_id"] == run_id
    assert seen["asked"]["ask_id"].startswith("ask_")
    form = seen["asked"]["question"]
    assert form["goal"] == "backup setup"
    assert form["questions"][0]["question"] == FORM_ARGS["questions"][0]["question"]
    assert form["questions"][0]["options"][0] == {
        "label": "/dev/sda",
        "description": "the SSD",
    }

    # an answer nobody asked for is refused; the question keeps waiting
    assert not bool(iface.AnswerQuestion("ask_nope", '{"answers": [{}]}'))
    assert not asked_event.is_set() or not finished.is_set()

    # the real answer releases the blocked run
    assert bool(
        iface.AnswerQuestion(
            seen["asked"]["ask_id"], '{"answers": [{"selected": [1]}, {"selected": [1]}]}'
        )
    )
    assert finished.wait(15), "RunFinished signal never arrived"
    assert answered_event.wait(5), "QuestionAnswered signal never arrived"

    assert seen["answered"]["ask_id"] == seen["asked"]["ask_id"]
    assert seen["answered"]["ask_status"] == "answered"
    assert seen["answered"]["answer"] == {
        "answers": [
            {"selected": ["/dev/sdb"], "text": ""},
            {"selected": ["No"], "text": ""},
        ]
    }

    # the run finished with the scripted post-answer reply
    deadline = time.time() + 5
    while time.time() < deadline:
        if str(iface.GetState()[0]) == "idle":
            break
        time.sleep(0.05)
    assert str(iface.GetState()[0]) == "idle"

    # and the settled question no longer accepts answers
    assert not bool(
        iface.AnswerQuestion(seen["asked"]["ask_id"], '{"answers": [{}, {}]}')
    )
