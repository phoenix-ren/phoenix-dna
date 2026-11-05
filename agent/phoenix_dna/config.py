# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Configuration loading: defaults < ~/.config/phoenix-dna/agent.toml < environment.

API keys are never stored in this repository. They are read from
``~/.config/phoenix-dna/keys.json`` (mode 0600) or from the environment:

* MiniMax:  ``MINIMAX_API_KEY`` / ``PHOENIX_MINIMAX_API_KEY``
* OpenAI:   ``OPENAI_API_KEY`` / ``PHOENIX_OPENAI_API_KEY``
* Tavily:   ``TAVILY_API_KEY``  / ``PHOENIX_TAVILY_API_KEY``
* Exa:      ``EXA_API_KEY``     / ``PHOENIX_EXA_API_KEY``
* Speech:   ``PHOENIX_TTS_API_KEY`` / ``TTS_API_KEY`` / keys.json
  "tts_api_key" — else the key of the configured provider is reused
  (see Config.tts_key).

The agent talks to whichever endpoint the user names. ``llm_provider``
picks a set of sensible defaults (base URL, model, which key to read);
``llm_base_url`` and ``model`` override those defaults individually, so
any OpenAI-compatible server can be used by naming its endpoint. The
same holds for synthesis: ``tts_provider`` picks the engine, and
``tts_endpoint`` / ``tts_model`` / ``tts_voice`` override its details.
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

DEFAULT_TTS_ENDPOINT = "https://api.minimax.io/v1/t2a_v2"
DEFAULT_TTS_MODEL = "speech-2.8-turbo"
DEFAULT_TTS_VOICE = "English_expressive_narrator"
DEFAULT_TTS_FORMAT = "opus"

#: The known LLM providers: the name the user picks, the endpoint and
#: model to use with it, and the keys.json field its API key lives in.
#:
#: "minimax" and "openai" are the hosted services; "custom" is for any
#: other OpenAI-compatible server — it has no defaults of its own, so
#: llm_base_url and model must be set for it.
#:
#: A provider only supplies *defaults*. A base URL or a model named
#: explicitly (in agent.toml, the environment, or keys.json) always
#: wins, so a provider can be pointed at a proxy or a self-hosted
#: model without a new entry here.
LLM_PROVIDERS: dict[str, dict] = {
    "minimax": {
        "base_url": "https://api.minimax.io/v1",
        "model": "MiniMax-Text-01",
        "key_field": "minimax_api_key",
        # The model names this provider owns, by family. A model of
        # another provider's family is stale once the provider
        # changes (see load_config).
        "model_families": ("minimax",),
        # How many tokens the model holds at once: the context window
        # the context view measures the conversation against. Zero means
        # "not known" - the user names it (context_window).
        "context_window": 1_000_000,
    },
    "openai": {
        "base_url": "https://api.openai.com/v1",
        "model": "gpt-4o-mini",
        "key_field": "openai_api_key",
        "model_families": ("gpt-", "o1", "o3", "o4", "chatgpt-"),
        "context_window": 128_000,
    },
    "custom": {
        "base_url": "",
        "model": "",
        "key_field": "llm_api_key",
        # "custom" owns no family: it is the user's own server, so
        # whatever model they name is theirs.
        "model_families": (),
        # A server of the user's own: only they know how much its
        # model holds at once.
        "context_window": 0,
    },
    "groq": {
        "base_url": "https://api.groq.com/openai/v1",
        "model": "openai/gpt-oss-120b",
        "key_field": "groq_api_key",
        # Groq serves Llama and the open-weight models, under their own
        # names; "gpt-oss" is the one it hosts under an OpenAI name.
        "model_families": ("llama-", "gpt-oss", "mixtral-", "gemma-"),
        "context_window": 128_000,
    },
    "openrouter": {
        "base_url": "https://openrouter.ai/api/v1",
        # OpenRouter carries hundreds of models, each named "vendor/name".
        # This one is a strong general-purpose one; name another in
        # "model" (the list is at https://openrouter.ai/models) for a
        # different trade-off.
        "model": "anthropic/claude-sonnet-4.5",
        "key_field": "openrouter_api_key",
        # Every model carries its own window, so none is named here.
        "context_window": 0,
        # OpenRouter names every model "vendor/name", so the families
        # are the vendors it carries: a model of one of these belongs
        # to OpenRouter and follows a provider switch.
        "model_families": (
            "anthropic/",
            "openai/",
            "google/",
            "meta-llama/",
            "mistralai/",
            "qwen/",
            "deepseek/",
            "x-ai/",
            "cohere/",
        ),
    },
}

