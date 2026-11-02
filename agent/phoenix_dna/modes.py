# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Operating modes: root / user / web.

Each mode fixes the working directory, the toolset and whether bash runs
under sudo.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from .config import Config
from .tools import Tool
from .tools.files import EditTool, LsTool, ReadTool, WriteTool
from .tools.search import ExaSearchTool, TavilySearchTool
from .tools.shell import BashTool


class Mode(str, Enum):
    ROOT = "root"
    USER = "user"
    WEB = "web"


@dataclass(frozen=True)
class ModeProfile:
    mode: Mode
    description: str
    tool_names: tuple[str, ...]
    use_sudo: bool

    def working_dir(self, config: Config, home_dir: Path | None = None) -> Path:
        if self.mode is Mode.ROOT:
            return Path("/")
        if self.mode is Mode.USER:
            return home_dir if home_dir is not None else Path.home()
        return config.data_dir / "scratch" / "web"  # hidden scratch dir


MODE_PROFILES: dict[Mode, ModeProfile] = {
    Mode.ROOT: ModeProfile(
        mode=Mode.ROOT,
        description=(
            "System administration with root privileges; commands run via "
            "sudo. Working directory: /"
        ),
        tool_names=("bash", "read", "write", "edit", "ls"),
        use_sudo=True,
    ),
    Mode.USER: ModeProfile(
        mode=Mode.USER,
        description=(
            "Everyday tasks on this machine as the regular user. "
            "Working directory: your home directory."
        ),
        tool_names=("bash", "read", "write", "edit", "ls"),
        use_sudo=False,
    ),
    Mode.WEB: ModeProfile(
        mode=Mode.WEB,
        description=(
            "General-knowledge questions and web research. No personal file "
            "access; a private scratch directory is used for calculations."
        ),
        tool_names=("bash", "tavily_search", "exa_search"),
        use_sudo=False,
    ),
}


def build_toolset(
    profile: ModeProfile,
    config: Config,
    *,
    credential_cache=None,
    home_dir: Path | None = None,
) -> dict[str, Tool]:
    """Instantiate the tools for a mode run.

    ``credential_cache`` is required for root mode (bash runs under sudo) and
    must be None otherwise. Search tools are only registered when their API
    keys are configured.
    """
    working_dir = profile.working_dir(config, home_dir=home_dir)
    working_dir.mkdir(parents=True, exist_ok=True)  # relevant for web scratch

    if profile.use_sudo and credential_cache is None:
        raise ValueError("root mode requires a credential cache")

    tools: dict[str, Tool] = {}
    for name in profile.tool_names:
        if name == "bash":
            tools["bash"] = BashTool(
                working_dir,
                credential_cache=credential_cache if profile.use_sudo else None,
                sudo_bin=config.sudo_bin,
                default_timeout=config.bash_default_timeout,
                max_timeout=config.bash_max_timeout,
                max_output_chars=config.max_output_chars,
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
    return tools
