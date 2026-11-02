# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""REPL helper tests: event rendering, run waiting, D-Bus signal conversion."""

from __future__ import annotations

import queue

from phoenix_ai.repl import (
    EventPrinter,
    dbus_signal_to_event,
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
    assert dbus_signal_to_event("RunStarted", "r1", "web", "s9") == {
        "kind": "run_started",
        "run_id": "r1",
        "mode": "web",
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