#: The known speech-synthesis providers: the name the user picks, the
#: endpoint, model and voice to use with it, and the keys.json field
#: its API key lives in.
#:
#: "minimax" and "openai" are hosted services with entirely different
#: APIs — MiniMax answers with a hex-encoded SSE stream, OpenAI with a
#: plain audio body (see speech/minimax_engine.py and
#: speech/openai_engine.py). "local" is the on-device engine and needs
#: no key; "none" turns speech off.
#:
#: As with the LLM, a provider supplies only defaults: tts_endpoint,
#: tts_model and tts_voice override them, so any compatible server
#: can be used by naming its endpoint.
TTS_PROVIDERS: dict[str, dict] = {
    "minimax": {
        "endpoint": "https://api.minimax.io/v1/t2a_v2",
        "model": "speech-2.8-turbo",
        "voice": "English_expressive_narrator",
        "format": "opus",
        "key_field": "minimax_api_key",
        # MiniMax answers with Opus, MP3, FLAC or raw PCM.
        "formats": ("opus", "mp3", "flac", "pcm"),
    },
    "openai": {
        "endpoint": "https://api.openai.com/v1/audio/speech",
        "model": "gpt-4o-mini-tts",
        "voice": "alloy",
        # OpenAI's default is MP3, and it is the format every
        # OpenAI-compatible server understands.
        "format": "mp3",
        "key_field": "openai_api_key",
        # OpenAI answers with MP3, Opus, AAC, FLAC, WAV or raw PCM.
        "formats": ("mp3", "opus", "aac", "flac", "wav", "pcm"),
    },
    "groq": {
        # The same OpenAI-shaped speech endpoint, on Groq's own base
        # URL. The model is Orpheus, and the voices are its own.
        "endpoint": "https://api.groq.com/openai/v1/audio/speech",
        "model": "canopylabs/orpheus-v1-english",
        "voice": "troy",
        # Groq's own default is WAV.
        "format": "wav",
        "key_field": "groq_api_key",
        "formats": ("wav", "mp3", "pcm", "flac", "opus"),
    },
    "local": {
        "endpoint": "",
        "model": "",
        "voice": "",
        "format": "",
        "key_field": "",
        "formats": (),
    },
    "openrouter": {
        # The OpenAI-shaped speech endpoint, on OpenRouter's base URL.
        "endpoint": "https://openrouter.ai/api/v1/audio/speech",
        # ElevenLabs' Turbo, which is fast and cheap; the list of what
        # OpenRouter carries is at
        # https://openrouter.ai/models?output_modalities=speech
        "model": "elevenlabs/eleven-turbo-v2",
        "voice": "george",
        # This endpoint answers in mp3 or pcm only.
        "format": "mp3",
        "key_field": "openrouter_api_key",
        "formats": ("mp3", "pcm"),
    },
    "none": {
        "endpoint": "",
        "model": "",
        "voice": "",
        "format": "",
        "key_field": "",
        "formats": (),
    },
}

