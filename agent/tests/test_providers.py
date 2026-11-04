# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for the provider-agnostic configuration and the OpenAI engine.

What is verified here is the plumbing: that choosing a provider brings
its own endpoint, model and key; that an explicitly named endpoint or
model always wins over the provider's default; and that the OpenAI
synthesis engine speaks the right request shape and decodes the reply.

No network and no audio are involved — the HTTP layer and the decoder
are faked, as in test_speech_engines.py.
"""

from __future__ import annotations

import io
import json
import stat
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from phoenix_dna.config import LLM_PROVIDERS, TTS_PROVIDERS, load_config
from phoenix_dna.provider import build_provider
from phoenix_dna.speech import openai_engine


# -- LLM providers -------------------------------------------------------------


def _config(tmp_path: Path, files: dict | None = None, env: dict | None = None):
    """A config from the given keys.json contents and environment."""
    cfg = tmp_path / "cfg"
    cfg.mkdir(exist_ok=True)
    if files:
        (cfg / "keys.json").write_text(json.dumps(files))
    return load_config(
        config_dir=cfg, data_dir=tmp_path / "data", env=env or {}
    )


def test_defaults_still_point_at_minimax(tmp_path):
    config = _config(tmp_path)
    assert config.llm_provider == "minimax"
    assert config.llm_base_url == "https://api.minimax.io/v1"
    assert config.model == "MiniMax-Text-01"


def test_openai_provider_brings_its_own_defaults(tmp_path):
    config = _config(
        tmp_path, {"llm_provider": "openai", "openai_api_key": "sk-openai"}
    )
    assert config.llm_provider == "openai"
    assert config.llm_base_url == "https://api.openai.com/v1"
    assert config.model == "gpt-4o-mini"
    assert config.llm_api_key == "sk-openai"


def test_llm_provider_can_come_from_the_environment(tmp_path):
    config = _config(
        tmp_path, env={"PHOENIX_LLM_PROVIDER": "openai", "OPENAI_API_KEY": "sk-e"}
    )
    assert config.llm_provider == "openai"
    assert config.llm_base_url == "https://api.openai.com/v1"
    assert config.llm_api_key == "sk-e"


def test_an_explicit_endpoint_beats_the_provider_default(tmp_path):
    """The point of the whole exercise: any OpenAI-compatible server."""
    config = _config(
        tmp_path,
        {"llm_provider": "openai"},
        env={"PHOENIX_BASE_URL": "https://my.gateway/v1"},
    )
    assert config.llm_base_url == "https://my.gateway/v1"
    # The model still comes from the provider, since it was not named.
    assert config.model == "gpt-4o-mini"


def test_an_explicit_model_beats_the_provider_default(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "agent.toml").write_text('[agent]\nmodel = "gpt-4o"\n')
    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "data",
        env={"PHOENIX_LLM_PROVIDER": "openai"},
    )
    assert config.model == "gpt-4o"
    assert config.llm_base_url == "https://api.openai.com/v1"


def test_a_dedicated_llm_key_beats_the_provider_key(tmp_path):
    """A gateway with its own credential, next to a provider's key."""
    config = _config(
        tmp_path,
        {
            "llm_provider": "openai",
            "openai_api_key": "sk-provider",
            "llm_api_key": "sk-dedicated",
        },
    )
    assert config.llm_api_key == "sk-dedicated"


def test_a_custom_provider_has_no_defaults_of_its_own(tmp_path):
    """Nothing is inherited from MiniMax: the user must name the
    endpoint and the model, and the daemon says so."""
    config = _config(
        tmp_path, {"llm_provider": "custom", "llm_api_key": "sk-custom"}
    )
    assert config.llm_provider == "custom"
    assert config.llm_base_url == ""
    assert config.model == ""
    assert config.llm_api_key == "sk-custom"

    with pytest.raises(RuntimeError, match="not configured"):
        build_provider(config)


def test_an_unknown_provider_falls_back_to_minimax(tmp_path):
    config = _config(tmp_path, {"llm_provider": "nonsense"})
    assert config.llm_provider == "minimax"
    assert config.llm_base_url == "https://api.minimax.io/v1"


