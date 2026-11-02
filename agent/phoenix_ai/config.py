# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Configuration loading: defaults < ~/.config/phoenix.ai/agent.toml < environment.

API keys are never stored in this repository. They are read from
``~/.config/phoenix.ai/keys.json`` (mode 0600) or from the environment:

* MiniMax:  ``MINIMAX_API_KEY`` / ``PHOENIX_MINIMAX_API_KEY``
* Tavily:   ``TAVILY_API_KEY``  / ``PHOENIX_TAVILY_API_KEY``
* Exa:      ``EXA_API_KEY``     / ``PHOENIX_EXA_API_KEY``
"""

from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass, field, fields, replace
from pathlib import Path

log = logging.getLogger("phoenix.config")

DEFAULT_BASE_URL = "https://api.minimax.io/v1"
DEFAULT_MODEL = "MiniMax-Text-01"

KEY_ENV = {
    "minimax_api_key": ("MINIMAX_API_KEY", "PHOENIX_MINIMAX_API_KEY"),
    "tavily_api_key": ("TAVILY_API_KEY", "PHOENIX_TAVILY_API_KEY"),
    "exa_api_key": ("EXA_API_KEY", "PHOENIX_EXA_API_KEY"),
}


@dataclass(frozen=True)
class Config:
    # LLM
    provider: str = "minimax"  # "minimax" | "fake"
    llm_base_url: str = DEFAULT_BASE_URL
    llm_api_key: str = ""
    model: str = DEFAULT_MODEL
    request_timeout: float = 120.0
    max_turns: int = 24

    # Dirs / state
    config_dir: Path = field(
        default_factory=lambda: Path("~/.config/phoenix.ai").expanduser()
    )
    data_dir: Path = field(
        default_factory=lambda: Path("~/.local/state/phoenix.ai").expanduser()
    )

    # Privileges / shell
    credential_ttl_seconds: float = 900.0
    sudo_bin: str = "/usr/bin/sudo"
    kdialog_bin: str = "/usr/bin/kdialog"
    bash_default_timeout: float = 60.0
    bash_max_timeout: float = 600.0
    max_output_chars: int = 20_000
    max_read_lines: int = 2000

    # API keys (minimax_api_key / tavily_api_key / exa_api_key)
    keys: dict = field(default_factory=dict)

    def search_keys(self) -> dict:
        """Only the search keys that are actually present."""
        return {
            "tavily_search": self.keys.get("tavily_api_key", ""),
            "exa_search": self.keys.get("exa_api_key", ""),
        }


def _load_keys_file(config_dir: Path) -> dict:
    keys_file = config_dir / "keys.json"
    if not keys_file.is_file():
        return {}
    try:
        data = json.loads(keys_file.read_text())
        if isinstance(data, dict):
            return {k: v for k, v in data.items() if isinstance(v, str)}
    except (OSError, ValueError) as exc:
        log.warning("failed to read %s: %s", keys_file, exc)
    return {}


def _apply_overrides(config: Config, overrides: dict) -> Config:
    valid = {f.name for f in fields(Config)} - {"keys", "config_dir", "data_dir"}
    cleaned = {}
    for key, value in overrides.items():
        if key not in valid:
            log.warning("ignoring unknown setting %r", key)
            continue
        current = getattr(config, key)
        try:
            if isinstance(current, bool):
                value = bool(value)
            elif isinstance(current, int):
                value = int(value)
            elif isinstance(current, float):
                value = float(value)
            elif isinstance(current, Path):
                value = Path(str(value)).expanduser()
            else:
                value = str(value)
        except (TypeError, ValueError):
            log.warning("bad value for %s: %r", key, value)
            continue
        cleaned[key] = value
    return replace(config, **cleaned)


def load_config(
    config_dir: str | Path | None = None,
    data_dir: str | Path | None = None,
    env: dict | None = None,
) -> Config:
    """Build a Config from defaults, keys.json, agent.toml and the environment."""
    env = dict(os.environ) if env is None else dict(env)

    config_dir = Path(
        config_dir
        or env.get("PHOENIX_CONFIG_DIR")
        or Path("~/.config/phoenix.ai").expanduser()
    ).expanduser()
    data_dir = Path(
        data_dir
        or env.get("PHOENIX_DATA_DIR")
        or Path("~/.local/state/phoenix.ai").expanduser()
    ).expanduser()

    keys = _load_keys_file(config_dir)
    for key, (primary, secondary) in KEY_ENV.items():
        value = env.get(secondary) or env.get(primary)
        if value:
            keys[key] = value

    config = Config(
        config_dir=config_dir,
        data_dir=data_dir,
        keys=keys,
        llm_api_key=env.get("PHOENIX_LLM_API_KEY") or keys.get("minimax_api_key", ""),
    )

    # agent.toml overrides
    toml_file = config_dir / "agent.toml"
    if toml_file.is_file():
        import tomllib

        try:
            data = tomllib.loads(toml_file.read_text())
            table = data.get("agent", data)
            if isinstance(table, dict):
                config = _apply_overrides(config, table)
        except (OSError, ValueError) as exc:
            log.warning("failed to parse %s: %s", toml_file, exc)

    # environment overrides
    env_overrides = {}
    if env.get("PHOENIX_PROVIDER"):
        env_overrides["provider"] = env["PHOENIX_PROVIDER"]
    if env.get("PHOENIX_MODEL"):
        env_overrides["model"] = env["PHOENIX_MODEL"]
    if env.get("PHOENIX_BASE_URL"):
        env_overrides["llm_base_url"] = env["PHOENIX_BASE_URL"]
    if env.get("PHOENIX_LLM_API_KEY"):
        env_overrides["llm_api_key"] = env["PHOENIX_LLM_API_KEY"]
    config = _apply_overrides(config, env_overrides)

    config.data_dir.mkdir(parents=True, exist_ok=True)
    config.config_dir.mkdir(parents=True, exist_ok=True)
    return config


def describe(config: Config) -> str:
    """Human-readable summary with masked keys (for --check-config and logs)."""

    def mask(value: str) -> str:
        if not value:
            return "<not set>"
        return value[:6] + "..." if len(value) > 10 else "***"

    lines = [
        f"provider        : {config.provider}",
        f"model           : {config.model}",
        f"llm_base_url    : {config.llm_base_url}",
        f"llm api key     : {mask(config.llm_api_key)}",
        f"tavily api key  : {mask(config.keys.get('tavily_api_key', ''))}",
        f"exa api key     : {mask(config.keys.get('exa_api_key', ''))}",
        f"config dir      : {config.config_dir}",
        f"data dir        : {config.data_dir}",
        f"sudo            : {config.sudo_bin} (credential ttl {config.credential_ttl_seconds:.0f}s)",
        f"kdialog         : {config.kdialog_bin}",
        f"max turns       : {config.max_turns}",
    ]
    return "\n".join(lines)
