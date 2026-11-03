# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for the Speaker: events in, synthesized sentences out.

A fake engine and a fake player keep these tests offline; the event
sequences replay exactly what AgentLoop emits (see loop.py): a turn
that called tools streams its text first, then TOOL_CALL_STARTED — the
"this turn was not the final answer" signal.
"""

from __future__ import annotations

import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from phoenix_dna import events  # noqa: E402
from phoenix_dna.speech.speaker import Speaker  # noqa: E402
from conftest import wait_for  # noqa: E402


class FakeEngine:
    """Records what would be synthesized; yields a fixed PCM chunk."""

    sample_rate = 32000

    def __init__(self, delay: float = 0.0):
        self.synthesized: list[str] = []
        self.stops = 0
        self._delay = delay

    def synthesize(self, text: str):
        self.synthesized.append(text)
        if self._delay:
            threading.Event().wait(self._delay)
        yield b"\x01\x00\x00\x01"

    def voices(self):
        return ["fake"]

    def stop(self):
        self.stops += 1

    def close(self):
        pass


class FakePlayer:
    def __init__(self):
        self.written: list[bytes] = []
        self.stops = 0
        self.started: list[int] = []

    def start(self, rate: int):
        self.started.append(rate)

    def write(self, pcm: bytes):
        self.written.append(pcm)

    def stop(self):
        self.stops += 1

    def close(self):
        pass


def make_speaker(engine=None, player=None, **kwargs) -> Speaker:
    engine = engine or FakeEngine()
    player = player or FakePlayer()
    return Speaker(engine=engine, player=player, enabled=True, **kwargs)


def replay(speaker: Speaker, event: dict) -> None:
    speaker.handle_event(event)


def run_events(run_id: str, turns: list[list[dict]], **finish) -> list[dict]:
    """An event stream: RUN_STARTED, per-turn events, RUN_FINISHED."""
    stream = [events.make(events.RUN_STARTED, run_id=run_id, mode="user", session_id="s")]
    for index, turn in enumerate(turns, start=1):
        stream.append(events.make(events.TURN_STARTED, run_id=run_id, turn=index))
        stream.extend(turn)
    stream.append(
        events.make(
            events.RUN_FINISHED,
            run_id=run_id,
            turns=len(turns),
            tokens_in=0,
            tokens_out=0,
            **finish,
        )
    )
    return stream


# -- final mode (the default) -------------------------------------------------


def test_final_mode_speaks_only_final_text():
    engine, player = FakeEngine(), FakePlayer()
    speaker = make_speaker(engine, player, read_mode="final")
    for event in run_events(
        "run_1",
        [
            [
                events.make(events.TEXT_DELTA, run_id="run_1", text="Let me check. "),
                events.make(events.TOOL_CALL_STARTED, run_id="run_1", call_id="c1", tool="bash", args={}),
                events.make(events.TOOL_CALL_FINISHED, run_id="run_1", call_id="c1", tool="bash", status="success", output="", details={}),
            ],
            [events.make(events.TEXT_DELTA, run_id="run_1", text="The disk is 80% full.")],
        ],
        status="completed",
        final_text="The disk is 80% full.",
    ):
        replay(speaker, event)
    assert wait_for(lambda: engine.synthesized == ["The disk is 80% full."])
    speaker.close()


def test_reasoning_and_tool_output_never_spoken():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    for event in run_events(
        "run_1",
        [
            [
                events.make(events.THINKING_DELTA, run_id="run_1", text="checking "),
                events.make(events.THINKING_DELTA, run_id="run_1", text="disk"),
                events.make(events.TOOL_CALL_STARTED, run_id="run_1", call_id="c1", tool="bash", args={}),
                events.make(
                    events.TOOL_CALL_FINISHED,
                    run_id="run_1",
                    call_id="c1",
                    tool="bash",
                    status="success",
                    output="Filesystem  Size  Used  Avail",
                    details={},
                ),
            ],
            [events.make(events.TEXT_DELTA, run_id="run_1", text="Done.")],
        ],
        status="completed",
        final_text="Done.",
    ):
        replay(speaker, event)
    assert wait_for(lambda: len(engine.synthesized) == 1)
    assert engine.synthesized == ["Done."]
    speaker.close()


def test_final_mode_cancelled_speaks_nothing():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    for event in run_events(
        "run_1",
        [[events.make(events.TEXT_DELTA, run_id="run_1", text="Half an answer.")]],
        status="cancelled",
        final_text="",
    ):
        replay(speaker, event)
    speaker.close()
    assert engine.synthesized == []


def test_final_mode_error_speaks_error_once():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    for event in run_events(
        "run_1", [], status="error", final_text="error: upstream broke"
    ):
        replay(speaker, event)
    assert wait_for(lambda: engine.synthesized == ["error: upstream broke"])
    speaker.close()


def test_final_mode_max_turns_speaks_final_text():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    for event in run_events(
        "run_1",
        [[events.make(events.TOOL_CALL_STARTED, run_id="run_1", call_id="c1", tool="bash", args={})]],
        status="max_turns",
        final_text="Stopped: too many tool turns without an answer.",
    ):
        replay(speaker, event)
    assert wait_for(
        lambda: engine.synthesized == ["Stopped: too many tool turns without an answer."]
    )
    speaker.close()


# -- stream mode ---------------------------------------------------------------


def test_stream_mode_speaks_before_run_finishes():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="stream", holdback_sentences=1)
    text = (
        "First sentence is long enough on its own. "
        "Second sentence follows it here as well. "
        "Third sentence closes the answer nicely."
    )
    replay(speaker, events.make(events.RUN_STARTED, run_id="r", mode="user", session_id="s"))
    replay(speaker, events.make(events.TURN_STARTED, run_id="r", turn=1))
    replay(speaker, events.make(events.TEXT_DELTA, run_id="r", text=text))
    # Three sentences exist; the third may still grow, the second is
    # held back — so the first plays while the run is still going.
    assert wait_for(
        lambda: engine.synthesized == ["First sentence is long enough on its own."]
    )
    replay(
        speaker,
        events.make(events.RUN_FINISHED, run_id="r", status="completed", final_text=text, turns=1, tokens_in=0, tokens_out=0),
    )
    assert wait_for(lambda: len(engine.synthesized) == 3)
    assert engine.synthesized[1:] == [
        "Second sentence follows it here as well.",
        "Third sentence closes the answer nicely.",
    ]
    speaker.close()


def test_stream_mode_retracts_turn_that_called_tools():
    engine = FakeEngine()
    player = FakePlayer()
    speaker = make_speaker(engine, player, read_mode="stream", holdback_sentences=1)
    replay(speaker, events.make(events.RUN_STARTED, run_id="r", mode="user", session_id="s"))
    replay(speaker, events.make(events.TURN_STARTED, run_id="r", turn=1))
    preamble = (
        "Let me check the disk usage now. "
        "This preamble sentence keeps going. "
        "And a third sentence still rambles on."
    )
    replay(speaker, events.make(events.TEXT_DELTA, run_id="r", text=preamble))
    # The first preamble sentence plays (holdback keeps the last two).
    assert wait_for(lambda: engine.synthesized == ["Let me check the disk usage now."])
    assert wait_for(lambda: player.written)
    # …and then the turn turns out to have called tools: cut it off.
    replay(
        speaker,
        events.make(events.TOOL_CALL_STARTED, run_id="r", call_id="c1", tool="bash", args={}),
    )
    assert wait_for(lambda: player.stops >= 1)
    assert engine.synthesized == ["Let me check the disk usage now."]  # no more
    for event in run_events("r2", [], status="completed", final_text="The answer."):
        replay(speaker, event)
    # Retracted sentence never finished; the next run speaks its answer.
    assert wait_for(lambda: engine.synthesized[-1] == "The answer.")
    speaker.close()


def test_stream_mode_flushes_held_tail_on_finish():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="stream", holdback_sentences=1)
    text = (
        "One sentence that is long enough alone. "
        "A second one to hold back briefly. "
        "A third completes the whole answer."
    )
    for event in run_events(
        "r",
        [[events.make(events.TEXT_DELTA, run_id="r", text=text)]],
        status="completed",
        final_text=text,
    ):
        replay(speaker, event)
    assert wait_for(
        lambda: engine.synthesized
        == [
            "One sentence that is long enough alone.",
            "A second one to hold back briefly.",
            "A third completes the whole answer.",
        ]
    )
    speaker.close()


# -- lifecycle -----------------------------------------------------------------


def test_stop_interrupts_mid_stream():
    engine = FakeEngine(delay=0.2)
    player = FakePlayer()
    speaker = make_speaker(engine, player, read_mode="final")
    replay(
        speaker,
        events.make(events.RUN_FINISHED, run_id="r", status="completed", final_text="Spoken until interrupted.", turns=1, tokens_in=0, tokens_out=0),
    )
    assert wait_for(lambda: engine.synthesized == ["Spoken until interrupted."])
    speaker.speak("This utterance dies mid-flight.")
    assert wait_for(lambda: len(engine.synthesized) == 2)
    speaker.stop()
    assert wait_for(lambda: player.stops >= 1)
    assert wait_for(lambda: speaker.state() == "idle")
    speaker.close()


def test_new_run_stops_stale_speech():
    engine = FakeEngine(delay=0.2)
    player = FakePlayer()
    speaker = make_speaker(engine, player, read_mode="final")
    speaker.speak("The old answer is still talking.")
    assert wait_for(lambda: engine.synthesized)
    replay(speaker, events.make(events.RUN_STARTED, run_id="r2", mode="user", session_id="s"))
    assert wait_for(lambda: player.stops >= 1)
    speaker.close()


def test_speak_text_ad_hoc():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    speaker.speak("Read this on demand, please. " * 3)
    assert wait_for(lambda: engine.synthesized == ["Read this on demand, please. Read this on demand, please. Read this on demand, please."])
    speaker.close()


def test_state_changes_emitted():
    states: list[dict] = []

    def emitter(event):
        states.append(event)

    engine = FakeEngine(delay=0.05)
    speaker = Speaker(engine=engine, player=FakePlayer(), enabled=True, emitter=emitter)
    speaker.speak("Something to say out loud that takes a moment to finish.")
    speaker.speak("And one more utterance follows right after the first one.")
    # The state starts "idle"; the utterances must first move it to
    # "speaking" and only then settle back.
    assert wait_for(lambda: speaker.state() == "speaking")
    assert wait_for(lambda: speaker.state() == "idle")
    kinds = [e for e in states if e.get("kind") == events.SPEECH_STATE_CHANGED]
    assert [e["state"] for e in kinds][:1] == ["speaking"]
    assert kinds[-1]["state"] == "idle"
    speaker.close()


def test_disabled_speaker_is_silent():
    engine = FakeEngine()
    speaker = Speaker(engine=engine, player=FakePlayer(), enabled=False)
    for event in run_events(
        "r", [[events.make(events.TEXT_DELTA, run_id="r", text="text")]], status="completed", final_text="final"
    ):
        speaker.handle_event(event)
    speaker.speak("nothing")
    speaker.close()
    assert engine.synthesized == []
    assert speaker.speech_state() == {"enabled": False, "state": "idle"}


def test_math_in_final_text_spoken_not_read_as_markup():
    engine = FakeEngine()
    speaker = make_speaker(engine, read_mode="final")
    replay(
        speaker,
        events.make(
            events.RUN_FINISHED,
            run_id="r",
            status="completed",
            final_text="The area is $\\pi r^2$ exactly.",
            turns=1,
            tokens_in=0,
            tokens_out=0,
        ),
    )
    assert wait_for(lambda: engine.synthesized == ["The area is pi r squared exactly."])
    speaker.close()


# -- through the daemon ---------------------------------------------------------


def test_daemon_funnel_feeds_speaker(config):
    """A real run through AgentDaemon speaks final_text, nothing else.

    The multi-turn fake provider first answers with a preamble + tool
    call, then the answer: exactly the shape the read policy exists for.
    """
    from phoenix_dna.daemon import AgentDaemon
    from phoenix_dna.provider import FakeProvider

    engine = FakeEngine()
    speaker = Speaker(engine=engine, player=FakePlayer(), enabled=True)
    provider = FakeProvider(
        [
            {
                "text": "Let me look at the disk first. ",
                "tool": "bash",
                "args": {"command": "echo hi"},
            },
            "The disk is fine. Everything checks out in the end.",
        ]
    )
    daemon = AgentDaemon(config, provider=provider, speaker=speaker)
    speaker.set_emitter(daemon._emit)
    try:
        daemon.submit("check the disk", "user")
        assert wait_for(
            lambda: engine.synthesized
            == ["The disk is fine. Everything checks out in the end."]
        ), engine.synthesized
    finally:
        daemon.shutdown()