def test_the_provider_follows_the_usual_precedence(tmp_path):
    """agent.toml beats keys.json, and the environment beats both."""
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "keys.json").write_text(json.dumps({"llm_provider": "minimax"}))
    (cfg / "agent.toml").write_text('[agent]\nllm_provider = "openai"\n')

    config = load_config(
        config_dir=cfg, data_dir=tmp_path / "data", env={}
    )
    assert config.llm_provider == "openai"

    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "data",
        env={"PHOENIX_LLM_PROVIDER": "custom"},
    )
    assert config.llm_provider == "custom"


def test_build_provider_uses_the_configured_endpoint(tmp_path):
    config = _config(
        tmp_path,
        {"llm_provider": "openai", "openai_api_key": "sk-openai"},
    )
    provider = build_provider(config)
    assert provider.base_url == "https://api.openai.com/v1"
    assert provider.model == "gpt-4o-mini"
    assert provider.api_key == "sk-openai"


def test_the_minimax_key_still_works_as_before(tmp_path):
    """No regression: the old spelling still resolves the old way."""
    config = _config(tmp_path, {"minimax_api_key": "sk-minimax"})
    assert config.llm_api_key == "sk-minimax"
    assert config.llm_base_url == "https://api.minimax.io/v1"


# -- TTS providers -------------------------------------------------------------


def test_the_openai_speech_provider_brings_its_defaults(tmp_path):
    config = _config(
        tmp_path,
        {"tts_provider": "openai", "openai_api_key": "sk-openai"},
    )
    assert config.tts_provider == "openai"
    assert config.tts_endpoint == "https://api.openai.com/v1/audio/speech"
    assert config.tts_model == "gpt-4o-mini-tts"
    assert config.tts_voice == "alloy"
    assert config.tts_key() == "sk-openai"


def test_an_explicit_voice_beats_the_speech_provider_default(tmp_path):
    config = _config(
        tmp_path,
        {"tts_provider": "openai", "tts_voice": "nova"},
    )
    assert config.tts_voice == "nova"
    assert config.tts_endpoint == "https://api.openai.com/v1/audio/speech"


def test_the_minimax_speech_defaults_are_unchanged(tmp_path):
    config = _config(tmp_path, {"tts_provider": "minimax"})
    assert config.tts_endpoint == "https://api.minimax.io/v1/t2a_v2"
    assert config.tts_model == "speech-2.8-turbo"
    assert config.tts_voice == "English_expressive_narrator"
    assert config.tts_format == "opus"


def test_the_speech_format_follows_the_provider(tmp_path):
    """The two hosted engines prefer different formats; an explicitly
    named one still wins."""
    config = _config(tmp_path, {"tts_provider": "openai"})
    assert config.tts_format == "mp3"

    config = _config(
        tmp_path, {"tts_provider": "openai", "tts_format": "wav"}
    )
    assert config.tts_format == "wav"


def test_the_openrouter_provider_brings_its_own_defaults(tmp_path):
    """OpenRouter carries hundreds of models on the OpenAI shape."""
    config = _config(
        tmp_path, {"llm_provider": "openrouter", "openrouter_api_key": "sk-or-test"}
    )
    assert config.llm_provider == "openrouter"
    assert config.llm_base_url == "https://openrouter.ai/api/v1"
    assert config.model == "anthropic/claude-sonnet-4.5"
    assert config.llm_api_key == "sk-or-test"


def test_the_openrouter_speech_provider_brings_its_own_defaults(tmp_path):
    """OpenRouter's speech endpoint is the OpenAI one on its base URL."""
    config = _config(
        tmp_path, {"tts_provider": "openrouter", "openrouter_api_key": "sk-or-test"}
    )
    assert config.tts_endpoint == "https://openrouter.ai/api/v1/audio/speech"
    assert config.tts_model == "elevenlabs/eleven-turbo-v2"
    assert config.tts_voice == "george"
    assert config.tts_key() == "sk-or-test"


def test_the_speech_key_follows_the_speech_provider(tmp_path):
    """The OpenAI key serves the speech when the provider is OpenAI."""
    config = _config(
        tmp_path,
        {"tts_provider": "openai", "openai_api_key": "sk-openai"},
    )
    assert config.tts_key() == "sk-openai"

    config = _config(
        tmp_path,
        {"tts_provider": "minimax", "minimax_api_key": "sk-minimax"},
    )
    assert config.tts_key() == "sk-minimax"


