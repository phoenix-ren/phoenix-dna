# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Daemon-level tests: queueing, cancellation, root auth flow, sessions."""

from __future__ import annotations

import dataclasses

from conftest import wait_for

from phoenix_ai.daemon import AgentDaemon
from phoenix_ai.modes import Mode
from phoenix_ai.privileges import StaticPasswordProvider
from phoenix_ai.provider import FakeProvider


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def make_daemon(config, script, password_provider=None, events=None):
    sink = (lambda e: events.append(e)) if events is not None else (lambda e: None)
    return AgentDaemon(
        config,
        provider=FakeProvider(script),
        password_provider=password_provider,
        emitter=sink,
    )


def test_submit_runs_and_finishes(config):
    events = []
    daemon = make_daemon(config, ["All done."], events=events)
    try:
        run_id = daemon.submit("say something", "user")
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


def test_submit_validates_mode_and_prompt(config):
    daemon = make_daemon(config, ["x"])
    try:
        for bad_mode in ("bogus", "", "sudo"):
            try:
                daemon.submit("hi", bad_mode)
            except ValueError as exc:
                assert "unknown mode" in str(exc)
            else:
                raise AssertionError(f"mode {bad_mode!r} accepted")
        try:
            daemon.submit("   ", "user")
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
        first = daemon.submit("slow one", "user")
        second = daemon.submit("quick two", "user")
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
        first = daemon.submit("slow", "user")
        second = daemon.submit("never runs", "user")
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
        run_id = daemon.submit("long running", "user")
        assert wait_for(lambda: daemon.state()["state"] == "running", timeout=5)
        assert daemon.cancel(run_id) is True
        assert wait_for(
            lambda: events_of(events, "run_finished")
            and events_of(events, "run_finished")[0]["status"] == "cancelled",
            timeout=10,
        ), f"events={events}"
    finally:
        daemon.shutdown()


def test_root_auth_flow_and_multi_prompt_cache(config, home_dir, fake_sudo):
    events = []
    cfg = dataclasses.replace(config, sudo_bin=str(fake_sudo))
    # NOTE: root mode working dir is '/' by design; the scripted commands
    # below are read-only or echo-only, so they are safe anywhere.
    daemon = make_daemon(
        cfg,
        [
            {"tool": "bash", "args": {"command": "echo sudo-$FAKE_SUDO_OK"}},
            "root work complete",
            {"tool": "bash", "args": {"command": "echo sudo-$FAKE_SUDO_OK"}},
            "root work complete again",
        ],
        password_provider=StaticPasswordProvider(["hunter2"]),
        events=events,
    )
    try:
        run1 = daemon.submit("check sudo", "root")
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 1, timeout=10)
        assert events_of(events, "run_finished")[0]["status"] == "completed"

        # auth events were emitted exactly once
        assert len(events_of(events, "auth_requested")) == 1
        finished_auth = events_of(events, "auth_finished")
        assert len(finished_auth) == 1 and finished_auth[0]["success"] is True

        tool_outputs = [
            e["output"] for e in events_of(events, "tool_call_finished")
        ]
        assert any("sudo-1" in out for out in tool_outputs)

        # second root prompt in the same session: no new auth, still sudo
        session_id = events_of(events, "run_started")[0]["session_id"]
        run2 = daemon.submit("check again", "root", session_id=session_id)
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 2, timeout=10)
        assert len(events_of(events, "auth_requested")) == 1  # cache reused
        outputs2 = [
            e["output"]
            for e in events_of(events, "tool_call_finished")
        ]
        assert any("sudo-1" in out for out in outputs2)
        assert daemon._root_cache._password == "hunter2"
    finally:
        daemon.shutdown()


def test_set_mode_preauthenticates_root(config, home_dir, fake_sudo):
    """Selecting root pops the password dialog immediately and caches it."""
    events = []
    cfg = dataclasses.replace(config, sudo_bin=str(fake_sudo))
    daemon = make_daemon(
        cfg,
        [],
        password_provider=StaticPasswordProvider(["hunter2"]),
        events=events,
    )
    try:
        assert daemon.set_mode("root") == "root"
        assert daemon.mode() is Mode.ROOT

        # The pre-auth runs on its own thread; wait for the dialog flow.
        assert wait_for(lambda: events_of(events, "auth_finished"), timeout=5), (
            f"no auth_finished; events={events}"
        )
        assert len(events_of(events, "auth_requested")) == 1
        requested = events_of(events, "auth_requested")[0]
        assert requested["run_id"] == ""  # no run is in flight
        assert "root mode" in requested["message"]

        finished_auth = events_of(events, "auth_finished")
        assert len(finished_auth) == 1 and finished_auth[0]["success"] is True
        assert daemon._root_cache._password == "hunter2"

        # Leaving root drops the cached password.
        assert daemon.set_mode("user") == "user"
        assert daemon.mode() is Mode.USER
        assert daemon._root_cache._password is None
    finally:
        daemon.shutdown()


def test_set_mode_validates_and_normalizes(config):
    daemon = make_daemon(config, [])
    try:
        assert daemon.set_mode(" WEB ") == "web"
        try:
            daemon.set_mode("bogus")
        except ValueError as exc:
            assert "unknown mode" in str(exc)
        else:
            raise AssertionError("mode 'bogus' accepted")
    finally:
        daemon.shutdown()


def test_state_transitions_emitted(config):
    events = []
    daemon = make_daemon(config, ["done"], events=events)
    try:
        daemon.submit("hi", "user")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        states = [e["state"] for e in events_of(events, "state_changed")]
        assert "running" in states and "idle" in states
    finally:
        daemon.shutdown()
