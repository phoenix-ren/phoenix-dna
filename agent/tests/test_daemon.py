# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Daemon-level tests: queueing, cancellation, the auth flow, sessions."""

from __future__ import annotations

import dataclasses
import threading

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.privileges import StaticPasswordProvider
from phoenix_dna.provider import FakeProvider


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def make_daemon(config, script, password_provider=None, events=None, provider=None):
    sink = (lambda e: events.append(e)) if events is not None else (lambda e: None)
    return AgentDaemon(
        config,
        provider=provider or FakeProvider(script),
        password_provider=password_provider,
        emitter=sink,
    )


def test_submit_runs_and_finishes(config):
    events = []
    daemon = make_daemon(config, ["All done."], events=events)
    try:
        run_id = daemon.submit("say something")
        assert run_id.startswith("run_")
        assert wait_for(
            lambda: events_of(events, "run_finished"), timeout=5
        ), f"no run_finished; events={events}"
        finished = events_of(events, "run_finished")[0]
        assert finished["status"] == "completed"
        assert finished["final_text"] == "All done."
        assert finished["run_id"] == run_id
        assert wait_for(lambda: daemon.state()["state"] == "idle")
    finally:
        daemon.shutdown()


def test_submit_validates_the_prompt(config):
    daemon = make_daemon(config, ["x"])
    try:
        try:
            daemon.submit("   ")
        except ValueError:
            pass
        else:
            raise AssertionError("empty prompt accepted")
    finally:
        daemon.shutdown()


def test_queue_runs_prompts_sequentially(config):
    events = []
    script = [
        {"sleep": 0.3, "text": "first answer"},
        "second answer",
    ]
    daemon = make_daemon(config, script, events=events)
    try:
        first = daemon.submit("slow one")
        second = daemon.submit("quick two")
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 2, timeout=10)
        statuses = {e["run_id"]: e["status"] for e in events_of(events, "run_finished")}
        assert statuses[first] == "completed"
        assert statuses[second] == "completed"
    finally:
        daemon.shutdown()


def test_cancel_queued_prompt(config):
    events = []
    daemon = make_daemon(
        config, [{"sleep": 1.0, "text": "slow answer"}, "queued answer"], events=events
    )
    try:
        first = daemon.submit("slow")
        second = daemon.submit("never runs")
        assert daemon.cancel(second) is True
        assert daemon.cancel("run_nope") is False
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=10)

        statuses = {e["run_id"]: e for e in events_of(events, "run_finished")}
        assert statuses[second]["status"] == "cancelled"
        assert wait_for(
            lambda: any(
                e["run_id"] == first and e["status"] == "completed"
                for e in events_of(events, "run_finished")
            ),
            timeout=10,
        )
    finally:
        daemon.shutdown()


def test_cancel_running_prompt(config):
    events = []
    daemon = make_daemon(config, [{"sleep": 1.5, "text": "late"}], events=events)
    try:
        run_id = daemon.submit("long running")
        assert wait_for(lambda: daemon.state()["state"] == "running", timeout=5)
        assert daemon.cancel(run_id) is True
        assert wait_for(
            lambda: events_of(events, "run_finished")
            and events_of(events, "run_finished")[0]["status"] == "cancelled",
            timeout=10,
        ), f"events={events}"
    finally:
        daemon.shutdown()


def test_admin_command_asks_for_the_password(config, home_dir, fake_sudo):
    """A command that calls sudo asks the user for their password, and
    the credential is reused by the commands that follow."""
    events = []
    cfg = dataclasses.replace(config, sudo_bin=str(fake_sudo))
    # NOTE: the scripted commands below are read-only or echo-only, so
    # they are safe anywhere.
    daemon = make_daemon(
        cfg,
        [
            {"tool": "bash", "args": {"command": "sudo echo sudo-$FAKE_SUDO_OK"}},
            "root work complete",
            {"tool": "bash", "args": {"command": "sudo echo sudo-$FAKE_SUDO_OK"}},
            "root work complete again",
        ],
        password_provider=StaticPasswordProvider(["hunter2"]),
        events=events,
    )
    try:
        run1 = daemon.submit("check sudo")
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 1, timeout=10)
        assert events_of(events, "run_finished")[0]["status"] == "completed"

        # auth events were emitted exactly once
        assert len(events_of(events, "auth_requested")) == 1
        finished_auth = events_of(events, "auth_finished")
        assert len(finished_auth) == 1 and finished_auth[0]["success"] is True

        tool_outputs = [e["output"] for e in events_of(events, "tool_call_finished")]
        assert any("sudo-1" in out for out in tool_outputs)

        # second prompt in the same session: no new auth, still sudo
        session_id = events_of(events, "run_started")[0]["session_id"]
        run2 = daemon.submit("check again", session_id=session_id)
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 2, timeout=10)
        assert len(events_of(events, "auth_requested")) == 1  # cache reused
        outputs2 = [e["output"] for e in events_of(events, "tool_call_finished")]
        assert any("sudo-1" in out for out in outputs2)
        assert daemon._credential_cache._password == "hunter2"
    finally:
        daemon.shutdown()


def test_plain_command_asks_for_nothing(config, home_dir):
    """A command that needs no privileges never asks for a password."""
    events = []
    daemon = make_daemon(
        config,
        [{"tool": "bash", "args": {"command": "echo plain"}}, "done"],
        password_provider=StaticPasswordProvider([]),
        events=events,
    )
    try:
        daemon.submit("say something")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=10)
        assert events_of(events, "auth_requested") == []
        assert events_of(events, "auth_finished") == []
    finally:
        daemon.shutdown()


def test_state_transitions_emitted(config):
    events = []
    daemon = make_daemon(config, ["done"], events=events)
    try:
        daemon.submit("hi")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        states = [e["state"] for e in events_of(events, "state_changed")]
        assert "running" in states and "idle" in states
    finally:
        daemon.shutdown()


def test_queue_length_announced_when_a_prompt_waits(config):
    """A prompt that waits its turn says so.

    The daemon runs one prompt at a time, so a prompt submitted while
    another run is busy waits in the queue. The widget opens its results
    pane when the daemon *starts* a run - which, for a queued prompt, is
    only when the run before it is done. The queue length is what tells
    the widget that a prompt is on its way but has not started yet, so
    the daemon announces it the moment the prompt joins the queue.
    """
    events = []
    # The first prompt blocks the worker until it is released, so the
    # second one is still waiting when the queue is inspected.
    release = threading.Event()

    class BlockingProvider:
        def stream(self, messages, tools, should_cancel=None):
            release.wait(timeout=5)
            yield {"type": "response", "response": {"choices": []}}

    daemon = make_daemon(config, ["done"], events=events, provider=BlockingProvider())
    try:
        daemon.submit("first")
        assert wait_for(lambda: events_of(events, "run_started"), timeout=5)

        daemon.submit("second")
        # The queue grew: the daemon says so without waiting for the
        # first run to end.
        assert wait_for(
            lambda: any(
                e.get("queue_length", 0) > 0 for e in events_of(events, "state_changed")
            ),
            timeout=5,
        ), "the queued prompt is announced"
        assert daemon.state()["queue_length"] == 1
    finally:
        release.set()
        daemon.shutdown()