def test_a_dedicated_speech_key_beats_the_provider_key(tmp_path):
    config = _config(
        tmp_path,
        {
            "tts_provider": "openai",
            "openai_api_key": "sk-openai",
            "tts_api_key": "sk-dedicated",
        },
    )
    assert config.tts_key() == "sk-dedicated"


def test_the_provider_tables_cover_what_the_ui_offers():
    assert set(LLM_PROVIDERS) == {
        "minimax",
        "openai",
        "groq",
        "openrouter",
        "custom",
    }
    assert set(TTS_PROVIDERS) == {
        "minimax",
        "openai",
        "groq",
        "openrouter",
        "local",
        "none",
    }


def test_the_groq_provider_brings_its_own_defaults(tmp_path):
    """Groq serves the OpenAI shape on its own base URL."""
    config = _config(tmp_path, {"llm_provider": "groq", "groq_api_key": "gsk-test"})
    assert config.llm_provider == "groq"
    assert config.llm_base_url == "https://api.groq.com/openai/v1"
    assert config.model == "openai/gpt-oss-120b"
    assert config.llm_api_key == "gsk-test"


def test_the_groq_speech_provider_brings_its_own_defaults(tmp_path):
    """Groq's speech endpoint is the OpenAI one on Groq's base URL."""
    config = _config(tmp_path, {"tts_provider": "groq", "groq_api_key": "gsk-test"})
    assert config.tts_endpoint == "https://api.groq.com/openai/v1/audio/speech"
    assert config.tts_model == "canopylabs/orpheus-v1-english"
    assert config.tts_key() == "gsk-test"


def test_the_requests_carry_a_user_agent(monkeypatch):
    """urllib's own User-Agent is refused by the bot protection some
    hosted APIs sit behind — Groq's Cloudflare answers it with a 403
    and an HTML page, which has nothing to do with the key."""
    from phoenix_dna.http import USER_AGENT

    seen = {}

    class FakeResponse:
        def __iter__(self):
            return iter([b'data: {"choices": [{"delta": {"content": "hi"}}]}\n\n',
                         b'data: [DONE]\n\n'])

        def close(self):
            return None

    def fake_urlopen(request, timeout=0):
        seen["headers"] = request.headers
        return FakeResponse()

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    provider = build_provider(
        type("C", (), {"provider": "x", "llm_api_key": "k",
                       "llm_base_url": "https://example.test/v1",
                       "model": "m", "request_timeout": 30.0})()
    )
    list(provider.stream([{"role": "user", "content": "hi"}], tools=[]))

    agent = seen["headers"].get("User-agent") or seen["headers"].get("User-Agent")
    assert agent == USER_AGENT
    assert "Python-urllib" not in agent


def test_a_model_of_another_provider_follows_the_provider(tmp_path):
    """Switching provider must not leave the old provider's model
    behind: "MiniMax-M3" means nothing to OpenAI or to Groq."""
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "agent.toml").write_text('[agent]\nmodel = "MiniMax-M3"\n')

    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "data",
        env={"PHOENIX_LLM_PROVIDER": "openai"},
    )
    assert config.model == "gpt-4o-mini"

    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "data",
        env={"PHOENIX_LLM_PROVIDER": "groq"},
    )
    assert config.model == "openai/gpt-oss-120b"


def test_a_model_of_ones_own_is_left_alone(tmp_path):
    """A model the user named for their own server is theirs."""
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "agent.toml").write_text('[agent]\nmodel = "my-tuned-model"\n')

    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "data",
        env={"PHOENIX_LLM_PROVIDER": "groq"},
    )
    assert config.model == "my-tuned-model"


# -- the OpenAI synthesis engine ----------------------------------------------


class FakeAudioResponse:
    """The plain audio body the speech endpoint answers with."""

    def __init__(self, frames: list[bytes]):
        self._frames = list(frames)
        self._buffer = io.BytesIO(b"".join(frames))

    def read(self, size: int = -1) -> bytes:
        return self._buffer.read(size)

    def __iter__(self):
        return iter(self._frames)

    def close(self) -> None:
        return None


