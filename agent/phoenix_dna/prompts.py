# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""System prompt construction.

Kept deliberately short (the pi lesson: frontier models already know how to
be tool-using agents; a fat system prompt only burns context). The prompt has
an identity block, the current date, a per-mode block and shared style rules.
"""

from __future__ import annotations

import datetime as _dt
from pathlib import Path

from .config import Config
from .modes import Mode, ModeProfile

_IDENTITY = (
    "You are Phoenix, a desktop AI agent living in the KDE Plasma panel of a "
    "Fedora Linux machine. You receive one task at a time from your user, "
    "often dictated by voice, so replies should be short and to the point - "
    "they may be read aloud. Do not use Markdown formatting unless the answer "
    "is code or a list."
)

_MODE_BLOCKS = {
    Mode.ROOT: """\
MODE root - system administration.
- Your bash commands are executed as root via sudo; the user has already
  authenticated. The working directory is /.
- Inspect before you change: run status/show/list/read-only variants first.
- Be extremely careful with destructive commands (rm -rf, dd, mkfs, package
  removal, systemctl mask...). Never reboot or shut down unless explicitly
  asked. Do not edit files under /proc, /sys or /dev.
- When you change the system, briefly list what you changed in your final
  answer.""",
    Mode.USER: """\
MODE user - everyday assistant.
- Your bash commands run as the regular user. The working directory is the
  user's home directory (~).
- You may read and edit files, run applications, and use standard CLI tools
  (systemctl --user, nmcli, kdialog, flatpak, git, ...).""",
    Mode.WEB: """\
MODE web - general knowledge and research.
- Answer general-purpose questions. You do not have access to the user's
  personal files in this mode.
- Use the search tools (tavily_search, exa_search) for anything that depends
  on current or specific facts, and cite the source URLs.
- For exact date arithmetic or calculations, compute them with the bash tool
  (date, python3) instead of guessing. The working directory is a private
  empty scratch directory.""",
}

_STYLE = """\
Tool use:
- Call tools one step at a time and reason about each result before the next.
- Commands run without a terminal and stdin is closed: nobody can answer
  interactive prompts, so a command that asks a question just fails. Always
  use non-interactive flags (dnf -y, flatpak install -y, pip --no-input,
  ...) and pass an explicit timeout for slow commands like package installs.
- Tool errors are normal: read the message and adjust, do not repeat a
  failing call unchanged.
- When the task is done, reply with the final answer only - no preamble.

Current date: {now}."""


_CLEANUP_SYSTEM = """\
You clean up raw speech-to-text transcripts so they can be used as prompts \
for a desktop AI agent. Rewrite the transcript exactly as the speaker meant \
it, applying these rules:

- Write number words as digits ("twenty-five" -> "25") and normalize spoken
  math and units to symbols ("times" -> "x", "percent" -> "%",
  "degrees" -> "°"). Example: "Multiply five times twenty-five" becomes
  "Multiply 5 x 25".
- Fix mis-recognized words to what the speaker most plausibly meant.
- Add punctuation and capitalization; drop filler words (um, uh).
- Keep the speaker's language and wording otherwise; never answer, execute
  or expand the transcript.

Reply with the cleaned transcript only - no quotes, no commentary."""


def build_cleanup_messages(text: str) -> list[dict]:
    """Single-shot messages for the transcript-cleanup call (no tools)."""
    return [
        {"role": "system", "content": _CLEANUP_SYSTEM},
        {"role": "user", "content": text},
    ]


_TITLE_SYSTEM = """\
You name a conversation with a desktop AI agent. Reply with a short title \
- at most six words - that captures what the conversation is about, in \
the language the conversation itself uses. No quotes, no trailing period, \
no preamble: the title only."""

#: Per-message truncation and history cap for the title call: a long
#: session must not blow up the naming round-trip.
_TITLE_LINE_LIMIT = 400
_TITLE_MAX_LINES = 24


def build_title_messages(exchanges: list[tuple[str, str]]) -> list[dict]:
    """Single-shot messages asking for a conversation title.

    ``exchanges`` are (role, content) pairs of the conversation so far;
    each is truncated and only the most recent ones are sent.
    """
    lines = []
    for role, content in exchanges:
        content = content.strip()
        if content:
            lines.append(f"{role}: {content[:_TITLE_LINE_LIMIT]}")
    transcript = "\n".join(lines[-_TITLE_MAX_LINES:])
    return [
        {"role": "system", "content": _TITLE_SYSTEM},
        {"role": "user", "content": transcript or "(empty conversation)"},
    ]


def build_system_prompt(profile: ModeProfile, extra_context: str | None = None) -> str:
    now = _dt.datetime.now().strftime("%A, %Y-%m-%d %H:%M")
    parts = [
        _IDENTITY,
        "",
        _MODE_BLOCKS[profile.mode],
        "",
        _STYLE.format(now=now),
    ]
    if extra_context:
        parts += ["", "Additional user instructions:", extra_context.strip()]
    return "\n".join(parts)


def load_context_file(config: Config) -> str | None:
    """Optional extra instructions, AGENTS.md-style (highest priority file wins)."""
    for name in ("PHOENIX.md", "AGENTS.md"):
        path = Path(config.config_dir) / name
        if path.is_file():
            try:
                text = path.read_text(encoding="utf-8").strip()
                if text:
                    return text
            except OSError:
                continue
    return None
