# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session titles: LLM-named sessions that follow the conversation."""

from __future__ import annotations

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.events import SESSION_TITLE_CHANGED
from phoenix_dna.prompts import (
    _TITLE_LINE_LIMIT,
    _TITLE_MAX_LINES,
    build_title_messages,
)
from phoenix_dna.provider import FakeProvider


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def title_of(daemon, conversation):
    return daemon._sessions.title(conversation)


def test_title_messages_truncate_and_cap():
    exchanges = [("user", "x" * 1000), ("assistant", ""), ("assistant", "y" * 500)]
    messages = build_title_messages(exchanges)
    assert messages[1]["content"].count("\n") == 1  # empty content dropped
    lines = messages[1]["content"].split("\n")
    assert all(len(line) <= len("assistant: ") + _TITLE_LINE_LIMIT for line in lines)

    many = [("user", f"msg {i}") for i in range(100)]
    messages = build_title_messages(many)
    assert len(messages[1]["content"].split("\n")) == _TITLE_MAX_LINES
    assert messages[1]["content"].startswith("user: msg 76")  # newest kept


def test_title_messages_empty_conversation():
    messages = build_title_messages([])
    assert messages[1]["content"] == "(empty conversation)"


def test_session_titled_after_first_run(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["Short answer."]),
        title_provider=FakeProvider(["US Presidents"]),
        emitter=events.append,
    )
    try:
        daemon.submit("who was the first US president", "user")
        assert wait_for(lambda: events_of(events, SESSION_TITLE_CHANGED), timeout=5)
        event = events_of(events, SESSION_TITLE_CHANGED)[0]
        assert event["title"] == "US Presidents"
        assert event["session_id"]  # a real session was named

        # The title is persisted in the session row.
        conversation = daemon._sessions.get("user", event["session_id"])
        assert title_of(daemon, conversation) == "US Presidents"
    finally:
        daemon.shutdown()


def test_title_updates_on_follow_on(config):
    """A follow-on that shifts the ground renames the session."""
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer one", "answer two"]),
        title_provider=FakeProvider(["First Topic", "Second Topic"]),
        emitter=events.append,
    )
    try:
        run = daemon.submit("first question", "user")
        assert wait_for(lambda: events_of(events, SESSION_TITLE_CHANGED), timeout=5)
        session_id = events_of(events, "run_started")[0]["session_id"]

        daemon.submit("second question", "user", session_id=session_id)
        assert wait_for(
            lambda: len(events_of(events, SESSION_TITLE_CHANGED)) >= 2, timeout=5
        )
        titles = [e["title"] for e in events_of(events, SESSION_TITLE_CHANGED)]
        assert titles == ["First Topic", "Second Topic"]

        conversation = daemon._sessions.get("user", session_id)
        assert title_of(daemon, conversation) == "Second Topic"
        assert run.startswith("run_")
    finally:
        daemon.shutdown()


def test_no_titles_without_provider(config):
    events = []
    daemon = AgentDaemon(
        config, provider=FakeProvider(["answer"]), emitter=events.append
    )
    try:
        daemon.submit("hi", "user")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        assert events_of(events, SESSION_TITLE_CHANGED) == []
    finally:
        daemon.shutdown()


def test_failed_title_keeps_previous(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer one", "answer two"]),
        title_provider=FakeProvider(
            ["Good Title", {"raise": "llm", "message": "boom"}]
        ),
        emitter=events.append,
    )
    try:
        daemon.submit("one", "user")
        assert wait_for(lambda: events_of(events, SESSION_TITLE_CHANGED), timeout=5)
        session_id = events_of(events, SESSION_TITLE_CHANGED)[0]["session_id"]

        daemon.submit("two", "user", session_id=session_id)
        assert wait_for(lambda: len(events_of(events, "run_finished")) >= 2, timeout=5)
        assert wait_for(
            lambda: len(events_of(events, SESSION_TITLE_CHANGED)) == 1, timeout=1
        )
        conversation = daemon._sessions.get("user", session_id)
        assert title_of(daemon, conversation) == "Good Title"  # previous kept
    finally:
        daemon.shutdown()


def test_cancelled_run_names_nothing(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider([{"sleep": 1.0, "text": "late"}]),
        title_provider=FakeProvider(["Should Not Happen"]),
        emitter=events.append,
    )
    try:
        run = daemon.submit("slow", "user")
        assert wait_for(lambda: daemon.state()["state"] == "running", timeout=5)
        daemon.cancel(run)
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        session_id = events_of(events, "run_started")[0]["session_id"]
        conversation = daemon._sessions.get("user", session_id)
        assert title_of(daemon, conversation) == ""
        assert events_of(events, SESSION_TITLE_CHANGED) == []
    finally:
        daemon.shutdown()


def test_title_collapses_whitespace_and_wrappers(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer"]),
        title_provider=FakeProvider(["\n  A Wrapped Title. \n"]),
        emitter=events.append,
    )
    try:
        daemon.submit("hi", "user")
        assert wait_for(lambda: events_of(events, SESSION_TITLE_CHANGED), timeout=5)
        assert events_of(events, SESSION_TITLE_CHANGED)[0]["title"] == "A Wrapped Title"
    finally:
        daemon.shutdown()