#: Where each key may come from in the environment. The PHOENIX_-
#: prefixed spelling is listed first because it wins over the plain
#: one, as it does in the C++ side (see shell/transcriber.cpp).
KEY_ENV = {
    "minimax_api_key": ("PHOENIX_MINIMAX_API_KEY", "MINIMAX_API_KEY"),
    "openai_api_key": ("PHOENIX_OPENAI_API_KEY", "OPENAI_API_KEY"),
    "groq_api_key": ("PHOENIX_GROQ_API_KEY", "GROQ_API_KEY"),
    "openrouter_api_key": ("PHOENIX_OPENROUTER_API_KEY", "OPENROUTER_API_KEY"),
    # A dedicated key for the agent's own model calls, for a gateway or
    # a self-hosted server that is none of the hosted providers.
    "llm_api_key": ("PHOENIX_LLM_API_KEY",),
    "tavily_api_key": ("PHOENIX_TAVILY_API_KEY", "TAVILY_API_KEY"),
    "exa_api_key": ("PHOENIX_EXA_API_KEY", "EXA_API_KEY"),
    # A dedicated speech key; absent, synthesis falls back to the
    # key of the configured speech provider (it is the same account
    # either way).
    "tts_api_key": ("PHOENIX_TTS_API_KEY", "TTS_API_KEY"),
}

#: The fields the settings window manages in keys.json (the same
#: entries the plasmoid reads, so both sides agree without a text file
#: in sight). Applied between the defaults and agent.toml: the file and
#: the environment remain the power-user overrides.
#:
#: The endpoint and the model are here, not only in agent.toml: the
#: window writes them, so the window's choice is what the daemon uses.
#: A user who names an endpoint in agent.toml still wins - that file is
#: applied after this one.
_KEYS_FILE_FIELDS = (
    "llm_provider",
    "llm_base_url",
    "model",
    "stt_provider",
    "stt_endpoint",
    "stt_model",
    "tts_provider",
    "tts_endpoint",
    "tts_model",
    "tts_voice",
    "tts_format",
)

#: The tts_* fields an environment variable can override, spelled
#: PHOENIX_TTS_<REST> (e.g. PHOENIX_TTS_ENABLED, PHOENIX_TTS_READ_MODE).
_TTS_ENV_FIELDS = (
    "tts_enabled",
    "tts_provider",
    "tts_model",
    "tts_endpoint",
    "tts_voice",
    "tts_speed",
    "tts_playback_speed",
    "tts_format",
    "tts_read_mode",
    "tts_holdback_sentences",
    "tts_voice_local",
    "tts_local_venv",
)

#: The tts_* fields the settings window manages in keys.json (the same
#: entries the plasmoid reads, so both sides agree without a text file
#: in sight). Applied between the defaults and agent.toml: the file and
#: the environment remain the power-user overrides.
_TTS_KEY_FIELDS = (
    "tts_enabled",
    "tts_provider",
    "tts_voice",
    "tts_speed",
    "tts_playback_speed",
    "tts_format",
    "tts_read_mode",
)

#: The fields the window writes for the *endpoints* and the *models* of
#: the three functions, on top of the tts_* ones above. They live in
#: keys.json too, so the window is enough: the user names an endpoint
#: there and the daemon reads it from the same place.
_ENDPOINT_KEY_FIELDS = (
    "llm_provider",
    "llm_base_url",
    "model",
    "stt_provider",
    "stt_endpoint",
    "stt_model",
    "tts_endpoint",
    "tts_model",
)

#: The fields that describe the model itself rather than where it is
#: reached: how much it holds at once (its context window) and what a
#: million of its tokens cost. They are read from keys.json, agent.toml
#: and the environment; the settings window writes the context window
#: of its own accord.
_MODEL_INFO_KEY_FIELDS = (
    "context_window",
    "price_input",
    "price_output",
)


