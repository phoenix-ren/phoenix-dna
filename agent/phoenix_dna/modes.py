# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Operating modes: root / user.

Each mode fixes the working directory, the toolset and whether bash runs
under sudo. Both modes carry the web search tools; they are only
registered when their API keys are configured.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from .config import Config
from .confirm import RootCommandConfirmer
from .tools import Tool
from .tools.ask import AskUserTool
from .tools.files import EditTool, LsTool, ReadTool, WriteTool
from .tools.search import ExaSearchTool, TavilySearchTool
from .tools.shell import BashTool


class Mode(str, Enum):
    ROOT = "root"
    USER = "user"


@dataclass(frozen=True)
class ModeProfile:
    mode: Mode
    description: str
    tool_names: tuple[str, ...]
    use_sudo: bool

    def working_dir(self, config: Config, home_dir: Path | None = None) -> Path:
        if self.mode is Mode.ROOT:
            return Path("/")
        return home_dir if home_dir is not None else Path.home()


#: The tools every mode shares: the filesystem quartet, the web search
#: pair and the question tool. The search tools drop out at build time
#: when their API keys are not configured; ask_user drops out when no
#: coordinator can deliver answers (the daemon always passes one).
_COMMON_TOOLS = (
    "bash",
    "read",
    "write",
    "edit",
    "ls",
    "tavily_search",
    "exa_search",
    "ask_user",
)

MODE_PROFILES: dict[Mode, ModeProfile] = {
    Mode.ROOT: ModeProfile(
        mode=Mode.ROOT,
        description=(
            "System administration with root privileges; commands run via "
            "sudo. Working directory: /"
        ),
        tool_names=_COMMON_TOOLS,
        use_sudo=True,
    ),
    Mode.USER: ModeProfile(
        mode=Mode.USER,
        description=(
            "Everyday tasks on this machine as the regular user, plus "
            "questions that need a web lookup. Working directory: your "
            "home directory."
        ),
        tool_names=_COMMON_TOOLS,
        use_sudo=False,
    ),
}


def build_toolset(
    profile: ModeProfile,
    config: Config,
    *,
    credential_cache=None,
    home_dir: Path | None = None,
    run_id: str = "",
    ask_coordinator=None,
) -> dict[str, Tool]:
    """Instantiate the tools for a mode run.

    ``credential_cache`` is required for root mode (bash runs under sudo) and
    must be None otherwise. Search tools are registered in every mode, but
    only when their API keys are configured; ask_user is only registered
    when an ``ask_coordinator`` is given (the daemon always passes one;
    bare-loop users get the tool only when they can deliver answers).
    """
    working_dir = profile.working_dir(config, home_dir=home_dir)
    working_dir.mkdir(parents=True, exist_ok=True)  # / and ~ exist; cheap insurance

    if profile.use_sudo and credential_cache is None:
        raise ValueError("root mode requires a credential cache")

    tools: dict[str, Tool] = {}
    for name in profile.tool_names:
        if name == "bash":
            # Root mode gates destructive commands on a harness-owned
            # Allow/Deny (see phoenix_dna.confirm): the confirmer blocks
            # through the same ask machinery the model's questions use,
            # so it needs the coordinator and fails closed on timeout.
            confirm = None
            if (
                profile.use_sudo
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
                working_dir,
                credential_cache=credential_cache if profile.use_sudo else None,
                sudo_bin=config.sudo_bin,
                default_timeout=config.bash_default_timeout,
                max_timeout=config.bash_max_timeout,
                max_output_chars=config.max_output_chars,
                confirm=confirm,
            )
        elif name == "read":
            tools["read"] = ReadTool(working_dir, max_lines=config.max_read_lines)
        elif name == "write":
            tools["write"] = WriteTool(working_dir)
        elif name == "edit":
            tools["edit"] = EditTool(working_dir)
        elif name == "ls":
            tools["ls"] = LsTool(working_dir)
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
