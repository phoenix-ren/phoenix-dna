# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The agent's toolset: the files, the shell, the transfers and the
questions.

The agent works as the desktop user, in their home directory. Commands
that need administrator rights are escalated on demand (see
:mod:`phoenix_dna.tools.shell`): the user is asked for their password
and the command then runs as root.

The web search tools drop out at build time when their API keys are not
configured; ask_user drops out when no coordinator can deliver answers
(the daemon always passes one).
"""

from __future__ import annotations

from pathlib import Path

from .config import Config
from .confirm import RootCommandConfirmer
from .tools import Tool
from .tools.ask import AskUserTool
from .tools.files import EditTool, LsTool, ReadTool, WriteTool
from .tools.search import ExaSearchTool, TavilySearchTool
from .tools.shell import BashTool
from .tools.transfer import TransferTool

#: The tools the agent has: the filesystem quartet, the web search pair,
#: the transfer tool and the question tool.
TOOL_NAMES = (
    "bash",
    "read",
    "write",
    "edit",
    "ls",
    "transfer",
    "tavily_search",
    "exa_search",
    "ask_user",
)


def working_dir(home_dir: Path | None = None) -> Path:
    """The directory the agent works in: the user's home."""
    return home_dir if home_dir is not None else Path.home()


def build_toolset(
    config: Config,
    *,
    credential_cache=None,
    home_dir: Path | None = None,
    run_id: str = "",
    ask_coordinator=None,
    process_notifier=None,
    transfer_notifier=None,
) -> dict[str, Tool]:
    """Instantiate the tools for a run.

    ``credential_cache`` is what lets a command escalate to root: without
    it the bash tool runs everything as the user. Search tools are
    registered only when their API keys are configured; ask_user is only
    registered when an ``ask_coordinator`` is given (the daemon always
    passes one; bare-loop users get the tool only when they can deliver
    answers). ``process_notifier`` receives the started/finished pid of
    every command the bash tool runs, and ``transfer_notifier`` the byte
    counts of every transfer, so the UI can meter both.
    """
    workdir = working_dir(home_dir)
    workdir.mkdir(parents=True, exist_ok=True)  # ~ exists; cheap insurance

    tools: dict[str, Tool] = {}
    for name in TOOL_NAMES:
        if name == "bash":
            # Destructive commands are gated on a harness-owned Allow/Deny
            # (see phoenix_dna.confirm): the confirmer blocks through the
            # same ask machinery the model's questions use, so it needs
            # the coordinator and fails closed on timeout.
            confirm = None
            if (
                credential_cache is not None
                and config.confirm_destructive
                and ask_coordinator is not None
            ):
                confirm = RootCommandConfirmer(
                    ask_coordinator,
                    run_id,
                    timeout=config.ask_timeout_seconds,
                    extra_patterns=config.confirm_patterns,
                )
            tools["bash"] = BashTool(
                workdir,
                credential_cache=credential_cache,
                sudo_bin=config.sudo_bin,
                default_timeout=config.bash_default_timeout,
                max_timeout=config.bash_max_timeout,
                max_output_chars=config.max_output_chars,
                confirm=confirm,
                process_notifier=process_notifier,
            )
        elif name == "read":
            tools["read"] = ReadTool(workdir, max_lines=config.max_read_lines)
        elif name == "write":
            tools["write"] = WriteTool(workdir)
        elif name == "edit":
            tools["edit"] = EditTool(workdir)
        elif name == "ls":
            tools["ls"] = LsTool(workdir)
        elif name == "transfer":
            # The metered mover of bytes: downloads (web, FTP, magnets)
            # and uploads, reported to the UI as they happen.
            tools["transfer"] = TransferTool(
                workdir, transfer_notifier=transfer_notifier
            )
        elif name == "tavily_search":
            key = config.keys.get("tavily_api_key", "")
            if key:
                tools["tavily_search"] = TavilySearchTool(key)
        elif name == "exa_search":
            key = config.keys.get("exa_api_key", "")
            if key:
                tools["exa_search"] = ExaSearchTool(key)
        elif name == "ask_user":
            if ask_coordinator is not None:
                tools["ask_user"] = AskUserTool(
                    ask_coordinator,
                    run_id=run_id,
                    timeout=config.ask_timeout_seconds,
                )
    return tools