@dataclass(frozen=True)
class Config:
    # LLM
    provider: str = "minimax"  # "minimax" | "fake"
    #: The hosted service the agent talks to: "minimax" (default),
    #: "openai", or "custom" for any other OpenAI-compatible server.
    #: Only supplies defaults — llm_base_url and model override them.
    llm_provider: str = "minimax"
    llm_base_url: str = DEFAULT_BASE_URL
    llm_api_key: str = ""
    model: str = DEFAULT_MODEL
    request_timeout: float = 120.0
    max_turns: int = 24

    #: How many tokens the model holds at once: its context window.
    #: The context view (the ring in the results window's corner) reads
    #: it to say how much of the window the conversation has used.
    #: Zero means "not known" - the provider's own default fills it in
    #: where there is one, and the user names it otherwise.
    context_window: int = 0

    #: What a million tokens cost, in US dollars: the model's price
    #: for the input it reads and the output it writes. Zero means "not
    #: priced" - the context view then says so instead of quoting a
    #: price of nothing.
    price_input: float = 0.0
    price_output: float = 0.0

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

    # Destructive commands that escalate to root block on a harness-owned
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
    tts_provider: str = "minimax"  # "minimax" | "openai" | "local" | "none"
    tts_model: str = DEFAULT_TTS_MODEL
    tts_endpoint: str = DEFAULT_TTS_ENDPOINT
    tts_voice: str = DEFAULT_TTS_VOICE
    tts_speed: float = 1.0
    #: How fast the synthesized audio is *played back*: 1.0 is the speed
    #: it was spoken at, 1.5 is half again as fast. The stretch keeps
    #: the pitch, so the voice stays the voice (see speech/player.py).
    tts_playback_speed: float = 1.0
    tts_format: str = DEFAULT_TTS_FORMAT  # opus (preferred) | mp3 | flac | pcm
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

    def llm_key(self) -> str:
        """The API key the agent's own model calls use.

        A dedicated ``llm_api_key`` (keys.json, or PHOENIX_LLM_API_KEY in
        the environment) beats the key of the configured provider, so a
        user can point the agent at their own gateway without the
        provider's own key.
        """
        return self.keys.get("llm_api_key", "") or self.llm_api_key

    def tts_key(self) -> str:
        """The API key speech synthesis uses.

        A dedicated ``tts_api_key`` (keys.json, or PHOENIX_TTS_API_KEY /
        TTS_API_KEY in the environment) beats the key of the configured
        speech provider; without one the provider's own key serves,
        because the speech API belongs to the same account.
        """
        dedicated = self.keys.get("tts_api_key", "")
        if dedicated:
            return dedicated
        provider = self.tts_provider.strip().lower()
        if provider == "openai":
            return self.keys.get("openai_api_key", "")
        if provider == "groq":
            return self.keys.get("groq_api_key", "")
        if provider == "openrouter":
            return self.keys.get("openrouter_api_key", "")
        return self.keys.get("minimax_api_key", "")


def _load_keys_file(config_dir: Path) -> dict:
    keys_file = config_dir / "keys.json"
    if not keys_file.is_file():
        return {}
    try:
        data = json.loads(keys_file.read_text())
        if isinstance(data, dict):
            cleaned = {}
            for key, value in data.items():
                if isinstance(value, str):
                    cleaned[key] = value
                elif isinstance(value, bool):
                    # The settings window writes strings, but a
                    # hand-edited file may carry real booleans; the
                    # GUI-managed tts_* settings must survive either.
                    cleaned[key] = "true" if value else "false"
                elif isinstance(value, (int, float)):
                    cleaned[key] = str(value)
            return cleaned
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


#: Which provider an endpoint URL belongs to. The point is that the
#: user can name an endpoint and nothing else: the provider is then
#: worked out from the URL, so the key field, the default model and the
#: default voice follow.
#:
#: Matched on the host, so a proxy in front of a service does not
#: change the answer.
_ENDPOINT_HOSTS = {
    "api.openai.com": "openai",
    "api.groq.com": "groq",
    "openrouter.ai": "openrouter",
    "api.minimax.io": "minimax",
    "api.minimax.chat": "minimax",
}


def provider_for_endpoint(endpoint: str) -> str:
    """The provider an endpoint URL belongs to, or "" when it is none
    of the known ones.

    A URL the user typed is the strongest signal there is: it says
    which service is meant, better than a name they had to pick from a
    list. So a recognised host wins over a remembered provider.
    """
    text = (endpoint or "").strip().lower()
    if not text:
        return ""
    # Strip the scheme and anything after the host: a URL may be
    # "https://api.openai.com/v1" or "api.openai.com/v1".
    host = text.split("://", 1)[-1].split("/", 1)[0].split(":", 1)[0]
    for known, provider in _ENDPOINT_HOSTS.items():
        if host == known or host.endswith("." + known):
            return provider
    return ""


