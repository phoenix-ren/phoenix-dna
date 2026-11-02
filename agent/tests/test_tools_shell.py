# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Bash tool tests, including the sudo (root mode) path via a fake sudo."""

from __future__ import annotations

import threading
import time

import pytest

from phoenix_ai.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_ai.tools import ToolError
from phoenix_ai.tools.shell import BashTool, format_output, interactive_hint


def make_tool(cwd, **kw):
    defaults = dict(default_timeout=10.0, max_timeout=30.0, max_output_chars=2000)
    defaults.update(kw)
    return BashTool(cwd, **defaults)


def test_simple_command(home_dir):
    tool = make_tool(home_dir)
    result = tool.execute({"command": "echo hello"})
    assert "hello" in result.output
    assert result.details["exit_code"] == 0


def test_exit_code_and_stderr(home_dir):
    tool = make_tool(home_dir)
    result = tool.execute({"command": "echo oops >&2; exit 3"})
    assert "[exit 3]" in result.output
    assert "stderr:" in result.output and "oops" in result.output


def test_working_directory(home_dir):
    tool = make_tool(home_dir)
    result = tool.execute({"command": "pwd"})
    assert str(home_dir) in result.output


def test_timeout_kills_process_group(home_dir):
    tool = make_tool(home_dir, default_timeout=1.0)
    start = time.monotonic()
    with pytest.raises(ToolError, match="timed out"):
        tool.execute({"command": "sleep 30"})
    assert time.monotonic() - start < 10


def test_cancel_event_kills(home_dir):
    tool = make_tool(home_dir)
    cancel = threading.Event()
    threading.Timer(0.4, cancel.set).start()
    with pytest.raises(ToolError, match="cancelled"):
        tool.execute({"command": "sleep 30", "timeout": 25}, cancel=cancel)


def test_output_truncation(home_dir):
    tool = make_tool(home_dir, max_output_chars=300)
    result = tool.execute(
        {"command": "seq 1 500", "timeout": 10}
    )
    assert "characters truncated" in result.output
    assert len(result.output) < 1000


def test_missing_required_argument(home_dir):
    with pytest.raises(ToolError, match="command"):
        make_tool(home_dir).execute({"timeout": 5})


def test_interactive_failure_gets_hint(home_dir):
    # stdin is closed, so a command that asks "Is this ok [y/N]" reads EOF
    # and aborts; the tool appends a hint so the model retries with -y.
    tool = make_tool(home_dir)
    result = tool.execute(
        {"command": "echo 'Is this ok [y/N]:'; echo 'Error: Operation aborted.' >&2; exit 1"}
    )
    assert "[exit 1]" in result.output
    assert "hint" in result.output
    assert "dnf -y" in result.output


def test_interactive_hint_only_on_failure():
    assert interactive_hint("Is this ok [y/N]:", "", 1) is not None
    assert interactive_hint("", "Error: Operation aborted.", 1) is not None
    assert interactive_hint("", "Proceed with these changes? [y/n]", 1) is not None
    # a clean exit never hints, whatever the output looks like
    assert interactive_hint("Is this ok [y/N]:", "", 0) is None
    assert interactive_hint("plain failure", "", 1) is None


# -- sudo / root mode ---------------------------------------------------------


def sudo_tool(home_dir, fake_sudo, passwords, **kw):
    cache = SudoCredentialCache(
        StaticPasswordProvider(passwords), sudo_bin=str(fake_sudo)
    )
    return make_tool(home_dir, credential_cache=cache, sudo_bin=str(fake_sudo), **kw)


def test_sudo_happy_path(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, ["hunter2"])
    result = tool.execute({"command": "echo sudo-ok-$FAKE_SUDO_OK"})
    assert "sudo-ok-1" in result.output
    assert result.details["sudo"] is True


def test_sudo_retries_wrong_password(home_dir, fake_sudo):
    # the first ask fails validation, the second succeeds
    tool = sudo_tool(home_dir, fake_sudo, ["wrong", "hunter2"])
    result = tool.execute({"command": "echo fine-$FAKE_SUDO_OK"})
    assert "fine-1" in result.output


def test_sudo_gives_up_without_password(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, [])
    with pytest.raises(ToolError, match="root authentication failed"):
        tool.execute({"command": "echo nope"})


def test_sudo_timeout(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, ["hunter2"], default_timeout=1.0)
    with pytest.raises(ToolError, match="timed out"):
        tool.execute({"command": "sleep 30"})


def test_format_output_truncation():
    text = format_output("x" * 1000, "", 0, max_chars=100)
    assert "characters truncated" in text
    assert len(text) < 300
    assert format_output("out", "err", 2, 100) == "out\nstderr:\nerr\n[exit 2]"
