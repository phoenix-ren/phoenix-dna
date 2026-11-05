# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The toolset, its construction and the system prompts."""

from __future__ import annotations

import dataclasses

import pytest

from phoenix_dna.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_dna.prompts import build_system_prompt, load_context_file
from phoenix_dna.toolset import TOOL_NAMES, build_toolset, working_dir


def test_tool_names():
    # The agent has the filesystem quartet, the web search pair, the
    # transfer tool and the question tool.
    assert set(TOOL_NAMES) == {
        "bash",
        "read",
        "write",
        "edit",
        "ls",
        "transfer",
        "tavily_search",
        "exa_search",
        "ask_user",
    }


def test_working_dir_is_the_home(home_dir):
    assert working_dir(home_dir) == home_dir
    assert working_dir().is_absolute()


def test_build_toolset(config, home_dir):
    tools = build_toolset(config, home_dir=home_dir)
    # without an ask coordinator there is nobody to deliver answers,
    # so ask_user stays out — but the search tools are in (their keys
    # are configured); each drops out for its own reason
    assert set(tools) == {
        "bash",
        "read",
        "write",
        "edit",
        "ls",
        "transfer",
        "tavily_search",
        "exa_search",
    }
    assert tools["bash"].cwd == home_dir
    # no credential cache: the bash tool cannot escalate to root
    assert tools["bash"].credential_cache is None
    # the transfer tool is always there, and carries the metering hook
    assert tools["transfer"].transfer_notifier is None

    # without search keys the search tools are simply not registered
    stripped = dataclasses.replace(config, keys={})
    assert set(build_toolset(stripped, home_dir=home_dir)) == {
        "bash",
        "read",
        "write",
        "edit",
        "ls",
        "transfer",
    }


def test_build_toolset_wires_transfer_notifier(config, home_dir):
    seen = []

    def notifier(direction, done, total):
        seen.append((direction, done, total))

    tools = build_toolset(
        config, home_dir=home_dir, transfer_notifier=notifier
    )
    assert tools["transfer"].transfer_notifier is notifier


def test_build_toolset_with_ask_coordinator(config, home_dir):
    from phoenix_dna.tools.ask import AskCoordinator

    coordinator = AskCoordinator(emit=lambda event: None)
    tools = build_toolset(
        config,
        home_dir=home_dir,
        run_id="run_ask",
        ask_coordinator=coordinator,
    )
    assert "ask_user" in tools
    assert tools["ask_user"]._run_id == "run_ask"
    assert tools["ask_user"]._timeout == config.ask_timeout_seconds


def test_build_toolset_with_credential_cache(config, fake_sudo):
    """With a credential cache the bash tool may escalate to root."""
    from phoenix_dna.tools.ask import AskCoordinator

    cache = SudoCredentialCache(
        StaticPasswordProvider(["hunter2"]), sudo_bin=str(fake_sudo)
    )
    tools = build_toolset(
        dataclasses.replace(config, sudo_bin=str(fake_sudo)),
        credential_cache=cache,
        ask_coordinator=AskCoordinator(emit=lambda event: None),
    )
    assert set(tools) == {
        "bash",
        "read",
        "write",
        "edit",
        "ls",
        "transfer",
        "tavily_search",
        "exa_search",
        "ask_user",
    }
    assert tools["bash"].credential_cache is cache
    # the destructive-command confirmation rides along
    assert tools["bash"].confirm is not None


def test_system_prompt(config):
    prompt = build_system_prompt()
    assert prompt.startswith("You are Phoenix")
    assert "CONTEXT" in prompt
    assert "Current date:" in prompt
    # the agent drives bash without a terminal: the non-interactive
    # rule must be in the prompt
    assert "non-interactive" in prompt
    assert "dnf -y" in prompt
    assert "apt-get -y" in prompt
    assert "stdin is closed" in prompt
    # math must be asked for in LaTeX delimiters, the spellings the
    # interface's renderer understands
    assert "$...$" in prompt
    assert "$$...$$" in prompt
    # the model must know how to ask for administrator rights
    assert "sudo" in prompt
    assert "destructive" in prompt
    # without registered search tools the prompt must not advertise any
    assert "tavily_search" not in prompt
    assert "exa_search" not in prompt
    # and no transfer tool either: the prompt never advertises a tool
    # the run does not have
    assert "transfer tool" not in prompt
    # the agent can ask the user a question
    assert "ask_user" in prompt

    # With search tools registered (keys configured) they are advertised,
    # so the model reaches for them instead of refusing.
    with_tools = build_system_prompt(tool_names=TOOL_NAMES)
    assert "tavily_search" in with_tools and "exa_search" in with_tools
    # the transfer tool is advertised too, so downloads and uploads go
    # through it rather than through the shell
    assert "transfer tool" in with_tools

    # A subset (only one key configured) is advertised as-is.
    only_tavily = build_system_prompt(tool_names=("bash", "read", "tavily_search"))
    assert "tavily_search" in only_tavily
    assert "exa_search" not in only_tavily

    with_context = build_system_prompt(extra_context="Always reply in rhyme.")
    assert "Always reply in rhyme." in with_context


def test_context_file(config):
    assert load_context_file(config) is None
    (config.config_dir / "PHOENIX.md").write_text("# House rules\nBe terse.")
    assert "Be terse." in load_context_file(config)
