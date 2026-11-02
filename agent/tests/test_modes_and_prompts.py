# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Mode profiles, toolset construction and system prompts."""

from __future__ import annotations

import dataclasses

import pytest

from phoenix_dna.modes import MODE_PROFILES, Mode, ModeProfile, build_toolset
from phoenix_dna.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_dna.prompts import build_system_prompt, load_context_file


def test_profiles():
    assert [m.value for m in MODE_PROFILES] == ["root", "user", "web"]
    assert MODE_PROFILES[Mode.ROOT].use_sudo is True
    assert MODE_PROFILES[Mode.USER].use_sudo is False
    assert MODE_PROFILES[Mode.WEB].use_sudo is False
    assert "tavily_search" in MODE_PROFILES[Mode.WEB].tool_names
    assert set(MODE_PROFILES[Mode.USER].tool_names) == {
        "bash",
        "read",
        "write",
        "edit",
        "ls",
    }


def test_working_dirs(config, home_dir):
    assert MODE_PROFILES[Mode.ROOT].working_dir(config).is_absolute()
    assert MODE_PROFILES[Mode.ROOT].working_dir(config) == MODE_PROFILES[
        Mode.ROOT
    ].working_dir(config, home_dir=home_dir)
    assert MODE_PROFILES[Mode.USER].working_dir(config, home_dir=home_dir) == home_dir
    web_dir = MODE_PROFILES[Mode.WEB].working_dir(config)
    assert web_dir == config.data_dir / "scratch" / "web"


def test_build_toolset_user(config, home_dir):
    tools = build_toolset(MODE_PROFILES[Mode.USER], config, home_dir=home_dir)
    assert set(tools) == {"bash", "read", "write", "edit", "ls"}
    assert tools["bash"].cwd == home_dir
    assert tools["bash"].credential_cache is None


def test_build_toolset_root_requires_cache(config):
    with pytest.raises(ValueError):
        build_toolset(MODE_PROFILES[Mode.ROOT], config)


def test_build_toolset_root(config, fake_sudo):
    cache = SudoCredentialCache(
        StaticPasswordProvider(["hunter2"]), sudo_bin=str(fake_sudo)
    )
    tools = build_toolset(
        MODE_PROFILES[Mode.ROOT],
        dataclasses.replace(config, sudo_bin=str(fake_sudo)),
        credential_cache=cache,
    )
    assert set(tools) == {"bash", "read", "write", "edit", "ls"}
    assert tools["bash"].credential_cache is cache


def test_build_toolset_web(config):
    tools = build_toolset(MODE_PROFILES[Mode.WEB], config)
    assert set(tools) == {"bash", "tavily_search", "exa_search"}
    assert tools["bash"].cwd == config.data_dir / "scratch" / "web"
    assert tools["bash"].cwd.is_dir()

    # without search keys only bash remains
    stripped = dataclasses.replace(config, keys={})
    assert set(build_toolset(MODE_PROFILES[Mode.WEB], stripped)) == {"bash"}


def test_system_prompts(config):
    for mode in Mode:
        prompt = build_system_prompt(MODE_PROFILES[mode])
        assert prompt.startswith("You are Phoenix")
        assert f"MODE {mode.value}" in prompt
        assert "Current date:" in prompt
        # every mode drives bash without a terminal: the non-interactive
        # rule must reach root mode too, not just user mode
        assert "non-interactive" in prompt
        assert "dnf -y" in prompt
        assert "stdin is closed" in prompt

    root = build_system_prompt(MODE_PROFILES[Mode.ROOT])
    assert "sudo" in root and "destructive" in root
    web = build_system_prompt(MODE_PROFILES[Mode.WEB])
    assert "tavily_search" in web and "exa_search" in web

    with_context = build_system_prompt(
        MODE_PROFILES[Mode.USER], extra_context="Always reply in rhyme."
    )
    assert "Always reply in rhyme." in with_context


def test_context_file(config):
    assert load_context_file(config) is None
    (config.config_dir / "PHOENIX.md").write_text("# House rules\nBe terse.")
    assert "Be terse." in load_context_file(config)
