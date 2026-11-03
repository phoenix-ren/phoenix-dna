# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""REPL helper tests: event rendering, run waiting, D-Bus signal conversion."""

from __future__ import annotations

import json
import queue

from phoenix_dna.repl import (
    EventPrinter,
    dbus_signal_to_event,
    parse_answer_line,
    read_answer,
    wait_for_run,
)


def feed(printer, *events):
    for event in events:
        printer.on_event(event)


def test_printer_renders_a_full_run(capsys):
    printer = EventPrinter(color=False)
    feed(
        printer,
        {
            "kind": "run_started",
            "run_id": "run_abc",
            "mode": "user",
            "session_id": "sess1",
        },
        {"kind": "turn_started", "run_id": "run_abc", "turn": 1},
        {"kind": "text_delta", "run_id": "run_abc", "text": "Let me check "},
        {"kind": "text_delta", "run_id": "run_abc", "text": "the file."},
        {
            "kind": "tool_call_started",
            "run_id": "run_abc",
            "call_id": "c1",
            "tool": "bash",
            "args": {"command": "ls -la /tmp"},
        },
        {
            "kind": "tool_call_finished",
            "run_id": "run_abc",
            "call_id": "c1",
            "tool": "bash",
            "status": "success",
            "output": "total 0\ndrwxr-xr-x",
            "details": {},
        },
        {
            "kind": "tool_call_finished",
            "run_id": "run_abc",
            "call_id": "c2",
            "tool": "edit",
            "status": "error",
            "output": "oldText not found",
            "details": {},
        },
        {
            "kind": "run_finished",
            "run_id": "run_abc",
            "status": "completed",
            "final_text": "x",
            "turns": 2,
            "tokens_in": 11,
            "tokens_out": 7,
        },
    )
    out = capsys.readouterr().out
    assert "── run run_abc · user · session sess1" in out
    assert "Let me check the file." in out
    assert '⚒ bash {"command": "ls -la /tmp"}' in out
    assert "✓ total 0" in out
    assert "✗ oldText not found" in out
    assert "● completed · 2 turns · 11+7 tokens" in out


def test_printer_renders_auth_events(capsys):
    printer = EventPrinter(color=False)
    feed(
        printer,
        {
            "kind": "auth_requested",
            "run_id": "r",
            "message": "Administrator password required",
        },
        {"kind": "auth_finished", "run_id": "r", "success": True},
        {"kind": "auth_finished", "run_id": "r2", "success": False},
    )
    out = capsys.readouterr().out
    assert "administrator password requested" in out
    assert "password accepted" in out
    assert "password failed or cancelled" in out


def test_printer_truncates_long_tool_args_and_output(capsys):
    printer = EventPrinter(color=False)
    printer.on_event(
        {
            "kind": "tool_call_started",
            "run_id": "r",
            "call_id": "c",
            "tool": "write",
            "args": {"content": "x" * 300},
        }
    )
    printer.on_event(
        {
            "kind": "tool_call_finished",
            "run_id": "r",
            "call_id": "c",
            "tool": "bash",
            "status": "success",
            "output": "\n".join(f"line{i}" for i in range(10)),
        }
    )
    out = capsys.readouterr().out
    assert "..." in out  # args truncated
    assert "+7 more lines" in out  # 10 lines -> 3 shown + note


def test_printer_renders_a_question_and_collects_the_answer(capsys):
    answered = []
    printer = EventPrinter(
        color=False, answer_fn=lambda ask_id, question: answered.append(ask_id)
    )
    printer.on_event(
        {
            "kind": "question_asked",
            "run_id": "r",
            "ask_id": "ask_1",
            "question": {
                "question": "Which disk should the backup use?",
                "header": "Backup disk",
                "options": [
                    {"label": "/dev/sda", "description": "the SSD"},
                    {"label": "/dev/sdb"},
                ],
            },
        }
    )
    out = capsys.readouterr().out
    assert "❓ Which disk should the backup use?" in out
    assert "1) /dev/sda — the SSD" in out
    assert "2) /dev/sdb" in out
    assert "or your own words" in out
    assert answered == ["ask_1"]

    # wizard metadata is shown as progress context
    printer.on_event(
        {
            "kind": "question_asked",
            "run_id": "r",
            "ask_id": "ask_2",
            "question": {
                "question": "Encrypt it?",
                "options": [{"label": "Yes"}, {"label": "No"}],
                "step": 2,
                "est_steps": 4,
                "goal": "backup setup",
            },
        }
    )
    out = capsys.readouterr().out
    assert "(step 2 of ~4: backup setup) Encrypt it?" in out


