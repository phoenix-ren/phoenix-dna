# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""System prompt construction.

Kept deliberately short (the pi lesson: frontier models already know how to
be tool-using agents; a fat system prompt only burns context). The prompt has
an identity block, the current date, a block about the working context and
shared style rules.
"""

from __future__ import annotations

import datetime as _dt
from pathlib import Path

from .config import Config

_IDENTITY = (
    "You are Phoenix, a desktop AI agent living in the KDE Plasma panel of a "
    "Fedora Linux machine. You receive one task at a time from your user, "
    "often dictated by voice, so replies should be short and to the point - "
    "they may be read aloud. Do not use Markdown formatting unless the answer "
    "is code, a list or math."
)

_CONTEXT_BLOCK = """\
CONTEXT - everyday assistant on this desktop.
- Your bash commands run as the regular user, in the user's home directory
  (~). You may read and edit files, run applications, and use standard CLI
  tools (systemctl --user, nmcli, kdialog, flatpak, git, ...).
- When a task needs administrator rights, run the command with sudo (for
  example "sudo dnf install -y ..."): the user is asked for their password
  and the command then runs as root. Inspect before you change, and never
  run a destructive command (rm -rf, dd, mkfs, package removal, systemctl
  mask...) without the user's confirmation - it is refused when nobody
  confirms it in time."""

#: Appended to the context block when at least one search tool is actually
#: registered. The prompt must not advertise tools the run does not have:
#: the model would otherwise refuse ("I cannot search the web") exactly
#: like it did before the tools were wired in.
_SEARCH_LINE = (
    "- Web search tools ({names}) are available: use them for anything "
    "that depends on current or specific facts, and cite the source URLs."
)

#: Appended when the transfer tool is registered: downloads and uploads
#: belong to it, not to the shell — the advice names the commands the
#: model reaches for (curl, wget, ...) rather than the activity, because
#: the decision it has to interrupt is made in commands. curl stays fine
#: for talking to an API; fetching a file is what makes it the wrong
#: tool.
_TRANSFER_LINE = (
    "- Never move a file with curl, wget, ftp, aria2c or a torrent "
    "client: a download (a web URL, an FTP server, a magnet link) and "
    "an upload alike is a transfer tool call, whatever the size - its "
    "progress is metered for the user while it runs, and a slow "
    "transfer is a tool call with a budget of its own, not a command "
    "that outgrew its timeout. curl asking an API is another matter; "
    "it is fetching a file that makes it the wrong tool."
)

_STYLE = r"""Tool use:
- Call tools one step at a time and reason about each result before the next.
- Commands run without a terminal and stdin is closed: nobody can answer
  interactive prompts, so a command that asks a question just fails. Always
  use non-interactive flags (dnf -y, apt-get -y, flatpak install -y,
  pip --no-input, ...) and pass an explicit timeout for slow commands like
  package installs.
- Tool errors are normal: read the message and adjust, do not repeat a
  failing call unchanged.
- A task that needs decisions only the user can make (which files, which
  disk, encrypted or not, how often) is a task to ask about: put every
  question you already know you need into ONE ask_user call - the
  interface walks the user through it with a progress bar and a back
  button, and all answers return together. Ask follow-ups only when an
  answer opens a decision you could not have written in advance. Do not
  ask about what a sensible default covers - pick the default and
  proceed. Prompts may be read aloud and answered by voice: keep option
  labels short enough to say.
- When the task is done, reply with the final answer only - no preamble.

Math:
- Expressions in mathematical notation go in LaTeX: $...$ for inline
  math, $$...$$ on their own lines for display math ($f'(x)$,
  $\int_0^1 x^2\,dx$) - the interface renders it.

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


def build_system_prompt(
    extra_context: str | None = None,
    tool_names: tuple[str, ...] | list[str] = (),
) -> str:
    """Assemble the system prompt for a run.

    ``tool_names`` are the tools actually registered for the run (the
    daemon passes the built toolset's keys): the search line is appended
    only when a search tool is among them, and the transfer line only
    when the transfer tool is, so the prompt never advertises a tool
    the model cannot call.
    """
    now = _dt.datetime.now().strftime("%A, %Y-%m-%d %H:%M")
    context_block = _CONTEXT_BLOCK
    search_names = [n for n in tool_names if n in ("tavily_search", "exa_search")]
    if search_names:
        context_block += "\n" + _SEARCH_LINE.format(names=", ".join(search_names))
    if "transfer" in tool_names:
        context_block += "\n" + _TRANSFER_LINE
    parts = [
        _IDENTITY,
        "",
        context_block,
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
