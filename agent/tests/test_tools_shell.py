# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Bash tool tests, including the escalation to root via a fake sudo."""

from __future__ import annotations

import threading
import time

import pytest

from phoenix_dna.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_dna.tools import ToolError
from phoenix_dna.tools.shell import (
    BashTool,
    analyse_command,
    format_output,
    interactive_hint,
    needs_elevation,
)


def make_tool(cwd, **kw):
    defaults = dict(default_timeout=10.0, max_timeout=30.0, max_output_chars=2000)
    defaults.update(kw)
    return BashTool(cwd, **defaults)


class RecordingNotifier:
    """Collects the (state, pid, command) reports of a run."""

    def __init__(self):
        self.calls = []

    def __call__(self, state, pid, command):
        self.calls.append((state, pid, command))


def test_process_notifier_reports_start_and_finish(home_dir):
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, process_notifier=notifier)
    tool.execute({"command": "sleep 0.3"})
    assert [state for state, _, _ in notifier.calls] == ["started", "finished"]
    started_pid, finished_pid = notifier.calls[0][1], notifier.calls[1][1]
    assert started_pid > 1 and started_pid == finished_pid
    assert notifier.calls[0][2] == "sleep 0.3"


def test_process_notifier_fires_on_timeout(home_dir):
    # The finish report must also arrive for a killed command, or the UI
    # would wait on a process that no longer exists.
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, default_timeout=1.0, process_notifier=notifier)
    with pytest.raises(ToolError, match="timed out"):
        tool.execute({"command": "sleep 30"})
    assert [state for state, _, _ in notifier.calls] == ["started", "finished"]


def test_process_notifier_fires_on_cancel(home_dir):
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, process_notifier=notifier)
    cancel = threading.Event()
    threading.Timer(0.4, cancel.set).start()
    with pytest.raises(ToolError, match="cancelled"):
        tool.execute({"command": "sleep 30", "timeout": 25}, cancel=cancel)
    assert [state for state, _, _ in notifier.calls] == ["started", "finished"]


def test_broken_notifier_does_not_break_the_tool(home_dir):
    def boom(state, pid, command):
        raise RuntimeError("the UI is having a bad day")

    tool = make_tool(home_dir, process_notifier=boom)
    result = tool.execute({"command": "echo still fine"})
    assert "still fine" in result.output


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
    result = tool.execute({"command": "seq 1 500", "timeout": 10})
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
        {
            "command": "echo 'Is this ok [y/N]:'; echo 'Error: Operation aborted.' >&2; exit 1"
        }
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


# -- escalation to root ---------------------------------------------------------


def sudo_tool(home_dir, fake_sudo, passwords, **kw):
    """A tool that may escalate: it has the credential cache."""
    cache = SudoCredentialCache(
        StaticPasswordProvider(passwords), sudo_bin=str(fake_sudo)
    )
    return make_tool(home_dir, credential_cache=cache, sudo_bin=str(fake_sudo), **kw)


def test_needs_elevation():
    assert needs_elevation("sudo dnf install -y btop")
    assert needs_elevation("make && sudo make install")
    assert needs_elevation("FOO=bar sudo ls")
    assert needs_elevation("then sudo ls")
    # a sudo that is only mentioned, not called
    assert not needs_elevation("echo hello")
    assert not needs_elevation('echo "sudo rm -rf /"')
    assert not needs_elevation("ls -la")


def test_analyse_command_takes_the_sudo_call_out():
    # the command runs under sudo already - its own sudo would ask for a
    # password of its own
    assert analyse_command("sudo dnf install -y btop") == (
        True,
        "dnf install -y btop",
    )
    assert analyse_command("make && sudo make install") == (
        True,
        "make && make install",
    )
    # a call whose flags carry meaning keeps them
    assert analyse_command("sudo -u postgres psql") == (
        True,
        "sudo -u postgres psql",
    )
    assert analyse_command("ls -la") == (False, "ls -la")


def test_sudo_happy_path(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, ["hunter2"])
    result = tool.execute({"command": "sudo echo sudo-ok-$FAKE_SUDO_OK"})
    assert "sudo-ok-1" in result.output
    assert result.details["sudo"] is True


def test_sudo_retries_wrong_password(home_dir, fake_sudo):
    # the first ask fails validation, the second succeeds
    tool = sudo_tool(home_dir, fake_sudo, ["wrong", "hunter2"])
    result = tool.execute({"command": "sudo echo fine-$FAKE_SUDO_OK"})
    assert "fine-1" in result.output


def test_sudo_gives_up_without_password(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, [])
    with pytest.raises(ToolError, match="root authentication failed"):
        tool.execute({"command": "sudo echo nope"})


def test_sudo_timeout(home_dir, fake_sudo):
    tool = sudo_tool(home_dir, fake_sudo, ["hunter2"], default_timeout=1.0)
    with pytest.raises(ToolError, match="timed out"):
        tool.execute({"command": "sudo sleep 30"})


def test_without_a_credential_cache_nothing_escalates(home_dir, fake_sudo):
    """No credential cache: the command runs as the user, sudo and all."""
    tool = make_tool(home_dir, sudo_bin=str(fake_sudo))
    result = tool.execute({"command": "sudo echo plain-$FAKE_SUDO_OK"})
    # the real sudo would ask for a password on a closed stdin and fail;
    # what matters here is that the tool did not escalate on its own
    assert "sudo" not in result.details


def test_format_output_truncation():
    text = format_output("x" * 1000, "", 0, max_chars=100)
    assert "characters truncated" in text
    assert len(text) < 300
    assert format_output("out", "err", 2, 100) == "out\nstderr:\nerr\n[exit 2]"
