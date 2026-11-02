# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Transcript cleanup: the single-shot LLM pass over a finished dictation."""

from __future__ import annotations

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.events import CLEANUP_FINISHED
from phoenix_dna.prompts import build_cleanup_messages
from phoenix_dna.provider import FakeProvider


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def test_cleanup_messages_shape():
    messages = build_cleanup_messages("Multiply five times twenty-five")
    assert messages[0]["role"] == "system"
    assert messages[1] == {"role": "user", "content": "Multiply five times twenty-five"}
    # The prompt must teach the normalization the widget shows the user.
    assert "Multiply 5 x 25" in messages[0]["content"]


def test_disabled_without_cleanup_provider(config):
    """No dedicated provider -> submit_cleanup is a no-op returning ""."""
    events = []
    daemon = AgentDaemon(
        config, provider=FakeProvider(["answer"]), emitter=events.append
    )
    try:
        assert daemon.submit_cleanup("hello") == ""
        assert daemon.submit_cleanup("   ") == ""
        assert events_of(events, CLEANUP_FINISHED) == []
    finally:
        daemon.shutdown()


def test_cleanup_runs_and_emits_event(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer"]),
        cleanup_provider=FakeProvider(["Multiply 5 x 25"]),
        emitter=events.append,
    )
    try:
        cleanup_id = daemon.submit_cleanup("Multiply five times twenty-five")
        assert cleanup_id.startswith("cleanup_")
        assert wait_for(lambda: events_of(events, CLEANUP_FINISHED), timeout=5)
        event = events_of(events, CLEANUP_FINISHED)[0]
        assert event["cleanup_id"] == cleanup_id
        assert event["source"] == "Multiply five times twenty-five"
        assert event["cleaned"] == "Multiply 5 x 25"
        assert event["status"] == "completed"
    finally:
        daemon.shutdown()


def test_cleanup_strips_wrapper_quotes(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer"]),
        cleanup_provider=FakeProvider(['"Cleaned text."']),
        emitter=events.append,
    )
    try:
        assert daemon.submit_cleanup("cleaned text") != ""
        assert wait_for(lambda: events_of(events, CLEANUP_FINISHED), timeout=5)
        assert events_of(events, CLEANUP_FINISHED)[0]["cleaned"] == "Cleaned text."
    finally:
        daemon.shutdown()


def test_cleanup_error_reports_status_and_keeps_source(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["answer"]),
        cleanup_provider=FakeProvider([{"raise": "llm", "message": "boom"}]),
        emitter=events.append,
    )
    try:
        cleanup_id = daemon.submit_cleanup("some transcript")
        assert cleanup_id != ""
        assert wait_for(lambda: events_of(events, CLEANUP_FINISHED), timeout=5)
        event = events_of(events, CLEANUP_FINISHED)[0]
        assert event["status"] == "error"
        assert event["cleaned"] == ""  # frontend keeps the raw transcript
        assert event["source"] == "some transcript"
    finally:
        daemon.shutdown()


def test_cleanup_does_not_disturb_run_provider_script(config):
    """The cleanup uses its own provider: the run loop's script is untouched."""
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["first answer", "second answer"]),
        cleanup_provider=FakeProvider(["cleaned"]),
        emitter=events.append,
    )
    try:
        assert daemon.submit_cleanup("tidy me") != ""
        run_one = daemon.submit("one", "user")
        assert wait_for(
            lambda: any(
                e.get("run_id") == run_one and e.get("status") == "completed"
                for e in events_of(events, "run_finished")
            ),
            timeout=5,
        )
        # The first run consumed the first script item, not the cleanup's.
        assert events_of(events, "run_finished")[0]["final_text"] == "first answer"
    finally:
        daemon.shutdown()
