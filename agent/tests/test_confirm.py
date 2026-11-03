# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Harness-owned confirmation of destructive root commands: the screen,
the fail-closed confirmer, the bash gate, and the daemon flow."""

from __future__ import annotations

import dataclasses
import json
import os
import threading
from pathlib import Path

import pytest

from conftest import wait_for

from phoenix_dna.confirm import RootCommandConfirmer, destructive_reason
from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.privileges import SudoCredentialCache, StaticPasswordProvider
from phoenix_dna.provider import FakeProvider
from phoenix_dna.tools import ToolError
from phoenix_dna.tools.ask import AskCoordinator
from phoenix_dna.tools.shell import BashTool


DESTRUCTIVE = [
    ("rm -rf /var/log/old", "recursive forced deletion"),
    ("rm -fr ~/tmp/junk", "recursive forced deletion"),
    ("sudo rm -r -f /home/u/x", "recursive forced deletion"),
    ("rm -f -r /home/u/x", "recursive forced deletion"),
    ("rm --recursive --force /home/u/x", "recursive forced deletion"),
    ("mkfs.ext4 /dev/sdb1", "filesystem format"),
    ("dd if=image.iso of=/dev/sda bs=4M", "raw write to a device"),
    ("wipefs -a /dev/sdb", "disk erase"),
    ("shred /dev/nvme0n1", "disk erase"),
    ("systemctl mask nginx.service", "system unit masked or disabled"),
    ("dnf remove -y nginx", "package removal"),
    ("apt-get purge firefox", "package removal"),
    ("echo x > /dev/sda", "write to a device node"),
    ("parted /dev/sda rm 3", "partition table change"),
    ("rm -rf / --no-preserve-root", "recursive forced deletion"),
]

BENIGN = [
    "echo hello",
    "rm notes.txt",  # no -r/-f
    "rm -i old.txt",  # interactive, not forced
    "cat /var/log/messages",
    "dnf install -y btop",  # install, not removal
    "systemctl status nginx",
    "dd if=/dev/zero of=/tmp/file bs=1k count=1",  # writes a file, not a device
    "df -h",
]


def test_destructive_screen_matches():
    for command, expected in DESTRUCTIVE:
        reason = destructive_reason(command)
        assert reason is not None, f"expected a reason for: {command}"
        assert reason == expected, command


def test_destructive_screen_ignores_benign():
    for command in BENIGN:
        assert destructive_reason(command) is None, command


def test_extra_patterns_and_bad_ones():
    reason = destructive_reason("fleetctl drift", extra=[r"fleetctl\s+drift"])
    assert reason == "matched a configured pattern"
    # a regex that does not compile is dropped, not fatal
    assert destructive_reason("rm -rf /x", extra=["([bad"]) is not None


# -- the confirmer -------------------------------------------------------------


def make_confirmer(events, **kwargs):
    coordinator = AskCoordinator(emit=lambda event: events.append(event))
    return (
        RootCommandConfirmer(coordinator, "run_c", **kwargs),
        coordinator,
    )


def answer_when_asked(coordinator, events, payload):
    def run():
        if wait_for(
            lambda: [e for e in events if e["kind"] == "question_asked"], timeout=5
        ):
            asked = [e for e in events if e["kind"] == "question_asked"][0]
            coordinator.answer(asked["ask_id"], json.dumps(payload))

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread


def test_confirm_question_is_harness_authored():
    events: list[dict] = []
    confirmer, coordinator = make_confirmer(events)

    thread = answer_when_asked(coordinator, events, {"selected": [0]})
    allowed, why = confirmer.confirm("rm -rf /tmp/scratch")
    thread.join(timeout=5)

    assert allowed is True
    asked = [e for e in events if e["kind"] == "question_asked"][0]
    question = asked["question"]
    # the load-bearing properties: daemon-authored, fixed options
    assert question["harness"] is True
    assert [o["label"] for o in question["options"]] == ["Allow", "Deny"]
    assert "rm -rf /tmp/scratch" in question["question"]
    assert question["header"] == "Root command"


def test_confirm_denied():
    events: list[dict] = []
    confirmer, coordinator = make_confirmer(events)

    thread = answer_when_asked(coordinator, events, {"selected": [1]})
    allowed, why = confirmer.confirm("mkfs.vfat /dev/sdc1")
    thread.join(timeout=5)

    assert allowed is False
    assert "denied" in why


def test_confirm_timeout_denies_fail_closed():
    events: list[dict] = []
    confirmer, _coordinator = make_confirmer(events, timeout=0.15)

    allowed, why = confirmer.confirm("dd if=x of=/dev/sda")

    assert allowed is False
    assert "fail closed" in why
    assert [e for e in events if e["kind"] == "question_answered"][0][
        "ask_status"
    ] == "timeout"


def test_confirm_skips_benign_commands_without_asking():
    events: list[dict] = []
    confirmer, _coordinator = make_confirmer(events)

    allowed, why = confirmer.confirm("echo hello > /tmp/f")

    assert allowed is True
    assert why == ""
    assert [e for e in events if e["kind"] == "question_asked"] == []


# -- the bash gate --------------------------------------------------------------


def make_root_bash(fake_sudo, confirm, passwords=("hunter2",)):
    cache = SudoCredentialCache(
        StaticPasswordProvider(list(passwords)), sudo_bin=str(fake_sudo)
    )
    return BashTool(
        Path("/tmp"),
        credential_cache=cache,
        sudo_bin=str(fake_sudo),
        default_timeout=10.0,
        max_timeout=30.0,
        max_output_chars=2000,
        confirm=confirm,
    )


def test_bash_denied_command_never_touches_sudo(config, fake_sudo):
    events: list[dict] = []
    # A short timeout, nobody answering: deny. The password provider is
    # empty on purpose — if the gate leaked to sudo, the tool would fail
    # with an authentication error instead of the denial.
    confirmer, _coordinator = make_confirmer(events, timeout=1.0)

    bash = make_root_bash(fake_sudo, confirmer, passwords=())
    with pytest.raises(ToolError, match="not run"):
        bash.execute({"command": "rm -rf /usr/local/junk"})

    asked = [e for e in events if e["kind"] == "question_asked"]
    assert asked, "the destructive command should have asked first"
    assert [e for e in events if e["kind"] == "question_answered"][0][
        "ask_status"
    ] == "timeout"


def test_bash_allowed_command_runs_as_root(config, fake_sudo):
    events: list[dict] = []
    confirmer, coordinator = make_confirmer(events)

    thread = answer_when_asked(coordinator, events, {"selected": [0]})
    result = make_root_bash(fake_sudo, confirmer).execute(
        {"command": "echo sudo-$FAKE_SUDO_OK > /tmp/confirm-marker"}
    )
    thread.join(timeout=5)

    # the echo's output went to the marker file; the proof of root is there
    assert "[exit 0]" in result.output
    with open("/tmp/confirm-marker") as handle:
        assert handle.read() == "sudo-1\n"
    os.remove("/tmp/confirm-marker")


# -- the daemon flow -------------------------------------------------------------


def events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def test_daemon_root_confirm_denied(config, fake_sudo, tmp_path):
    cfg = dataclasses.replace(config, sudo_bin=str(fake_sudo))
    events: list[dict] = []
    daemon = AgentDaemon(
        cfg,
        provider=FakeProvider(
            [
                {"tool": "bash", "args": {"command": "rm -rf /var/cache/junk"}},
                "Cleaned up anyway.",
            ]
        ),
        password_provider=StaticPasswordProvider(["hunter2"]),
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit("clean the cache", "root")

        assert wait_for(lambda: events_of(events, "question_asked"), timeout=5)
        asked = events_of(events, "question_asked")[0]
        assert asked["question"]["harness"] is True
        assert asked["question"]["options"][0]["label"] == "Allow"

        assert (
            daemon.answer_question(asked["ask_id"], '{"selected": [1]}') is True
        )

        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        finished = events_of(events, "run_finished")[0]
        assert finished["status"] == "completed"
        # the model was told the command was not run
        denied = events_of(events, "tool_call_finished")[0]
        assert denied["status"] == "error"
        assert "not run" in denied["output"]
        answered = events_of(events, "question_answered")[0]
        assert answered["ask_status"] == "answered"
    finally:
        daemon.shutdown()


def test_daemon_root_confirm_disabled_runs_directly(config, fake_sudo):
    cfg = dataclasses.replace(
        config, sudo_bin=str(fake_sudo), confirm_destructive=False
    )
    events: list[dict] = []
    daemon = AgentDaemon(
        cfg,
        provider=FakeProvider(
            [
                {"tool": "bash", "args": {"command": "echo sudo-$FAKE_SUDO_OK"}},
                "Done.",
            ]
        ),
        password_provider=StaticPasswordProvider(["hunter2"]),
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit("clean the cache", "root")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        assert events_of(events, "question_asked") == []
        assert "sudo-1" in events_of(events, "tool_call_finished")[0]["output"]
    finally:
        daemon.shutdown()


def test_daemon_root_benign_command_does_not_ask(config, fake_sudo):
    cfg = dataclasses.replace(config, sudo_bin=str(fake_sudo))
    events: list[dict] = []
    daemon = AgentDaemon(
        cfg,
        provider=FakeProvider(
            [
                {"tool": "bash", "args": {"command": "echo sudo-$FAKE_SUDO_OK"}},
                "Done.",
            ]
        ),
        password_provider=StaticPasswordProvider(["hunter2"]),
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit("check", "root")
        assert wait_for(lambda: events_of(events, "run_finished"), timeout=5)
        assert events_of(events, "question_asked") == []
    finally:
        daemon.shutdown()
