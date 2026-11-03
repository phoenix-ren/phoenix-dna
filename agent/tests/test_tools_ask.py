# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""ask_user tests: validation, answering, timeouts, cancellation, and the
loop/daemon integration (the blocked tool released by an answer)."""

from __future__ import annotations

import dataclasses
import json
import threading

import pytest

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.loop import AgentLoop
from phoenix_dna.modes import MODE_PROFILES, Mode, build_toolset
from phoenix_dna.prompts import build_system_prompt
from phoenix_dna.provider import FakeProvider
from phoenix_dna.sessions import SessionManager, SessionStore
from phoenix_dna.tools import ToolError
from phoenix_dna.tools.ask import (
    AskCoordinator,
    AskUserTool,
    _normalize_question,
)


QUESTION_ARGS = {
    "question": "Which disk should the backup use?",
    "header": "Backup disk",
    "options": [
        {"label": "/dev/sda", "description": "the SSD"},
        {"label": "/dev/sdb", "description": "the spinning one"},
    ],
}

QUESTION = {
    "question": "Which disk should the backup use?",
    "header": "Backup disk",
    "options": [
        {"label": "/dev/sda", "description": "the SSD"},
        {"label": "/dev/sdb", "description": "the spinning one"},
    ],
}


def make_tool(events, **kwargs):
    coordinator = AskCoordinator(emit=lambda event: events.append(event))
    # a default timeout keeps a lost answer from blocking the suite forever
    kwargs.setdefault("timeout", 10.0)
    return AskUserTool(coordinator, **kwargs), coordinator


def answer_when_asked(coordinator, events, payload):
    """Answer the first question in another thread (the tool blocks)."""
    result = {}

    def run():
        if wait_for(lambda: events_of(events, "question_asked"), timeout=5):
            asked = events_of(events, "question_asked")[0]
            result["ok"] = coordinator.answer(
                asked["ask_id"], json.dumps(payload)
            )

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, result


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


# -- argument validation ------------------------------------------------------


@pytest.mark.parametrize(
    "args, complaint",
    [
        ({"options": QUESTION_ARGS["options"]}, "question"),
        ({"question": "q", "options": [{"label": "only"}]}, "2 to 6"),
        ({"question": "q", "options": [{"label": str(i)} for i in range(7)]}, "2 to 6"),
        ({"question": "q", "options": ["a", "b"]}, "option 1"),
        (
            {"question": "q", "options": [{"label": "a"}, {"description": "d"}]},
            "option 2",
        ),
        ({"question": "  ", "options": QUESTION_ARGS["options"]}, "question"),
        ({"question": 1, "options": QUESTION_ARGS["options"]}, "question"),
        (
            {"question": "q", "options": QUESTION_ARGS["options"], "multiple": "yes"},
            "multiple",
        ),
        (
            {"question": "q", "options": QUESTION_ARGS["options"], "step": 0},
            "step",
        ),
        (
            {"question": "q", "options": QUESTION_ARGS["options"], "est_steps": "four"},
            "est_steps",
        ),
    ],
)
def test_validation_rejects_bad_arguments(args, complaint):
    tool, _coordinator = make_tool([])
    with pytest.raises(ToolError) as excinfo:
        tool.execute(args)
    assert complaint in str(excinfo.value)


def test_question_is_normalized_for_the_ui():
    normalize = _normalize_question

    payload = normalize(
        {
            "question": "  Which disk?  ",
            "options": [
                {"label": " sda ", "description": "  the SSD  "},
                {"label": "sdb", "description": None},
            ],
            "header": "disk",
            "multiple": True,
            "goal": "backup setup",
            "step": 2,
            "est_steps": 4,
        }
    )
    assert payload == {
        "question": "Which disk?",
        "header": "disk",
        "options": [
            {"label": "sda", "description": "the SSD"},
            {"label": "sdb"},
        ],
        "multiple": True,
        "goal": "backup setup",
        "step": 2,
        "est_steps": 4,
    }

    # wizard metadata is optional; absent fields stay absent
    assert "multiple" not in normalize(
        {"question": "q", "options": [{"label": "a"}, {"label": "b"}]}
    )


# -- answering ----------------------------------------------------------------


def test_answer_by_index_and_by_label():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    thread, result = answer_when_asked(coordinator, events, {"selected": [0]})
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)

    assert result["ok"] is True
    assert outcome.output == 'The user chose "/dev/sda".'
    assert outcome.details["ask_status"] == "answered"
    assert outcome.details["answer"] == {"selected": ["/dev/sda"], "text": ""}
    assert outcome.details["question"]["question"] == QUESTION["question"]

    events.clear()
    thread, result = answer_when_asked(coordinator, events, {"selected": ["/dev/sdb"]})
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)

    assert result["ok"] is True
    assert outcome.output == 'The user chose "/dev/sdb".'


def test_events_announce_ask_and_answer():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    thread, _result = answer_when_asked(coordinator, events, {"selected": [1]})
    tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)

    asked = events_of(events, "question_asked")[0]
    assert asked["ask_id"].startswith("ask_")
    assert asked["run_id"] == tool._run_id  # empty here; the daemon fills it
    assert asked["question"] == QUESTION

    answered = events_of(events, "question_answered")[0]
    assert answered["ask_id"] == asked["ask_id"]
    assert answered["ask_status"] == "answered"
    assert answered["answer"] == {"selected": ["/dev/sdb"], "text": ""}


def test_free_text_and_combined_answers():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    thread, _r = answer_when_asked(coordinator, events, {"text": "the external one"})
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)
    assert outcome.output == 'The user said "the external one".'

    events.clear()
    thread, _r = answer_when_asked(
        coordinator, events, {"selected": [0], "text": "and hurry"}
    )
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)
    assert outcome.output == 'The user chose "/dev/sda" and said "and hurry".'

    # a bare JSON string counts as the user's own words (gdbus one-liners)
    events.clear()
    thread, _r = answer_when_asked(coordinator, events, "whichever is faster")
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)
    assert outcome.output == 'The user said "whichever is faster".'


def test_multiple_selection():
    events: list[dict] = []
    args = dict(QUESTION_ARGS, multiple=True, question="Which disks?")
    tool, coordinator = make_tool(events)

    thread, _r = answer_when_asked(coordinator, events, {"selected": [0, 1]})
    outcome = tool.execute(args)
    thread.join(timeout=5)
    assert outcome.output == 'The user chose several: "/dev/sda", "/dev/sdb".'


def test_unusable_answers_are_rejected_and_the_question_waits():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    def reject(payload):
        assert coordinator.answer("ask_nope", json.dumps(payload)) is False

    reject({"selected": [0]})  # unknown ask_id

    answered = threading.Event()

    def answer_late():
        if not wait_for(lambda: events_of(events, "question_asked"), timeout=5):
            return
        ask_id = events_of(events, "question_asked")[0]["ask_id"]
        # every one of these leaves the question pending...
        for raw in (
            '{"selected": [9]}',  # index out of range
            '{"selected": ["/dev/sdc"]}',  # unknown label
            '{"selected": [0, 1]}',  # several picks, single choice
            '{"selected": []}',  # empty
            '{"text": "   "}',  # still empty
            "not json {",  # unparseable (sent raw, not through dumps)
        ):
            assert coordinator.answer(ask_id, raw) is False
        assert coordinator.pending_ask_ids() == [ask_id]
        # ...until a good one arrives
        assert coordinator.answer(ask_id, '{"selected": [1]}') is True
        # and a second answer for a settled question is refused
        assert coordinator.answer(ask_id, '{"selected": [0]}') is False
        answered.set()

    thread = threading.Thread(target=answer_late, daemon=True)
    thread.start()
    outcome = tool.execute(QUESTION_ARGS)
    thread.join(timeout=5)

    assert answered.is_set()
    assert outcome.output == 'The user chose "/dev/sdb".'


# -- timeout and cancellation ---------------------------------------------------


def test_timeout_tells_the_model_to_proceed():
    events: list[dict] = []
    tool, _coordinator = make_tool(events, run_id="run_t", timeout=0.15)

    outcome = tool.execute(QUESTION_ARGS)

    assert "did not answer" in outcome.output
    assert "best judgment" in outcome.output
    assert outcome.details == {
        "question": QUESTION,
        "answer": None,
        "ask_status": "timeout",
    }
    assert events_of(events, "question_answered")[0]["ask_status"] == "timeout"


def test_cancel_unblocks_the_tool():
    events: list[dict] = []
    tool, _coordinator = make_tool(events)
    cancel = threading.Event()
    threading.Timer(0.15, cancel.set).start()

    with pytest.raises(ToolError, match="cancelled"):
        tool.execute(QUESTION_ARGS, cancel=cancel)

    answered = events_of(events, "question_answered")
    assert answered[0]["ask_status"] == "cancelled"


# -- agent loop integration -----------------------------------------------------


def make_ask_loop(config, home_dir, script, **toolset_kw):
    events: list[dict] = []
    coordinator = AskCoordinator(emit=lambda event: events.append(event))
    tools = build_toolset(
        MODE_PROFILES[Mode.USER],
        config,
        home_dir=home_dir,
        run_id="run_test",
        ask_coordinator=coordinator,
        **toolset_kw,
    )
    manager = SessionManager(SessionStore(config.data_dir / "sessions"))
    loop = AgentLoop(
        run_id="run_test",
        mode_name="user",
        provider=FakeProvider(script),
        tools=tools,
        system_prompt=build_system_prompt(MODE_PROFILES[Mode.USER]),
        conversation=manager.get("user"),
        session_manager=manager,
        event_sink=lambda event: events.append(event),
    )
    return loop, events, coordinator


def test_full_run_with_an_answered_question(config, home_dir):
    script = [
        {"tool": "ask_user", "args": QUESTION_ARGS},
        "Backing up to the chosen disk.",
    ]
    loop, events, coordinator = make_ask_loop(config, home_dir, script)

    thread, _r = answer_when_asked(coordinator, events, {"selected": [1]})
    summary = loop.run("back up my disk")
    thread.join(timeout=5)

    assert summary["status"] == "completed"
    assert summary["final_text"] == "Backing up to the chosen disk."

    # the model was offered the tool and the Q&A is in the transcript
    assert "ask_user" in {t.name for t in loop._provider.last_tools}
    conversation = loop._conversation
    roles = [m["role"] for m in conversation.messages]
    assert roles == ["user", "assistant", "tool", "assistant"]
    assert "/dev/sdb" in conversation.messages[2]["content"]


def test_full_run_with_timeout(config, home_dir):
    script = [
        {"tool": "ask_user", "args": QUESTION_ARGS},
        "Backed up to /dev/sda by default.",
    ]
    # the short timeout must flow from the config through build_toolset
    cfg = dataclasses.replace(config, ask_timeout_seconds=0.15)
    loop, events, _coordinator = make_ask_loop(cfg, home_dir, script)

    summary = loop.run("back up my disk")

    assert summary["status"] == "completed"
    tool_done = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert tool_done["status"] == "success"
    assert "did not answer" in tool_done["output"]


# -- daemon integration ----------------------------------------------------------


def test_daemon_ask_flow(config):
    events: list[dict] = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(
            [{"tool": "ask_user", "args": QUESTION_ARGS}, "Backed up to /dev/sdb."]
        ),
        emitter=lambda event: events.append(event),
    )
    try:
        run_id = daemon.submit("back up my disk", "user")

        assert wait_for(lambda: events_of(events, "question_asked"), timeout=5)
        asked = events_of(events, "question_asked")[0]
        assert asked["run_id"] == run_id

        # an answer for an unknown question is refused, the real one works
        assert daemon.answer_question("ask_bogus", '{"selected": [0]}') is False
        assert (
            daemon.answer_question(asked["ask_id"], '{"selected": [1]}') is True
        )

        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        finished = events_of(events, "run_finished")[0]
        assert finished["status"] == "completed"
        assert finished["final_text"] == "Backed up to /dev/sdb."
        answered = events_of(events, "question_answered")[0]
        assert answered["ask_status"] == "answered"
        assert answered["answer"] == {"selected": ["/dev/sdb"], "text": ""}
        assert wait_for(lambda: daemon.state()["state"] == "idle")
    finally:
        daemon.shutdown()


def test_daemon_cancel_releases_a_pending_question(config):
    events: list[dict] = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(
            [{"tool": "ask_user", "args": QUESTION_ARGS}, "never reached"]
        ),
        emitter=lambda event: events.append(event),
    )
    try:
        run_id = daemon.submit("back up my disk", "user")
        assert wait_for(lambda: events_of(events, "question_asked"), timeout=5)

        assert daemon.cancel(run_id) is True
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)

        finished = events_of(events, "run_finished")[0]
        assert finished["status"] == "cancelled"
        answered = events_of(events, "question_answered")[0]
        assert answered["ask_status"] == "cancelled"
        assert wait_for(lambda: daemon.state()["state"] == "idle")

        # a late answer for the released question is refused
        asked = events_of(events, "question_asked")[0]
        assert daemon.answer_question(asked["ask_id"], '{"selected": [0]}') is False
    finally:
        daemon.shutdown()
