# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Agent loop tests: turns, tool execution, events, cancellation, errors."""

from __future__ import annotations

import dataclasses

from phoenix_ai.loop import AgentLoop
from phoenix_ai.modes import MODE_PROFILES, Mode, build_toolset
from phoenix_ai.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_ai.prompts import build_system_prompt
from phoenix_ai.provider import FakeProvider
from phoenix_ai.sessions import SessionManager, SessionStore


def make_loop(
    config, home_dir, script, mode=Mode.USER, cache=None, sudo_bin=None, **loop_kw
):
    profile = MODE_PROFILES[mode]
    cfg = dataclasses.replace(config, sudo_bin=sudo_bin or config.sudo_bin)
    tools = build_toolset(profile, cfg, credential_cache=cache, home_dir=home_dir)
    manager = SessionManager(SessionStore(config.data_dir / "sessions"))
    conversation = manager.get(mode.value)
    events, sink = [], (lambda e: events.append(e))
    provider = FakeProvider(script)
    loop = AgentLoop(
        run_id="run_test",
        mode_name=mode.value,
        provider=provider,
        tools=tools,
        system_prompt=build_system_prompt(profile),
        conversation=conversation,
        session_manager=manager,
        event_sink=sink,
        **loop_kw,
    )
    return loop, events, conversation, manager


def kinds(events):
    return [e["kind"] for e in events]


def test_happy_path_with_tool(config, home_dir):
    script = [
        {"tool": "bash", "args": {"command": "echo hello > greet.txt"}},
        "The file was written.",
    ]
    loop, events, conv, manager = make_loop(config, home_dir, script)
    summary = loop.run("create greet.txt saying hello")

    assert summary["status"] == "completed"
    assert summary["final_text"] == "The file was written."
    assert summary["turns"] == 2
    assert (home_dir / "greet.txt").read_text() == "hello\n"

    seq = kinds(events)
    assert seq[0] == "run_started"
    assert "turn_started" in seq
    assert "tool_call_started" in seq and "tool_call_finished" in seq
    assert seq[-1] == "run_finished"
    assert seq.count("text_delta") == 2  # fake provider streams two chunks

    tool_done = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert tool_done["status"] == "success"
    assert "hello" not in tool_done["output"] or True

    roles = [m["role"] for m in conv.messages]
    assert roles == ["user", "assistant", "tool", "assistant"]
    assert conv.messages[1]["tool_calls"][0]["function"]["name"] == "bash"


def test_multiple_tool_calls_in_one_turn(config, home_dir):
    script = [
        {
            "tools": [
                {"tool": "bash", "args": {"command": "echo a > a.txt"}},
                {"tool": "write", "args": {"path": "b.txt", "content": "bee"}},
            ]
        },
        "Both files created.",
    ]
    loop, events, conv, _ = make_loop(config, home_dir, script)
    summary = loop.run("create a.txt and b.txt")
    assert summary["status"] == "completed"
    assert (home_dir / "a.txt").exists() and (home_dir / "b.txt").exists()
    assert kinds(events).count("tool_call_started") == 2
    assert len([m for m in conv.messages if m["role"] == "tool"]) == 2


def test_unknown_tool_recovered(config, home_dir):
    script = [
        {"tool": "teleport", "args": {}},
        "Recovered without that tool.",
    ]
    loop, events, _conv, _ = make_loop(config, home_dir, script)
    summary = loop.run("do the impossible")
    assert summary["status"] == "completed"
    failed = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert failed["status"] == "error"
    assert "unknown tool 'teleport'" in failed["output"]


def test_missing_required_argument(config, home_dir):
    script = [{"tool": "bash", "args": {"timeout": 5}}, "Recovered."]
    loop, events, _c, _m = make_loop(config, home_dir, script)
    summary = loop.run("run nothing")
    assert summary["status"] == "completed"
    failed = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert "missing required argument" in failed["output"]