def _passthrough_decoder(tmp_path: Path) -> Path:
    """A stand-in ffmpeg: copies stdin to stdout whatever the args."""
    script = tmp_path / "fakeffmpeg"
    script.write_text("#!/bin/sh\nexec cat\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


def test_the_openai_engine_speaks_the_documented_request_shape(
    monkeypatch, tmp_path
):
    opened = {}

    def fake_urlopen(request, timeout=0):
        opened["body"] = json.loads(request.data)
        opened["headers"] = request.headers
        opened["url"] = request.full_url
        return FakeAudioResponse([b"\x01\x00" * 100, b"\x02\x00" * 50])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = openai_engine.OpenAiEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    pcm = b"".join(engine.synthesize("one short sentence"))
    assert pcm == b"\x01\x00" * 100 + b"\x02\x00" * 50

    body = opened["body"]
    assert body["model"] == "gpt-4o-mini-tts"
    assert body["input"] == "one short sentence"
    assert body["voice"] == "alloy"
    assert body["response_format"] == "mp3"
    assert opened["headers"]["Authorization"] == "Bearer k"
    assert opened["url"] == "https://api.openai.com/v1/audio/speech"
    engine.close()


def test_the_openai_engine_omits_the_default_speed(monkeypatch, tmp_path):
    """A plain 1.0 is left out: some servers refuse the field."""
    bodies = []

    def fake_urlopen(request, timeout=0):
        bodies.append(json.loads(request.data))
        return FakeAudioResponse([b"\x00\x00"])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = openai_engine.OpenAiEngine(
        api_key="k",
        speed=1.0,
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
    )
    list(engine.synthesize("hello"))
    assert "speed" not in bodies[0]

    engine = openai_engine.OpenAiEngine(
        api_key="k",
        speed=1.25,
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
    )
    list(engine.synthesize("hello"))
    assert bodies[1]["speed"] == 1.25
    engine.close()


def test_the_openai_engine_falls_back_to_mp3(monkeypatch, tmp_path):
    calls = []

    def fake_urlopen(request, timeout=0):
        body = json.loads(request.data)
        calls.append(body["response_format"])
        if body["response_format"] != "mp3":
            raise urllib.error.HTTPError(
                "url",
                400,
                "bad request",
                hdrs=None,
                fp=io.BytesIO(b"unsupported format"),
            )
        return FakeAudioResponse([b"\x03\x00" * 10])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = openai_engine.OpenAiEngine(
        api_key="k",
        audio_format="opus",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
    )
    pcm = b"".join(engine.synthesize("fallback sentence"))
    assert calls == ["opus", "mp3"]
    assert pcm == b"\x03\x00" * 10
    engine.close()


def test_the_openai_engine_reports_a_refusal(monkeypatch, tmp_path):
    def fake_urlopen(request, timeout=0):
        raise urllib.error.HTTPError(
            "url",
            401,
            "unauthorized",
            hdrs=None,
            fp=io.BytesIO(b'{"error": {"message": "bad key"}}'),
        )

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = openai_engine.OpenAiEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    with pytest.raises(openai_engine.SynthesisError, match="401"):
        list(engine.synthesize("nope"))
    engine.close()


def test_the_openai_engine_speaks_a_silent_failure(monkeypatch, tmp_path):
    """An empty body is the worst kind of failure: it must not pass
    for a sentence that was spoken."""

    def fake_urlopen(request, timeout=0):
        return FakeAudioResponse([])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = openai_engine.OpenAiEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    with pytest.raises(openai_engine.SynthesisError, match="no audio"):
        list(engine.synthesize("silent"))
    engine.close()


def test_the_openai_engine_decodes_raw_pcm_without_a_header(
    monkeypatch, tmp_path
):
    """The "pcm" format is raw samples at 24 kHz: ffmpeg has to be told
    the rate, or it cannot read them — and the engine's callers expect
    a single rate, so the samples are converted."""
    commands = []

    def fake_urlopen(request, timeout=0):
        return FakeAudioResponse([b"\x01\x00" * 40])

    real_popen = openai_engine.subprocess.Popen

    def spy_popen(command, *args, **kwargs):
        commands.append(command)
        return real_popen(command, *args, **kwargs)

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    monkeypatch.setattr(openai_engine.subprocess, "Popen", spy_popen)
    engine = openai_engine.OpenAiEngine(
        api_key="k",
        audio_format="pcm",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
    )
    pcm = b"".join(engine.synthesize("raw"))
    assert pcm == b"\x01\x00" * 40
    # The rate the raw samples arrive at is given, so ffmpeg knows
    # what it reads; the engine's own rate is what comes out.
    assert "24000" in commands[0]
    assert str(engine.sample_rate) in commands[0]
    engine.close()
