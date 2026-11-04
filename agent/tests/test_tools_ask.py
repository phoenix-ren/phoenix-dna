# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""ask_user tests: the batch form - validation, answering (whole, partial,
blank), timeouts, cancellation, and the loop/daemon integration."""

from __future__ import annotations

import dataclasses
import json
import threading

import pytest

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.loop import AgentLoop
from phoenix_dna.toolset import build_toolset
from phoenix_dna.prompts import build_system_prompt
from phoenix_dna.provider import FakeProvider
from phoenix_dna.sessions import SessionManager, SessionStore
from phoenix_dna.tools import ToolError
from phoenix_dna.tools.ask import AskCoordinator, AskUserTool, _normalize_form


def disk_question():
    return {
        "question": "Which disk should the backup use?",
        "header": "Backup disk",
        "options": [
            {"label": "/dev/sda", "description": "the SSD"},
            {"label": "/dev/sdb", "description": "the spinning one"},
        ],
    }


def form_args(**overrides):
    args = {
        "questions": [
            disk_question(),
            {
                "question": "Encrypt the backup?",
                "options": [{"label": "Yes"}, {"label": "No"}],
            },
        ],
        "goal": "backup setup",
    }
    args.update(overrides)
    return args


FORM = {
    "questions": [
        {
            "question": "Which disk should the backup use?",
            "header": "Backup disk",
            "options": [
                {"label": "/dev/sda", "description": "the SSD"},
                {"label": "/dev/sdb", "description": "the spinning one"},
            ],
        },
        {"question": "Encrypt the backup?", "options": [{"label": "Yes"}, {"label": "No"}]},
    ],
    "goal": "backup setup",
}


def make_tool(events, **kwargs):
    coordinator = AskCoordinator(emit=lambda event: events.append(event))
    # a default timeout keeps a lost answer from blocking the suite forever
    kwargs.setdefault("timeout", 10.0)
    return AskUserTool(coordinator, **kwargs), coordinator


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def answer_when_asked(coordinator, events, payload):
    """Answer the form in another thread (the tool blocks)."""
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


# -- argument validation ------------------------------------------------------


@pytest.mark.parametrize(
    "args, complaint",
    [
        ({}, "questions"),
        ({"questions": []}, "questions"),
        (
            {"questions": [{"question": "q", "options": [{"label": "only one"}]}]},
            "2 to 6",
        ),
        (
            {
                "questions": [
                    {"question": "q", "options": [{"label": "a"}, {"label": "b"}]}
                ],
                "goal": "  ",
            },
            "goal",
        ),
        (
            {
                "questions": [
                    {
                        "question": "q",
                        "options": [{"label": "a"}, {"label": "b"}],
                        "multiple": "yes",
                    }
                ]
            },
            "multiple",
        ),
    ],
)
def test_validation_rejects_bad_arguments(args, complaint):
    tool, _coordinator = make_tool([])
    with pytest.raises(ToolError) as excinfo:
        tool.execute(args)
    assert complaint in str(excinfo.value)


def test_form_is_normalized_for_the_ui():
    form = _normalize_form(form_args())
    assert form == FORM

    # a single question is a form of one (the inline card path)
    single = _normalize_form({"questions": [disk_question()]})
    assert len(single["questions"]) == 1
    assert "goal" not in single


# -- answering ----------------------------------------------------------------


def test_answer_whole_form():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    payload = {
        "answers": [
            {"selected": ["/dev/sda"], "text": "the fast one"},
            {"selected": [1]},
        ]
    }
    thread, result = answer_when_asked(coordinator, events, payload)
    outcome = tool.execute(form_args())
    thread.join(timeout=5)

    assert result["ok"] is True
    assert outcome.output == (
        "The user answered your questions:\n"
        '1. Which disk should the backup use? → chose "/dev/sda" and said "the fast one"\n'
        '2. Encrypt the backup? → chose "No"'
    )
    assert outcome.details["ask_status"] == "answered"
    assert outcome.details["answer"] == {
        "answers": [
            {"selected": ["/dev/sda"], "text": "the fast one"},
            {"selected": ["No"], "text": ""},
        ]
    }


def test_blank_answer_is_a_deliberate_skip():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    payload = {"answers": [{}, {"text": "  "}]}
    thread, _r = answer_when_asked(coordinator, events, payload)
    outcome = tool.execute(form_args())
    thread.join(timeout=5)

    assert "(no answer)" in outcome.output


def test_a_bare_string_answers_the_first_question():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    # a one-word dictated reply, as the input bar sends it
    thread, _r = answer_when_asked(coordinator, events, "the ssd")
    outcome = tool.execute({"questions": [disk_question()]})
    thread.join(timeout=5)

    assert 'said "the ssd"' in outcome.output


def test_events_announce_the_form():
    events: list[dict] = []
    tool, coordinator = make_tool(events, run_id="run_x")

    payload = {"answers": [{"selected": [1]}, {"selected": [0]}]}
    thread, _r = answer_when_asked(coordinator, events, payload)
    tool.execute(form_args())
    thread.join(timeout=5)

    asked = events_of(events, "question_asked")[0]
    assert asked["ask_id"].startswith("ask_")
    assert asked["run_id"] == "run_x"
    assert asked["question"] == FORM  # the form travels as the payload

    answered = events_of(events, "question_answered")[0]
    assert answered["ask_id"] == asked["ask_id"]
    assert answered["ask_status"] == "answered"
    assert answered["answer"] == {
        "answers": [{"selected": ["/dev/sdb"], "text": ""}, {"selected": ["Yes"], "text": ""}]
    }


def test_unusable_answers_are_rejected_and_the_form_waits():
    events: list[dict] = []
    tool, coordinator = make_tool(events)

    assert coordinator.answer("ask_nope", '{"answers": [{}]}') is False

    answered = threading.Event()

    def answer_late():
        if not wait_for(lambda: events_of(events, "question_asked"), timeout=5):
            return
        ask_id = events_of(events, "question_asked")[0]["ask_id"]
        # every one of these leaves the form pending...
        for raw in (
            '{"answers": [{}]}',  # too few answers
            '{"answers": [{}, {}, {}]}',  # too many
            '{"answers": [{"selected": [9]}, {}]}',  # index out of range
            '{"answers": [{"selected": ["/dev/sdc"]}, {}]}',  # unknown label
            '{"answers": [{"selected": [0, 1]}, {}]}',  # two picks, single choice
            "not json {",  # unparseable
        ):
            assert coordinator.answer(ask_id, raw) is False
        assert coordinator.pending_ask_ids() == [ask_id]
        # ...until a good one arrives
        assert coordinator.answer(ask_id, '{"answers": [{}, {"text": "yes please"}]}') is True
        assert coordinator.answer(ask_id, '{"answers": [{}, {}]}') is False
        answered.set()

    thread = threading.Thread(target=answer_late, daemon=True)
    thread.start()
    outcome = tool.execute(form_args())
    thread.join(timeout=5)

    assert answered.is_set()
    assert 'said "yes please"' in outcome.output


# -- timeout and cancellation ---------------------------------------------------


def test_timeout_tells_the_model_to_proceed():
    events: list[dict] = []
    tool, _coordinator = make_tool(events, run_id="run_t", timeout=0.15)

    outcome = tool.execute(form_args())

    assert "did not answer" in outcome.output
    assert "best judgment" in outcome.output
    assert outcome.details == {
        "form": FORM,
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
        tool.execute(form_args(), cancel=cancel)

    assert events_of(events, "question_answered")[0]["ask_status"] == "cancelled"


# -- agent loop integration -----------------------------------------------------


def make_ask_loop(config, home_dir, script):
    events: list[dict] = []
    coordinator = AskCoordinator(emit=lambda event: events.append(event))
    tools = build_toolset(
        config,
        home_dir=home_dir,
        run_id="run_test",
        ask_coordinator=coordinator,
    )
    manager = SessionManager(SessionStore(config.data_dir / "sessions"))
    loop = AgentLoop(
        run_id="run_test",
        provider=FakeProvider(script),
        tools=tools,
        system_prompt=build_system_prompt(),
        conversation=manager.get(),
        session_manager=manager,
        event_sink=lambda event: events.append(event),
    )
    return loop, events, coordinator


def test_full_run_with_an_answered_form(config, home_dir):
    script = [
        {"tool": "ask_user", "args": form_args()},
        "Backing up to the chosen disk.",
    ]
    loop, events, coordinator = make_ask_loop(config, home_dir, script)

    payload = {"answers": [{"selected": [1]}, {"selected": [0]}]}
    thread, _r = answer_when_asked(coordinator, events, payload)
    summary = loop.run("back up my disk")
    thread.join(timeout=5)

    assert summary["status"] == "completed"
    assert summary["final_text"] == "Backing up to the chosen disk."
    assert "ask_user" in {t.name for t in loop._provider.last_tools}

    conversation = loop._conversation
    roles = [m["role"] for m in conversation.messages]
    assert roles == ["user", "assistant", "tool", "assistant"]
    assert "/dev/sdb" in conversation.messages[2]["content"]
    assert "Yes" in conversation.messages[2]["content"]


def test_full_run_with_timeout(config, home_dir):
    script = [
        {"tool": "ask_user", "args": form_args()},
        "Backed up to /dev/sda by default.",
    ]
    cfg = dataclasses.replace(config, ask_timeout_seconds=0.15)
    loop, events, _coordinator = make_ask_loop(cfg, home_dir, script)

    summary = loop.run("back up my disk")

    assert summary["status"] == "completed"
    tool_done = events_of(events, "tool_call_finished")[0]
    assert tool_done["status"] == "success"
    assert "did not answer" in tool_done["output"]


# -- daemon integration ----------------------------------------------------------


def test_daemon_ask_flow(config):
    events: list[dict] = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(
            [
                {"tool": "ask_user", "args": form_args()},
                "Backed up to /dev/sdb.",
            ]
        ),
        emitter=lambda event: events.append(event),
    )
    try:
        run_id = daemon.submit("back up my disk", "user")

        assert wait_for(lambda: events_of(events, "question_asked"), timeout=5)
        asked = events_of(events, "question_asked")[0]
        assert asked["run_id"] == run_id
        assert asked["question"]["questions"][0]["header"] == "Backup disk"

        assert daemon.answer_question("ask_bogus", '{"answers": [{}]}') is False
        assert (
            daemon.answer_question(
                asked["ask_id"], '{"answers": [{"selected": [1]}, {"selected": [1]}]}'
            )
            is True
        )

        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        finished = events_of(events, "run_finished")[0]
        assert finished["status"] == "completed"
        assert finished["final_text"] == "Backed up to /dev/sdb."
        assert wait_for(lambda: daemon.state()["state"] == "idle")
    finally:
        daemon.shutdown()


def test_daemon_cancel_releases_a_pending_form(config):
    events: list[dict] = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider([{"tool": "ask_user", "args": form_args()}, "never"]),
        emitter=lambda event: events.append(event),
    )
    try:
        run_id = daemon.submit("back up my disk", "user")
        assert wait_for(lambda: events_of(events, "question_asked"), timeout=5)

        assert daemon.cancel(run_id) is True
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)

        assert events_of(events, "run_finished")[0]["status"] == "cancelled"
        assert events_of(events, "question_answered")[0]["ask_status"] == "cancelled"
        assert wait_for(lambda: daemon.state()["state"] == "idle")

        asked = events_of(events, "question_asked")[0]
        assert daemon.answer_question(asked["ask_id"], '{"answers": [{}]}') is False
    finally:
        daemon.shutdown()
