# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The conversation's growth: what a tool adds, what a run may spend, and
the summary that keeps a long conversation inside the model's window.

Checked without a network: the fake provider stands in for the model,
and the store is a real (temporary) one.
"""

from __future__ import annotations

import dataclasses

from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.loop import AgentLoop
from phoenix_dna.provider import FakeProvider, LLMResponse, Usage
from phoenix_dna.prompts import build_system_prompt
from phoenix_dna.sessions import SessionManager, SessionStore
from phoenix_dna.toolset import build_toolset
from phoenix_dna.tools import trim_output


def make_loop(config, home_dir, script, provider=None, **loop_kw):
    cfg = dataclasses.replace(config, sudo_bin=config.sudo_bin)
    tools = build_toolset(cfg, home_dir=home_dir)
    manager = SessionManager(SessionStore(config.data_dir / "sessions"))
    conversation = manager.get()
    events, sink = [], (lambda e: events.append(e))
    provider = provider or FakeProvider(script)
    loop = AgentLoop(
        run_id="run_test",
        provider=provider,
        tools=tools,
        system_prompt=build_system_prompt(tool_names=tuple(tools)),
        conversation=conversation,
        session_manager=manager,
        event_sink=sink,
        **loop_kw,
    )
    return loop, events, conversation, manager


# -- the tool output -------------------------------------------------------


def test_trim_output_keeps_the_head_and_the_tail():
    text = "HEAD" + ("x" * 1000) + "TAIL"

    trimmed = trim_output(text, 100)

    assert trimmed.startswith("HEAD")
    assert trimmed.endswith("TAIL")
    assert "left out" in trimmed
    assert len(trimmed) < len(text)

    # What fits is left alone.
    assert trim_output("short", 100) == "short"
    # A limit of zero means "no limit".
    assert trim_output(text, 0) == text


def test_a_tool_cannot_fill_the_conversation(config, home_dir):
    """What a tool brings back is cut to what the conversation may carry."""
    script = [
        {
            "tool": "bash",
            "args": {"command": "for i in $(seq 1 400); do echo aaaa; done"},
        },
        "Done.",
    ]
    loop, _events, conv, _manager = make_loop(
        config, home_dir, script, tool_output_chars=200
    )

    loop.run("make some noise")

    tool_messages = [m for m in conv.messages if m["role"] == "tool"]
    assert tool_messages
    for message in tool_messages:
        assert len(message["content"]) <= 300
        assert "left out" in message["content"]


# -- the budgets -----------------------------------------------------------


def test_a_run_that_runs_out_of_turns_still_answers(config, home_dir):
    """The turns are spent: the agent answers with what it has."""
    script = [
        {"tool": "bash", "args": {"command": "echo one > one.txt"}},
        {"tool": "bash", "args": {"command": "echo two > two.txt"}},
        # The third call is the wrap-up: the loop makes it when the
        # turns are spent.
        "Here is what I did.",
    ]
    loop, _events, _conv, _manager = make_loop(config, home_dir, script, max_turns=2)

    summary = loop.run("do a couple of things")

    assert summary["status"] == "max_turns"
    # The run ends with an answer, not with nothing: the wrap-up call
    # is the last thing the loop makes.
    assert summary["final_text"] == "Here is what I did."


def test_a_run_that_fills_the_window_stops_before_it_is_refused(config, home_dir):
    """The window is the bound: the run answers before the model refuses."""
    script = [
        {"tool": "bash", "args": {"command": "echo one > one.txt"}},
        # The second call is the wrap-up.
        "Here is where I got to.",
    ]
    loop, _events, _conv, _manager = make_loop(
        config,
        home_dir,
        script,
        context_window=20,
        context_budget=0.5,
        tool_output_chars=200,
    )

    summary = loop.run("keep going")

    # The fake provider reports 11 prompt tokens per request, which is
    # over half of the 20-token window: the run stops there.
    assert summary["status"] == "max_context"
    assert summary["final_text"] == "Here is where I got to."


def test_a_cancelled_run_leaves_no_call_without_an_answer(config, home_dir):
    """A cancelled run must leave the conversation readable.

    A tool call without a result is a conversation the model refuses to
    read, and the next run replays this one.
    """
    script = [
        {
            "tools": [
                {"tool": "bash", "args": {"command": "echo one > one.txt"}},
                {"tool": "bash", "args": {"command": "echo two > two.txt"}},
            ]
        }
    ]
    loop, _events, conv, _manager = make_loop(config, home_dir, script)

    # The run is cancelled between the two calls: the first one is
    # answered, the second one never runs.
    class _CancellingTool:
        """The bash tool, but it cancels the run as it runs."""

        def __init__(self, inner):
            self.inner = inner
            self.spec = inner.spec

        def execute(self, args, *, cancel=None):
            loop._cancel.set()
            return self.inner.execute(args, cancel=cancel)

    bash = loop._tools["bash"]
    loop._tools["bash"] = _CancellingTool(bash)

    summary = loop.run("do two things")

    assert summary["status"] == "cancelled"
    # Every call the assistant made has an answer.
    calls = [
        call["id"]
        for message in conv.messages
        if message.get("tool_calls")
        for call in message["tool_calls"]
    ]
    answers = [
        message.get("tool_call_id")
        for message in conv.messages
        if message["role"] == "tool"
    ]
    assert calls
    assert sorted(calls) == sorted(answers)


# -- the compaction ---------------------------------------------------------


def _store(tmp_path):
    return SessionStore(tmp_path / "users")


def test_the_store_keeps_the_history_and_replays_the_summary(tmp_path):
    """A compacted session replays its summary; its history keeps all."""
    store = _store(tmp_path)
    user = store.local_user
    store.append_message(user, "user", "s1", {"role": "user", "content": "first"})
    store.append_message(
        user, "user", "s1", {"role": "assistant", "content": "an answer"}
    )
    store.append_message(user, "user", "s1", {"role": "user", "content": "second"})

    store.compact(user, "s1", 2, "the first question was about the first thing")

    replay = store.read_messages(user, "s1")
    assert len(replay) == 2
    assert replay[0]["role"] == "user"
    assert "the first question was about" in replay[0]["content"]
    # The message that was kept is the one that came after the cut.
    assert replay[1]["content"] == "second"

    # The history itself keeps everything: the search shows it all.
    rows = store.session_messages(user, "s1")
    assert [row["content"] for row in rows] == ["first", "an answer", "second"]


def test_the_compaction_input_cuts_at_a_user_message(tmp_path):
    store = _store(tmp_path)
    user = store.local_user
    store.append_message(user, "user", "s1", {"role": "user", "content": "one"})
    store.append_message(
        user,
        "user",
        "s1",
        {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {
                    "id": "c1",
                    "type": "function",
                    "function": {"name": "bash", "arguments": "{}"},
                }
            ],
        },
    )
    store.append_message(
        user, "user", "s1", {"role": "tool", "tool_call_id": "c1", "content": "done"}
    )
    store.append_message(user, "user", "s1", {"role": "user", "content": "two"})
    store.append_message(
        user, "user", "s1", {"role": "assistant", "content": "an answer"}
    )

    # A budget of one token keeps only the last message: the cut then
    # moves forward to the user message before it, so that no tool
    # result is left without its call.
    messages, upto = store.compaction_input(user, "s1", 1)
    assert upto > 0
    assert messages
    # The cut is at a user message: the kept part begins with one.
    kept = store.read_messages(user, "s1")
    assert kept[0]["role"] == "user"


def test_the_daemon_summarises_a_conversation_that_outgrew_the_window(tmp_path):
    """The conversation is summarised when it passes the window's share."""
    config = dataclasses.replace(
        _config(tmp_path),
        context_window=1000,
        compact_at=0.5,
    )
    config.config_dir.mkdir(parents=True, exist_ok=True)
    config.data_dir.mkdir(parents=True, exist_ok=True)

    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["an answer"]),
        compaction_provider=FakeProvider(["the summary of the conversation"]),
        emitter=lambda e: events.append(e),
    )
    try:
        # A conversation that is already over the threshold: the
        # summariser is asked for a summary before the run begins.
        store = daemon._sessions.store
        user = store.local_user
        store.append_message(
            user, "user", "s1", {"role": "user", "content": "x" * 8000}
        )
        daemon.submit("a question", session_id="s1")

        # The run happens on the daemon's worker thread.
        assert wait_for(lambda: any(e["kind"] == "compaction" for e in events), timeout=5)
        compactions = [e for e in events if e["kind"] == "compaction"]
        assert compactions[0]["summarised"] > 0

        # The history keeps everything; what the model is sent does not.
        stored = daemon._sessions.store.session_messages(user, "s1")
        assert any(row["content"].startswith("x") for row in stored)
        replay = daemon._sessions.store.read_messages(user, "s1")
        assert any("summary of the conversation" in str(m.get("content"))
                   for m in replay)
    finally:
        daemon.shutdown()


def _config(tmp_path):
    from phoenix_dna.config import Config

    return Config(
        provider="fake",
        config_dir=tmp_path / "config",
        data_dir=tmp_path / "state",
    )
