# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Shared fixtures and helpers for the phoenix_dna test suite."""

from __future__ import annotations

import sys
import textwrap
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from phoenix_dna.config import Config  # noqa: E402


def wait_for(predicate, timeout: float = 5.0, interval: float = 0.02) -> bool:
    """Poll until predicate() is true; returns False on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


@pytest.fixture
def config(tmp_path) -> Config:
    config = Config(
        provider="fake",
        config_dir=tmp_path / "config",
        data_dir=tmp_path / "state",
        keys={"tavily_api_key": "tvly-test-key", "exa_api_key": "exa-test-key"},
        credential_ttl_seconds=900.0,
        bash_default_timeout=10.0,
        bash_max_timeout=30.0,
        max_output_chars=2000,
        max_read_lines=2000,
    )
    config.config_dir.mkdir(parents=True, exist_ok=True)
    config.data_dir.mkdir(parents=True, exist_ok=True)
    return config


@pytest.fixture
def home_dir(tmp_path) -> Path:
    home = tmp_path / "home"
    home.mkdir()
    return home


@pytest.fixture
def fake_sudo(tmp_path) -> Path:
    """A fake sudo binary that accepts the password 'hunter2'.

    Understands the two invocation shapes phoenix_dna produces:
      sudo -S -p '' -k -v             (validate only)
      sudo -S -p '' -- <command...>   (execute as root)
    """
    script = textwrap.dedent("""\
        #!/usr/bin/env bash
        password=""
        if [[ " $* " == *" -v "* ]]; then
          IFS= read -r password
          if [[ "$password" == "hunter2" ]]; then exit 0; fi
          echo "sudo: 1 incorrect password attempt" >&2
          exit 1
        fi
        while [[ $# -gt 0 && "$1" != "--" ]]; do shift; done
        shift
        IFS= read -r password
        if [[ "$password" != "hunter2" ]]; then
          echo "sudo: 1 incorrect password attempt" >&2
          exit 1
        fi
        export FAKE_SUDO_OK=1
        exec "$@"
        """)
    path = tmp_path / "fake-sudo"
    path.write_text(script)
    path.chmod(0o755)
    return path


@pytest.fixture
def collector():
    """Returns (events_list, emit) - a simple event sink."""
    events: list[dict] = []

    def emit(event: dict) -> None:
        events.append(event)

    return events, emit