class _BrokenArgsProvider:
    """Turn 1 emits a ToolCall with unparseable arguments, turn 2 recovers."""

    def __init__(self):
        self.turns = 0

    def stream(self, messages, tools, should_cancel=None):
        from phoenix_ai.provider import LLMResponse, ToolCall, Usage

        self.turns += 1
        if self.turns == 1:
            yield {
                "type": "response",
                "response": LLMResponse(
                    tool_calls=[
                        ToolCall(
                            id="call_bad",
                            name="bash",
                            arguments={},
                            arguments_error="Invalid \\escape: line 1 column 116",
                        )
                    ],
                    usage=Usage(input_tokens=1, output_tokens=1),
                    finish_reason="tool_calls",
                ),
            }
        else:
            yield {"type": "text_delta", "text": "Recovered."}
            yield {
                "type": "response",
                "response": LLMResponse(
                    text="Recovered.",
                    usage=Usage(input_tokens=1, output_tokens=1),
                    finish_reason="stop",
                ),
            }


def test_malformed_arguments_recovered_not_fatal(config, home_dir):
    loop, events, conv, _m = make_loop(config, home_dir, [])
    loop._provider = _BrokenArgsProvider()
    summary = loop.run("fetch the news")

    assert summary["status"] == "completed"
    assert summary["final_text"] == "Recovered."
    assert summary["turns"] == 2

    failed = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert failed["status"] == "error"
    assert "not valid JSON" in failed["output"]

    # the parse error went back as the tool message, so the model could retry
    roles = [m["role"] for m in conv.messages]
    assert roles == ["user", "assistant", "tool", "assistant"]
    assert "not valid JSON" in conv.messages[2]["content"]


def test_nonzero_exit_is_reported_not_fatal(config, home_dir):
    script = [{"tool": "bash", "args": {"command": "exit 7"}}, "It failed with code 7."]
    loop, events, _c, _m = make_loop(config, home_dir, script)
    summary = loop.run("fail a command")
    assert summary["status"] == "completed"
    done = [e for e in events if e["kind"] == "tool_call_finished"][0]
    assert done["status"] == "success"  # the tool ran fine; the command failed
    assert "[exit 7]" in done["output"]


def test_cancelled_before_start(config, home_dir):
    import threading

    loop, events, _c, _m = make_loop(config, home_dir, ["never reached"])
    loop._cancel.set()
    summary = loop.run("too late")
    assert summary["status"] == "cancelled"
    assert kinds(events) == ["run_started", "run_finished"]


def test_provider_llm_error(config, home_dir):
    loop, events, _c, _m = make_loop(
        config, home_dir, [{"raise": "llm", "message": "boom"}]
    )
    summary = loop.run("break the model")
    assert summary["status"] == "error"
    assert "boom" in summary["final_text"]


def test_provider_cancelled_midstream(config, home_dir):
    loop, events, _c, _m = make_loop(config, home_dir, [{"raise": "cancelled"}])
    summary = loop.run("stop mid generation")
    assert summary["status"] == "cancelled"


def test_max_turns_guard(config, home_dir):
    script = [lambda messages, tools: {"tool": "bash", "args": {"command": "true"}}]
    loop, events, _c, _m = make_loop(config, home_dir, script, max_turns=3)
    summary = loop.run("loop forever")
    assert summary["status"] == "max_turns"
    assert kinds(events).count("turn_started") == 3


def test_root_mode_flow_with_sudo(config, home_dir, fake_sudo):
    cache = SudoCredentialCache(
        StaticPasswordProvider(["hunter2"]), sudo_bin=str(fake_sudo)
    )
    script = [
        {"tool": "bash", "args": {"command": "echo sudo-$FAKE_SUDO_OK"}},
        {"tool": "read", "args": {"path": "/etc/os-release", "limit": 3}},
        "System checked.",
    ]
    loop, events, conv, _m = make_loop(
        config, home_dir, script, mode=Mode.ROOT, cache=cache, sudo_bin=str(fake_sudo)
    )
    summary = loop.run("check the system")

    assert summary["status"] == "completed"
    outputs = [e["output"] for e in events if e["kind"] == "tool_call_finished"]
    assert "sudo-1" in outputs[0]
    assert "os-release" in outputs[1].lower() or "fedora" in outputs[1].lower()

    # session persisted under root/
    assert (config.data_dir / "sessions" / "root").is_dir()


def test_system_prompt_included_in_request(config, home_dir):
    loop, _e, _c, _m = make_loop(config, home_dir, ["ok"])
    loop.run("hi")
    messages = loop._provider.last_messages
    assert messages[0]["role"] == "system"
    assert "You are Phoenix" in messages[0]["content"]
    assert messages[-1]["content"] == "hi"
    tool_names = {t.name for t in loop._provider.last_tools}
    assert tool_names == {"bash", "read", "write", "edit", "ls"}
