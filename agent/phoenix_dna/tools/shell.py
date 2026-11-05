# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The bash tool.

The command runs as the desktop user, in the agent's working directory:
``bash -c <command>``, with stdin closed.

A command that asks for administrator rights (a ``sudo`` call) is
escalated on demand: it is wrapped in ``sudo -S -p '' -- bash -c
<command>`` and the password the user typed (obtained/cached via
:mod:`phoenix_dna.privileges`) is fed on stdin. A destructive escalated
command is confirmed by the user first (see :mod:`phoenix_dna.confirm`).

Subprocesses start in their own session so that cancellation and timeouts
kill the whole process group, not just bash.

While a command runs, its process id is reported through an optional
``process_notifier`` callback (state "started"/"finished", the pid and the
command): the daemon turns that into D-Bus signals, and the UI meters the
command's CPU usage from /proc (the pid is the session leader, so the
process group can be followed as a whole).
"""

from __future__ import annotations

import os
import re
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
    "flag (e.g. dnf -y, apt-get -y, flatpak install -y)"
)

#: The command that asks for administrator rights. A command that starts
#: one of its segments with it is escalated on demand: the user is asked
#: for their password and the command then runs as root.
_ELEVATION_COMMAND = "sudo"

#: Words that may stand in front of the command: "then sudo ls" and
#: "time sudo ls" are sudo calls too.
_ELEVATION_WRAPPERS = (
    "!",
    "then",
    "else",
    "do",
    "time",
    "exec",
    "command",
    "nohup",
)

#: Splits a command into the segments a shell would run one after another.
_SEGMENT_SPLIT = re.compile(r";|\n|&&|\|\||\|")

#: What may stand in front of a command: whitespace, environment
#: assignments ("FOO=bar ls") and the wrappers above.
_LEAD = re.compile(
    r"^(?P<lead>(?:\s+|\s*[A-Za-z_][A-Za-z0-9_]*=\S*\s+|\s*(?:"
    + "|".join(re.escape(word) for word in _ELEVATION_WRAPPERS)
    + r")\s+)*)(?P<word>\S+)"
)


def _split_keep(command: str) -> list[tuple[str, str]]:
    """The command's segments, each with the separator that followed it."""
    parts = []
    start = 0
    for match in _SEGMENT_SPLIT.finditer(command):
        parts.append((command[start : match.start()], match.group()))
        start = match.end()
    parts.append((command[start:], ""))
    return parts


def _analyse_segment(segment: str) -> tuple[bool, str]:
    """(asks for root, segment without the sudo call) for one segment."""
    match = _LEAD.match(segment)
    if match is None or match.group("word") != _ELEVATION_COMMAND:
        return False, segment
    rest = segment[match.end() :]
    # A call whose flags carry meaning ("sudo -u postgres psql") keeps
    # them: taking them out would change what the command does.
    if rest.lstrip().startswith("-"):
        return True, segment
    return True, segment[: match.start("word")] + rest.lstrip()


def analyse_command(command: str) -> tuple[bool, str]:
    """(asks for root, the command without its sudo calls).

    A command that calls ``sudo`` needs administrator rights: the daemon
    then asks the user for their password and runs the command as root.
    The command that runs then no longer needs its own ``sudo`` - it would
    be a second one, asking for a password of its own.

    The check is deliberately conservative - a ``sudo`` buried in the
    middle of a segment, or quoted inside another word, is not seen, and
    such a command simply runs as the user (and fails with a permission
    error).
    """
    asks_for_root = False
    parts = []
    for segment, separator in _split_keep(command):
        segment_asks, stripped = _analyse_segment(segment)
        asks_for_root = asks_for_root or segment_asks
        parts.append(stripped)
        parts.append(separator)
    return asks_for_root, "".join(parts)


