# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Harness-owned confirmation for destructive root commands.

The ask_user questions the *model* asks are model-authored — fine for
clarifications ("which disk?"), but consent to run a destructive command
as root must not be: a prompt-injected option list could manufacture it.
So the screen lives here, daemon-side, and the question it asks carries
the ``harness`` marker: fixed Allow/Deny options built from the command,
never from anything the model wrote.

Fail-closed is the load-bearing property: a timeout or a cancellation
denies the command, unlike a model question whose timeout tells the model
to proceed with its best judgment.
"""

from __future__ import annotations

import re
import threading

from .tools.ask import AskCoordinator

#: (pattern, reason) — a command matching a pattern needs confirmation.
#: Deliberately coarse: a false positive only asks; a false negative
#: runs quietly, so the patterns err towards matching. Extra patterns
#: from the config (``confirm_patterns``) are appended.
#:
#: The rm patterns need recursion *and* force, in any shape: combined
#: flags (``-rf``, ``-fr``), separate flags (``-r -f``) and long forms
#: (``--recursive --force``). Recursion alone prompts (stdin is closed,
#: so it fails rather than deletes) and force alone only skips prompts
#: for write-protected files.
DEFAULT_PATTERNS: tuple[tuple[str, str], ...] = (
    (
        r"\brm\s+(-{1,2}[a-z]*r[a-z]*f[a-z]*|-{1,2}[a-z]*f[a-z]*r[a-z]*)\b",
        "recursive forced deletion",
    ),
    (
        r"\brm\s+-{1,2}[a-z]*r[a-z]*\b[^;|&]*\s+-{1,2}[a-z]*f[a-z]*\b",
        "recursive forced deletion",
    ),
    (
        r"\brm\s+-{1,2}[a-z]*f[a-z]*\b[^;|&]*\s+-{1,2}[a-z]*r[a-z]*\b",
        "recursive forced deletion",
    ),
    (r"--no-preserve-root", "deletion across the filesystem root"),
    (r"\bmkfs(\.\w+)?\b", "filesystem format"),
    (r"\bdd\b[^;|&]*\bof=/dev/", "raw write to a device"),
    (r"\b(shred|wipefs|blkdiscard)\b", "disk erase"),
    (r"\bsystemctl\s+(mask|disable)\b", "system unit masked or disabled"),
    (
        r"\b(dnf|yum|microdnf|apt|apt-get|pacman|zypper)\s+"
        r"(remove|erase|purge|uninstall)\b",
        "package removal",
    ),
    (r">\s*/dev/(sd|nvme|vd|mmcblk)", "write to a device node"),
    (r"\bparted\b[^;|&]*\b(rm|mklabel)\b", "partition table change"),
)


def compiled_patterns(
    extra: list[str] | tuple[str, ...] = (),
) -> list[tuple[re.Pattern, str]]:
    """The default screen plus configured extras, as (pattern, reason)."""
    compiled = [
        (re.compile(pattern, re.IGNORECASE), reason) for pattern, reason in DEFAULT_PATTERNS
    ]
    for pattern in extra:
        try:
            compiled.append(
                (re.compile(pattern, re.IGNORECASE), "matched a configured pattern")
            )
        except re.error:
            continue  # a bad configured pattern must not break the screen
    return compiled


def destructive_reason(
    command: str, extra: list[str] | tuple[str, ...] = ()
) -> str | None:
    """Why the command is considered destructive, or None when it is not."""
    haystack = command if len(command) <= 4000 else command[:4000]
    for pattern, reason in compiled_patterns(extra):
        if pattern.search(haystack):
            return reason
    return None


class RootCommandConfirmer:
    """Blocks a destructive root command on a harness-owned Allow/Deny."""

    def __init__(
        self,
        coordinator: AskCoordinator,
        run_id: str,
        *,
        timeout: float | None = None,
        extra_patterns: list[str] | tuple[str, ...] = (),
    ):
        self._coordinator = coordinator
        self._run_id = run_id
        self._timeout = timeout
        self._patterns = compiled_patterns(extra_patterns)

    def needs_confirmation(self, command: str) -> str | None:
        """The reason the command must be confirmed, or None."""
        haystack = command if len(command) <= 4000 else command[:4000]
        for pattern, reason in self._patterns:
            if pattern.search(haystack):
                return reason
        return None

    def confirm(
        self, command: str, *, cancel: threading.Event | None = None
    ) -> tuple[bool, str]:
        """(allowed, why) — the command runs only when allowed is True.

        The question is harness-authored: the command and the reason it
        matched are shown in the question text, the options are fixed,
        and the payload carries ``harness: true`` so frontends can style
        it as a system confirmation. Deny is the outcome of everything
        but an explicit Allow: a Deny pick, a timeout (fail closed) or a
        cancellation.
        """
        reason = self.needs_confirmation(command)
        if reason is None:
            return True, ""

        question = {
            "question": f"Allow this root command to run?\n"
            f"Reason: {reason}\n\n$ {command}",
            "header": "Root command",
            "options": [
                {"label": "Allow", "description": "Run it once, as root"},
                {"label": "Deny", "description": "Skip it and tell me why"},
            ],
            "harness": True,
        }
        answer, status = self._coordinator.ask(
            self._run_id, question, cancel=cancel, timeout=self._timeout
        )

        if status == "answered" and answer.get("selected") == ["Allow"]:
            return True, "allowed by the user"
        if status == "cancelled":
            return False, "cancelled"
        if status == "timeout":
            return False, "nobody confirmed it in time (fail closed)"
        return False, "denied by the user"