def _belongs_to_other(model: str, provider_name: str) -> bool:
    """Whether a model of that name belongs to a provider other than
    the one in force.

    Used to spot the model that outlived a provider switch: "MiniMax-M3"
    means nothing to OpenAI, and "gpt-4o" means nothing to MiniMax.
    The families are the ones each provider names its models by; a name
    that belongs to no family (the user's own server) belongs to
    nobody, and is left alone.
    """
    name = model.strip().lower()
    for other, spec in LLM_PROVIDERS.items():
        if other == provider_name:
            continue
        for family in spec.get("model_families", ()):
            if name.startswith(family.lower()):
                return True
    return False


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
    for key, names in KEY_ENV.items():
        # The names are listed PHOENIX_-first, because that spelling
        # wins over the plain one; the first one that is set wins.
        value = ""
        for name in names:
            value = env.get(name, "")
            if value:
                break
        if value:
            keys[key] = value

    config = Config(config_dir=config_dir, data_dir=data_dir, keys=keys)

    # What the settings window wrote to keys.json (see _TTS_KEY_FIELDS
    # and _ENDPOINT_KEY_FIELDS): above the defaults, below agent.toml and
    # the environment.
    from_keys = {
        field: keys[field]
        for field in _TTS_KEY_FIELDS + _ENDPOINT_KEY_FIELDS + _MODEL_INFO_KEY_FIELDS
        if keys.get(field)
    }
    config = _apply_overrides(config, from_keys)

    # agent.toml overrides
    toml_file = config_dir / "agent.toml"
    toml_table: dict = {}
    if toml_file.is_file():
        import tomllib

        try:
            data = tomllib.loads(toml_file.read_text())
            table = data.get("agent", data)
            if isinstance(table, dict):
                toml_table = table
                config = _apply_overrides(config, table)
        except (OSError, ValueError) as exc:
            log.warning("failed to parse %s: %s", toml_file, exc)

    # The LLM provider decides the defaults for the base URL, the model
    # and the key — but it must be known before they are applied, and
    # the provider itself can be named by keys.json, agent.toml or the
    # environment. So it is resolved first, here.
    #
    # The order is the one everything else follows: the environment
    # beats agent.toml, which beats keys.json. The toml table is asked
    # directly, because config.llm_provider carries a default and so
    # cannot say whether the user named one.
    provider_name = (
        env.get("PHOENIX_LLM_PROVIDER")
        or toml_table.get("llm_provider")
        or keys.get("llm_provider", "")
        or config.llm_provider
    ).strip().lower()
    if provider_name not in LLM_PROVIDERS:
        log.warning(
            "unknown llm_provider %r; known providers are %s — "
            "falling back to 'minimax'",
            provider_name,
            ", ".join(sorted(LLM_PROVIDERS)),
        )
        provider_name = "minimax"
    provider = LLM_PROVIDERS[provider_name]
    config = replace(config, llm_provider=provider_name)

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
    if env.get("PHOENIX_CONTEXT_WINDOW"):
        env_overrides["context_window"] = env["PHOENIX_CONTEXT_WINDOW"]
    if env.get("PHOENIX_PRICE_INPUT"):
        env_overrides["price_input"] = env["PHOENIX_PRICE_INPUT"]
    if env.get("PHOENIX_PRICE_OUTPUT"):
        env_overrides["price_output"] = env["PHOENIX_PRICE_OUTPUT"]
    for name in _TTS_ENV_FIELDS:
        env_name = "PHOENIX_" + name.upper()
        if env.get(env_name):
            env_overrides[name] = env[env_name]
    config = _apply_overrides(config, env_overrides)

    # An endpoint that names a service is a better signal than the
    # provider name: the user who typed "https://api.openai.com/v1"
    # means OpenAI, whatever the remembered provider says. So a
    # recognised host decides, and the provider follows it — which is
    # what lets the settings window offer nothing but an endpoint.
    #
    # This runs after the overrides, so the endpoint that is actually
    # in force is the one that decides.
    #
    # But only an endpoint the user *named* decides. The built-in
    # default is MiniMax's, and it must not override a provider that
    # was named outright: "llm_provider = openai" with no endpoint
    # means OpenAI, not MiniMax.
    named_endpoint = (
        env.get("PHOENIX_BASE_URL")
        or toml_table.get("llm_base_url")
        or keys.get("llm_base_url", "")
    )
    inferred = provider_for_endpoint(str(named_endpoint or ""))
    if inferred and inferred != provider_name:
        provider_name = inferred
        provider = LLM_PROVIDERS[inferred]
        config = replace(config, llm_provider=inferred)

    # The provider's defaults fill in what the user did not name, and
    # only that: a base URL or a model set in keys.json, agent.toml or
    # the environment is kept, so any OpenAI-compatible server can be
    # used by naming it. This runs last so that an explicit setting
    # always beats the default it replaces.
    #
    # A field is "unset" when it still carries the built-in default of
    # the *first* provider: that value cannot have come from the user,
    # so it may be replaced by the current provider's own default. Each
    # field has its own marker — a base URL left at MiniMax's must not
    # survive a switch to OpenAI, and a model left at MiniMax's must
    # not either.
    unset = {"llm_base_url": DEFAULT_BASE_URL, "model": DEFAULT_MODEL}
    for field, default in (
        ("llm_base_url", provider["base_url"]),
        ("model", provider["model"]),
    ):
        if default and getattr(config, field) == unset[field]:
            config = replace(config, **{field: default})

    # A model that belongs to *another* provider is stale once the
    # provider changes: "MiniMax-M3" means nothing to OpenAI or to a
    # gateway, and the request would be refused. So when the model in
    # force is one that another provider owns, the model follows the
    # provider in force.
    #
    # A model the user typed for their own server is theirs and is
    # left alone: only the model families the providers own are
    # treated as stale. "custom" owns none, so a model named for a
    # self-hosted server always stays.
    if config.model and provider["model"] and _belongs_to_other(
        config.model, provider_name
    ):
        config = replace(config, model=provider["model"])

    # A provider with no defaults of its own ("custom") must not
    # inherit the first provider's: an unset base URL or model there
    # means "not configured", not "MiniMax". The daemon reports it
    # clearly rather than calling somewhere unexpected.
    if not provider["base_url"]:
        if config.llm_base_url == DEFAULT_BASE_URL:
            config = replace(config, llm_base_url="")
    if not provider["model"]:
        if config.model == DEFAULT_MODEL:
            config = replace(config, model="")

    # An endpoint that is none of the known services is the user's own
    # server, so it gets no model from anywhere: "MiniMax-Text-01"
    # would be sent to a gateway that knows nothing about it. The
    # daemon says so plainly instead.
    #
    # Again, only a named endpoint counts: the built-in default is
    # MiniMax's, which is a known service.
    if named_endpoint and not provider_for_endpoint(str(named_endpoint)):
        if config.model == DEFAULT_MODEL:
            config = replace(config, model="")
        if provider_name != "custom":
            provider_name = "custom"
            provider = LLM_PROVIDERS["custom"]
            config = replace(config, llm_provider="custom")

    # The context window: how much the model holds at once. The
    # provider knows the size of the models it serves, so it supplies
    # the default - but only when the user named none, for a model of
    # their own the number is theirs to give.
    #
    # This runs late, after the provider in force is the final one: a
    # server of the user's own must not inherit the window of the
    # service the endpoint used to point at.
    named_context_window = (
        env.get("PHOENIX_CONTEXT_WINDOW", "")
        or str(toml_table.get("context_window", ""))
        or str(keys.get("context_window", ""))
    )
    if not named_context_window and provider.get("context_window"):
        config = replace(config, context_window=int(provider["context_window"]))

    # The key: a dedicated one (llm_api_key in keys.json, or
    # PHOENIX_LLM_API_KEY in the environment) beats the provider's own.
    # PHOENIX_LLM_API_KEY was already applied above as an environment
    # override; what is left is the keys.json entry.
    dedicated = keys.get("llm_api_key", "")
    if dedicated:
        config = replace(config, llm_api_key=dedicated)
    elif not config.llm_api_key:
        config = replace(
            config, llm_api_key=keys.get(provider["key_field"], "")
        )

    # The same for synthesis: the provider named (in keys.json,
    # agent.toml or the environment) brings its own endpoint, model and
    # voice, unless the user named those too.
    tts_provider = config.tts_provider.strip().lower()
    if tts_provider not in TTS_PROVIDERS:
        log.warning(
            "unknown tts_provider %r; known providers are %s — "
            "falling back to 'minimax'",
            tts_provider,
            ", ".join(sorted(TTS_PROVIDERS)),
        )
        tts_provider = "minimax"
        config = replace(config, tts_provider=tts_provider)
    speech = TTS_PROVIDERS[tts_provider]
    # The markers are the defaults of the first provider: a value that
    # still is one of them cannot have come from the user.
    for field, default, marker in (
        ("tts_endpoint", speech["endpoint"], DEFAULT_TTS_ENDPOINT),
        ("tts_model", speech["model"], DEFAULT_TTS_MODEL),
        ("tts_voice", speech["voice"], DEFAULT_TTS_VOICE),
        ("tts_format", speech["format"], DEFAULT_TTS_FORMAT),
    ):
        if default and getattr(config, field) == marker:
            config = replace(config, **{field: default})

    # A speech endpoint that names a service decides the engine, the
    # same way the model's endpoint decides the model provider — but
    # only an endpoint the user named, for the same reason.
    named_speech_endpoint = (
        env.get("PHOENIX_TTS_ENDPOINT")
        or toml_table.get("tts_endpoint")
        or keys.get("tts_endpoint", "")
    )
    inferred = provider_for_endpoint(str(named_speech_endpoint or ""))
    if inferred and inferred in TTS_PROVIDERS and inferred != tts_provider:
        tts_provider = inferred
        speech = TTS_PROVIDERS[inferred]
        config = replace(config, tts_provider=inferred)
        for field, default, marker in (
            ("tts_endpoint", speech["endpoint"], DEFAULT_TTS_ENDPOINT),
            ("tts_model", speech["model"], DEFAULT_TTS_MODEL),
            ("tts_voice", speech["voice"], DEFAULT_TTS_VOICE),
            ("tts_format", speech["format"], DEFAULT_TTS_FORMAT),
        ):
            if default and getattr(config, field) == marker:
                config = replace(config, **{field: default})

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
        f"llm provider    : {config.llm_provider}",
        f"model           : {config.model}",
        f"context window  : {config.context_window or 'unknown'}",
        f"price per 1M    : {config.price_input:g} in / {config.price_output:g} out",
        f"vision          : {config.vision if config.vision is not None else 'auto'}",
        f"llm_base_url    : {config.llm_base_url}",
        f"llm api key     : {mask(config.llm_api_key)}",
        f"tts api key     : {mask(config.tts_key())}",
        f"tavily api key  : {mask(config.keys.get('tavily_api_key', ''))}",
        f"exa api key     : {mask(config.keys.get('exa_api_key', ''))}",
        f"config dir      : {config.config_dir}",
        f"data dir        : {config.data_dir}",
        f"sudo            : {config.sudo_bin} (credential ttl {config.credential_ttl_seconds:.0f}s)",
        f"kdialog         : {config.kdialog_bin}",
        f"max turns       : {config.max_turns}",
        f"ask timeout     : {config.ask_timeout_seconds:.0f}s (0 = wait indefinitely)",
        f"tts             : {'on' if config.tts_enabled else 'off'} "
        f"({config.tts_provider}, {config.tts_read_mode} mode, "
        f"playback speed {config.tts_playback_speed:g}x)",
    ]
    return "\n".join(lines)