def test_printer_badges_harness_confirmations(capsys):
    printer = EventPrinter(color=False)
    printer.on_event(
        {
            "kind": "question_asked",
            "run_id": "r",
            "ask_id": "ask_c",
            "question": {
                "question": "Allow this root command to run?\n\n$ rm -rf /tmp/x",
                "options": [{"label": "Allow"}, {"label": "Deny"}],
                "harness": True,
            },
        }
    )
    out = capsys.readouterr().out
    assert "system confirmation" in out
    assert "$ rm -rf /tmp/x" in out
    assert "1) Allow" in out
    assert "2) Deny" in out


def test_parse_answer_line():
    question = {
        "options": [{"label": "/dev/sda"}, {"label": "/dev/sdb"}],
    }
    assert parse_answer_line("1", question) == {"selected": [0]}
    assert parse_answer_line("2", question) == {"selected": [1]}
    assert parse_answer_line("/dev/sdb", question) == {"selected": [1]}
    assert parse_answer_line("/dev/SDA", question) == {"selected": [0]}
    assert parse_answer_line("the external one", question) == {
        "text": "the external one"
    }
    assert parse_answer_line("", question) == {}
    assert parse_answer_line("9", question) == {"text": "9"}

    multi = dict(question, multiple=True)
    assert parse_answer_line("1, 2", multi) == {"selected": [0, 1]}
    assert parse_answer_line("2, and hurry", multi) == {
        "selected": [1],
        "text": "and hurry",
    }


def test_read_answer_prompts_until_nonempty():
    question = {"options": [{"label": "a"}, {"label": "b"}]}
    inputs = iter(["", "  ", "2"])
    answer = read_answer(question, input_fn=lambda _prompt: next(inputs))
    assert answer == {"selected": [1]}


def test_read_answer_eof_degrades_to_an_explicit_no_answer():
    def eof(_prompt):
        raise EOFError

    answer = read_answer({"options": [{"label": "a"}, {"label": "b"}]}, input_fn=eof)
    assert answer["text"]


def test_dbus_signal_to_event_questions():
    question_json = json.dumps({"question": "Disk?", "options": [{"label": "a"}]})
    assert dbus_signal_to_event("QuestionAsked", "r1", "ask_9", question_json) == {
        "kind": "question_asked",
        "run_id": "r1",
        "ask_id": "ask_9",
        "question": {"question": "Disk?", "options": [{"label": "a"}]},
    }
    # malformed question_json degrades to an empty question
    assert dbus_signal_to_event("QuestionAsked", "r1", "ask_9", "{oops")[
        "question"
    ] == {}
    assert dbus_signal_to_event(
        "QuestionAnswered", "r1", "ask_9", "answered", '{"selected": ["a"]}'
    ) == {
        "kind": "question_answered",
        "run_id": "r1",
        "ask_id": "ask_9",
        "ask_status": "answered",
        "answer": {"selected": ["a"]},
    }


def test_wait_for_run_returns_only_the_matching_finish():
    events = queue.Queue()
    for event in [
        {"kind": "run_started", "run_id": "other"},
        {
            "kind": "run_finished",
            "run_id": "other",
            "status": "completed",
            "final_text": "",
        },
        {"kind": "text_delta", "run_id": "mine", "text": "hi"},
        {
            "kind": "run_finished",
            "run_id": "mine",
            "status": "cancelled",
            "final_text": "",
        },
    ]:
        events.put(event)
    seen = []
    finished = wait_for_run(events, "mine", on_event=seen.append)
    assert finished["status"] == "cancelled"
    assert len(seen) == 4  # everything was passed through, including other runs


def test_dbus_signal_to_event():
    assert dbus_signal_to_event("RunStarted", "r1", "user", "s9") == {
        "kind": "run_started",
        "run_id": "r1",
        "mode": "user",
        "session_id": "s9",
    }
    assert dbus_signal_to_event("TextDelta", "r1", "chunk") == {
        "kind": "text_delta",
        "run_id": "r1",
        "text": "chunk",
    }
    event = dbus_signal_to_event(
        "ToolCallStarted", "r1", "c1", "bash", '{"command": "ls"}'
    )
    assert event == {
        "kind": "tool_call_started",
        "run_id": "r1",
        "call_id": "c1",
        "tool": "bash",
        "args": {"command": "ls"},
    }
    # malformed args_json degrades to empty args
    assert (
        dbus_signal_to_event("ToolCallStarted", "r", "c", "bash", "{oops")["args"] == {}
    )
    assert (
        dbus_signal_to_event("ToolCallFinished", "r", "c", "bash", "error", "boom")[
            "status"
        ]
        == "error"
    )
    assert dbus_signal_to_event("AuthFinished", "r", True)["success"] is True
    finished = dbus_signal_to_event("RunFinished", "r", "completed", "done", 2, 10, 5)
    assert finished == {
        "kind": "run_finished",
        "run_id": "r",
        "status": "completed",
        "final_text": "done",
        "turns": 2,
        "tokens_in": 10,
        "tokens_out": 5,
    }
    assert dbus_signal_to_event("StateChanged", "idle", "", 0) is None
