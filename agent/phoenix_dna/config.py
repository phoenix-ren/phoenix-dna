# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Configuration loading: defaults < ~/.config/phoenix-dna/agent.toml < environment.

API keys are never stored in this repository. They are read from
``~/.config/phoenix-dna/keys.json`` (mode 0600) or from the environment:

* MiniMax:  ``MINIMAX_API_KEY`` / ``PHOENIX_MINIMAX_API_KEY``
* Tavily:   ``TAVILY_API_KEY``  / ``PHOENIX_TAVILY_API_KEY``
* Exa:      ``EXA_API_KEY``     / ``PHOENIX_EXA_API_KEY``
* Speech:   ``PHOENIX_TTS_API_KEY`` / ``TTS_API_KEY`` / keys.json
  "tts_api_key" — else the MiniMax key is reused (see Config.tts_key).
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
    # A dedicated speech key; absent, synthesis falls back to the
    # MiniMax key (it is the same account either way).
    "tts_api_key": ("TTS_API_KEY", "PHOENIX_TTS_API_KEY"),
}

#: The tts_* fields an environment variable can override, spelled
#: PHOENIX_TTS_<REST> (e.g. PHOENIX_TTS_ENABLED, PHOENIX_TTS_READ_MODE).
_TTS_ENV_FIELDS = (
    "tts_enabled",
    "tts_provider",
    "tts_model",
    "tts_endpoint",
    "tts_voice",
    "tts_speed",
    "tts_format",
    "tts_read_mode",
    "tts_holdback_sentences",
    "tts_voice_local",
    "tts_local_venv",
)


@dataclass(frozen=True)
class Config:
    # LLM
    provider: str = "minimax"  # "minimax" | "fake"
    llm_base_url: str = DEFAULT_BASE_URL
    llm_api_key: str = ""
    model: str = DEFAULT_MODEL
    request_timeout: float = 120.0
    max_turns: int = 24

    # Whether the model may be sent image parts. None means "decide from
    # the model name" (see attachments.vision_enabled): a text-only model
    # rejects a request that carries an image.
    vision: bool | None = None

    # Dirs / state
    config_dir: Path = field(
        default_factory=lambda: Path("~/.config/phoenix-dna").expanduser()
    )
    data_dir: Path = field(
        default_factory=lambda: Path("~/.local/state/phoenix-dna").expanduser()
    )

    # Privileges / shell
    credential_ttl_seconds: float = 900.0
    sudo_bin: str = "/usr/bin/sudo"
    kdialog_bin: str = "/usr/bin/kdialog"
    bash_default_timeout: float = 60.0
    bash_max_timeout: float = 600.0
    max_output_chars: int = 20_000
    max_read_lines: int = 2000

    # Asking the user: how long an unanswered ask_user question may block
    # the run before the model is told the user may be away (0 = forever).
    ask_timeout_seconds: float = 300.0

    # Root mode: destructive bash commands block on a harness-owned
    # Allow/Deny (fail closed on timeout). Extra regexes that should
    # count as destructive can be configured.
    confirm_destructive: bool = True
    confirm_patterns: list = field(default_factory=list)

    # API keys (minimax_api_key / tavily_api_key / exa_api_key /
    # tts_api_key)
    keys: dict = field(default_factory=dict)

    # Text-to-speech: the daemon reading its own answers aloud (see
    # speech/speaker.py for what the read modes mean). Disabled by
    # default — speech is strictly opt-in, and with it off nothing
    # about the daemon's behaviour changes.
    tts_enabled: bool = False
    tts_provider: str = "minimax"  # "minimax" | "local" | "none"
    tts_model: str = "speech-2.8-turbo"
    tts_endpoint: str = "https://api.minimax.io/v1/t2a_v2"
    tts_voice: str = "English_expressive_narrator"
    tts_speed: float = 1.0
    tts_format: str = "opus"  # opus (preferred) | mp3 | flac | pcm
    tts_read_mode: str = "final"  # "final" | "stream"
    tts_holdback_sentences: int = 1
    #: Kokoro .onnx (voices.json beside it) or Piper .onnx; Path("")
    #: means "not set".
    tts_voice_local: Path = field(default_factory=lambda: Path(""))
    #: The py3.13 venv the local engine's worker runs in (kokoro-onnx
    #: cannot be imported by this daemon's interpreter).
    tts_local_venv: Path = field(default_factory=lambda: Path(""))

    def search_keys(self) -> dict:
        """Only the search keys that are actually present."""
        return {
            "tavily_search": self.keys.get("tavily_api_key", ""),
            "exa_search": self.keys.get("exa_api_key", ""),
        }

    def tts_key(self) -> str:
        """The API key speech synthesis uses.

        A dedicated ``tts_api_key`` (keys.json, or PHOENIX_TTS_API_KEY /
        TTS_API_KEY in the environment) beats the agent's MiniMax key;
        without one the MiniMax key serves, because the speech API is
        MiniMax's as well.
        """
        return self.keys.get("tts_api_key", "") or self.keys.get(
            "minimax_api_key", ""
        )


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
                # A bool from the environment arrives as a string; the
                # words that mean false must not become true.
                if isinstance(value, str):
                    value = value.strip().lower() in ("1", "true", "yes", "on")
                else:
                    value = bool(value)
            elif isinstance(current, list):
                # List settings (e.g. confirm_patterns) take either a
                # single entry or a list of entries, all as strings.
                value = [str(item) for item in value] if isinstance(value, (list, tuple)) else [str(value)]
            elif current is None:
                # A tri-state setting (e.g. "vision"): an empty value or
                # "auto" leaves the automatic behaviour in place.
                if value in ("", None) or str(value).strip().lower() == "auto":
                    value = None
                else:
                    value = str(value).strip().lower() in ("1", "true", "yes", "on")
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
        or Path("~/.config/phoenix-dna").expanduser()
    ).expanduser()
    data_dir = Path(
        data_dir
        or env.get("PHOENIX_DATA_DIR")
        or Path("~/.local/state/phoenix-dna").expanduser()
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
    for name in _TTS_ENV_FIELDS:
        env_name = "PHOENIX_" + name.upper()
        if env.get(env_name):
            env_overrides[name] = env[env_name]
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
        f"vision          : {config.vision if config.vision is not None else 'auto'}",
        f"llm_base_url    : {config.llm_base_url}",
        f"llm api key     : {mask(config.llm_api_key)}",
        f"tavily api key  : {mask(config.keys.get('tavily_api_key', ''))}",
        f"exa api key     : {mask(config.keys.get('exa_api_key', ''))}",
        f"config dir      : {config.config_dir}",
        f"data dir        : {config.data_dir}",
        f"sudo            : {config.sudo_bin} (credential ttl {config.credential_ttl_seconds:.0f}s)",
        f"kdialog         : {config.kdialog_bin}",
        f"max turns       : {config.max_turns}",
        f"ask timeout     : {config.ask_timeout_seconds:.0f}s (0 = wait indefinitely)",
        f"tts             : {'on' if config.tts_enabled else 'off'} "
        f"({config.tts_provider}, {config.tts_read_mode} mode)",
    ]
    return "\n".join(lines)
