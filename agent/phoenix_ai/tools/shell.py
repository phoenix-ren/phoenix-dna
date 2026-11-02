# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The bash tool.

* user/web modes: run ``bash -c <command>`` directly in the mode's working dir.
* root mode: wrap in ``sudo -S -p '' -- bash -c <command>`` and feed the
  password (obtained/cached via :mod:`phoenix_ai.privileges`) on stdin.

Subprocesses start in their own session so that cancellation and timeouts
kill the whole process group, not just bash.
"""

from __future__ import annotations

import os
import signal
import subprocess
import threading
import time
from pathlib import Path

from . import ToolError, ToolResult, ToolSpec, validate_required

_AUTH_FAILURE_MARKERS = (
    "incorrect password",
    "a password is required",
    "authentication failure",
    "not in the sudoers file",
)

#: Substrings (lowercased) of commands that failed because they asked a
#: question on stdin - which is closed, so the answer reads as EOF and the
#: command aborts (dnf drops the transaction, flatpak cancels, ...).
_INTERACTIVE_FAILURE_MARKERS = (
    "is this ok [y/n",  # dnf transaction confirmation
    "operation aborted",  # dnf after EOF on the confirmation
    "proceed [y/n",  # flatpak
    "proceed with these changes",  # flatpak (system installation)
    "continue? [y/n",  # apt
    "do you want to continue",  # apt
)

_INTERACTIVE_HINT = (
    "hint: this command failed because it asked for confirmation on stdin, "
    "which is closed and nobody can answer it; re-run with a non-interactive "
    "flag (e.g. dnf -y, flatpak install -y)"
)


def interactive_hint(stdout: str, stderr: str, rc: int) -> str | None:
    """Explain stdin-asking failures: the model reads the hint and retries."""
    if rc == 0:
        return None
    haystack = (stdout + "\n" + stderr).lower()
    if not any(marker in haystack for marker in _INTERACTIVE_FAILURE_MARKERS):
        return None
    return _INTERACTIVE_HINT


def format_output(stdout: str, stderr: str, rc: int, max_chars: int) -> str:
    """Compose a single output string for the model, truncating if needed."""
    parts = []
    if stdout:
        parts.append(stdout.rstrip("\n"))
    if stderr:
        parts.append("stderr:\n" + stderr.rstrip("\n"))
    parts.append(f"[exit {rc}]")
    text = "\n".join(parts)
    if len(text) > max_chars:
        head = int(max_chars * 0.6)
        tail = int(max_chars * 0.35)
        omitted = len(text) - head - tail
        text = (
            text[:head]
            + f"\n... [{omitted} characters truncated] ...\n"
            + text[-tail:]
        )
    return text


def _is_auth_failure(stderr: str, rc: int) -> bool:
    if rc == 0:
        return False
    low = stderr.lower()
    return any(marker in low for marker in _AUTH_FAILURE_MARKERS)


class BashTool:
    spec = ToolSpec(
        name="bash",
        description=(
            "Execute a bash command in the working directory and return "
            "stdout, stderr and the exit code. There is no terminal: stdin "
            "is closed and nobody can answer prompts, so use non-interactive "
            "flags (dnf -y, flatpak install -y, ...) and pass an explicit "
            "timeout for slow commands like package installs."
        ),
        parameters={
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "The bash command to execute"},
                "timeout": {
                    "type": "integer",
                    "description": "Timeout in seconds (optional)",
                    "minimum": 1,
                    "maximum": 600,
                },
            },
            "required": ["command"],
        },
    )

    def __init__(
        self,
        cwd: Path,
        *,
        credential_cache=None,
        sudo_bin: str = "/usr/bin/sudo",
        default_timeout: float = 60.0,
        max_timeout: float = 600.0,
        max_output_chars: int = 20_000,
        extra_env: dict | None = None,
    ):
        self.cwd = Path(cwd)
        self.credential_cache = credential_cache
        self.sudo_bin = sudo_bin
        self.default_timeout = default_timeout
        self.max_timeout = max_timeout
        self.max_output_chars = max_output_chars
        self.extra_env = extra_env or {}

    # -- public API ---------------------------------------------------------

    def execute(self, args: dict, *, cancel: threading.Event | None = None) -> ToolResult:
        validate_required(self.spec, args)
        command = args["command"]
        if not isinstance(command, str) or not command.strip():
            raise ToolError("command must be a non-empty string")
        try:
            timeout = float(args.get("timeout", self.default_timeout))
        except (TypeError, ValueError):
            timeout = self.default_timeout
        timeout = max(1.0, min(timeout, self.max_timeout))

        if self.credential_cache is not None:
            return self._execute_sudo(command, timeout, cancel)
        rc, out, err = self._run(
            ["bash", "-c", command], input_bytes=None, timeout=timeout, cancel=cancel
        )
        return ToolResult(
            output=self._compose_output(out, err, rc),
            details={"command": command, "exit_code": rc},
        )

    def _compose_output(self, out: str, err: str, rc: int) -> str:
        """Formatted output, plus the stdin hint when that is why it failed."""
        text = format_output(out, err, rc, self.max_output_chars)
        hint = interactive_hint(out, err, rc)
        if hint:
            text += "\n" + hint
        return text

    # -- sudo path -----------------------------------------------------------

    def _execute_sudo(self, command: str, timeout: float, cancel) -> ToolResult:
        argv = [self.sudo_bin, "-S", "-p", "", "--", "bash", "-c", command]
        # Two cycles: the first uses the cached/validated password; if sudo
        # reports an auth failure (password changed since caching) we
        # invalidate, re-prompt and retry once.
        for cycle in range(2):
            password = self.credential_cache.get_password()
            if password is None:
                raise ToolError(
                    "root authentication failed: no password provided or attempts exhausted"
                )
            rc, out, err = self._run(
                argv,
                input_bytes=(password + "\n").encode(),
                timeout=timeout,
                cancel=cancel,
            )
            if rc != 0 and _is_auth_failure(err, rc) and cycle == 0:
                self.credential_cache.invalidate()
                continue
            break
        return ToolResult(
            output=self._compose_output(out, err, rc),
            details={"command": command, "exit_code": rc, "sudo": True},
        )

    # -- subprocess plumbing --------------------------------------------------

    def _env(self) -> dict:
        env = dict(os.environ)
        env.update(self.extra_env)
        return env

    def _run(self, argv: list[str], *, input_bytes: bytes | None, timeout: float, cancel):
        try:
            proc = subprocess.Popen(
                argv,
                cwd=str(self.cwd),
                stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                env=self._env(),
            )
        except FileNotFoundError as exc:
            raise ToolError(f"cannot execute {argv[0]}: {exc}") from exc
        except OSError as exc:
            raise ToolError(f"cannot execute command: {exc}") from exc

        deadline = time.monotonic() + timeout
        first = True
        out = err = b""
        while True:
            try:
                out, err = proc.communicate(
                    input_bytes if first else None, timeout=0.25
                )
                break
            except subprocess.TimeoutExpired:
                first = False
                if cancel is not None and cancel.is_set():
                    self._kill_group(proc)
                    proc.communicate()
                    raise ToolError("command cancelled") from None
                if time.monotonic() >= deadline:
                    self._kill_group(proc)
                    proc.communicate()
                    raise ToolError(f"command timed out after {timeout:.0f}s") from None

        return (
            proc.returncode,
            out.decode("utf-8", errors="replace"),
            err.decode("utf-8", errors="replace"),
        )

    @staticmethod
    def _kill_group(proc: subprocess.Popen) -> None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            try:
                proc.kill()
            except OSError:
                pass