def needs_elevation(command: str) -> bool:
    """Whether the command asks for administrator rights."""
    return analyse_command(command)[0]


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
            text[:head] + f"\n... [{omitted} characters truncated] ...\n" + text[-tail:]
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
            "flags (dnf -y, apt-get -y, flatpak install -y, ...) and pass an "
            "explicit timeout for slow commands like package installs. A "
            "command that needs administrator rights is run with sudo: the "
            "user is asked for their password, and destructive commands are "
            "confirmed by them before they run. Do NOT download or upload "
            "files with this tool (no curl, wget, ftp or torrent clients): "
            "use the transfer tool, which meters its progress for the user "
            "and outlasts this tool's timeout on big transfers."
        ),
        parameters={
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "The bash command to execute",
                },
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
        confirm=None,
        process_notifier=None,
    ):
        self.cwd = Path(cwd)
        self.credential_cache = credential_cache
        self.sudo_bin = sudo_bin
        self.default_timeout = default_timeout
        self.max_timeout = max_timeout
        self.max_output_chars = max_output_chars
        self.extra_env = extra_env or {}
        # Escalated commands only: a RootCommandConfirmer whose confirm()
        # blocks destructive commands on a harness-owned Allow/Deny. None
        # (no credential cache, or the feature disabled) runs everything as
        # the user, as before.
        self.confirm = confirm
        # Optional process reporting for the UI: called with
        # ("started", pid, command) right after the subprocess exists and
        # ("finished", pid, command) when it ends — normally, by timeout or
        # by cancellation (it runs in a finally block, so it is reliable).
        # Failures in the notifier are swallowed: metering must never
        # disturb the command it meters.
        self.process_notifier = process_notifier

    # -- public API ---------------------------------------------------------

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult:
        validate_required(self.spec, args)
        command = args["command"]
        if not isinstance(command, str) or not command.strip():
            raise ToolError("command must be a non-empty string")
        try:
            timeout = float(args.get("timeout", self.default_timeout))
        except (TypeError, ValueError):
            timeout = self.default_timeout
        timeout = max(1.0, min(timeout, self.max_timeout))

        asks_for_root, privileged = analyse_command(command)
        if asks_for_root and self.credential_cache is not None:
            return self._execute_privileged(privileged, command, timeout, cancel)
        rc, out, err = self._run(
            ["bash", "-c", command],
            input_bytes=None,
            timeout=timeout,
            cancel=cancel,
            label=command,
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

    # -- the privileged path ---------------------------------------------------

    def _execute_privileged(
        self, command: str, original: str, timeout: float, cancel
    ) -> ToolResult:
        """Run a command that asked for administrator rights.

        ``command`` is what runs — the model's own ``sudo`` call is
        taken out of it, the run under sudo replaces it; ``original`` is
        what the model asked for, and that is what the user confirms and
        what the log shows.

        The password the user typed is fed to sudo on stdin. The harness
        confirmation comes first: a denied command must not spend a
        password attempt, and a destructive command nobody confirmed
        (timeout: fail closed) never reaches sudo at all.
        """
        if self.confirm is not None:
            allowed, why = self.confirm.confirm(original, cancel=cancel)
            if not allowed:
                if why == "cancelled":
                    raise ToolError("command cancelled")
                raise ToolError(
                    f"the root command was not run ({why}). Do not retry it "
                    "unchanged: adjust it, or ask the user how to proceed."
                )

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
                label=original,
            )
            if rc != 0 and _is_auth_failure(err, rc) and cycle == 0:
                self.credential_cache.invalidate()
                continue
            break
        return ToolResult(
            output=self._compose_output(out, err, rc),
            details={"command": original, "exit_code": rc, "sudo": True},
        )

    # -- subprocess plumbing --------------------------------------------------

    def _env(self) -> dict:
        env = dict(os.environ)
        env.update(self.extra_env)
        return env

    def _notify(self, state: str, pid: int, label: str) -> None:
        """Report a process event; metering must never disturb the run."""
        try:
            self.process_notifier(state, pid, label)
        except Exception:  # noqa: BLE001 - the UI must not break the tool
            pass

    def _run(
        self,
        argv: list[str],
        *,
        input_bytes: bytes | None,
        timeout: float,
        cancel,
        label: str = "",
    ):
        try:
            proc = subprocess.Popen(
                argv,
                cwd=str(self.cwd),
                stdin=(
                    subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                env=self._env(),
            )
        except FileNotFoundError as exc:
            raise ToolError(f"cannot execute {argv[0]}: {exc}") from exc
        except OSError as exc:
            raise ToolError(f"cannot execute command: {exc}") from exc

        if self.process_notifier is not None:
            self._notify("started", proc.pid, label)
        try:
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
        finally:
            # Also reached on the cancel and timeout paths above, so the
            # UI never waits for a process that is already gone.
            if self.process_notifier is not None:
                self._notify("finished", proc.pid, label)

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
