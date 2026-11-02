# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Pure (no D-Bus) tests of the event -> signal mapping."""

from __future__ import annotations

from phoenix_dna import events
from phoenix_dna.service import SignalBridge


class FakeService:
    """Records emitter calls; stands in for the D-Bus object."""

    def __init__(self):
        self.calls = []

    def RunFinished(self, *args):
        self.calls.append(("RunFinished", args))

    def ThinkingDelta(self, *args):
        self.calls.append(("ThinkingDelta", args))

    def TranscriptCleaned(self, *args):
        self.calls.append(("TranscriptCleaned", args))

    def SessionTitleChanged(self, *args):
        self.calls.append(("SessionTitleChanged", args))


def test_cleanup_event_maps_to_transcript_cleaned():
    service = FakeService()
    bridge = SignalBridge({"transcript_cleaned": service.TranscriptCleaned})
    bridge.dispatch(
        events.make(
            events.CLEANUP_FINISHED,
            cleanup_id="cleanup_abc",
            source="Multiply five times twenty-five",
            cleaned="Multiply 5 x 25",
            status="completed",
        )
    )
    assert service.calls == [
        (
            "TranscriptCleaned",
            (
                "cleanup_abc",
                "Multiply five times twenty-five",
                "Multiply 5 x 25",
                "completed",
            ),
        )
    ]


def test_cleanup_event_defaults_blank_fields():
    service = FakeService()
    bridge = SignalBridge({"transcript_cleaned": service.TranscriptCleaned})
    bridge.dispatch({"kind": "cleanup_finished"})
    assert service.calls == [("TranscriptCleaned", ("", "", "", ""))]


def test_thinking_event_maps_to_thinking_delta():
    service = FakeService()
    bridge = SignalBridge({"thinking_delta": service.ThinkingDelta})
    bridge.dispatch(
        events.make(events.THINKING_DELTA, run_id="run_1", text="Let me look.")
    )
    assert service.calls == [("ThinkingDelta", ("run_1", "Let me look."))]


def test_unknown_event_kind_is_ignored():
    service = FakeService()
    bridge = SignalBridge({"transcript_cleaned": service.TranscriptCleaned})
    bridge.dispatch({"kind": "mystery"})
    assert service.calls == []


def test_title_event_maps_to_session_title_changed():
    service = FakeService()
    bridge = SignalBridge({"session_title_changed": service.SessionTitleChanged})
    bridge.dispatch(
        events.make(
            events.SESSION_TITLE_CHANGED,
            session_id="abc123",
            mode="user",
            title="First US President",
        )
    )
    assert service.calls == [
        ("SessionTitleChanged", ("abc123", "user", "First US President"))
    ]
