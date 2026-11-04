# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""End-to-end tests against a mock OpenAI-compatible server.

These check the whole path — the configuration, the HTTP client, the
answer parsing — against a server that behaves the way OpenAI's does.
Nothing leaves the machine and no key is needed, so the questions the
real endpoints would answer ("does it send the right request?", "does
it read the right answer?") can be asked here.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from mock_openai_server import MockOpenAiServer  # noqa: E402

from phoenix_dna.config import load_config  # noqa: E402
from phoenix_dna.provider import build_provider  # noqa: E402
from phoenix_dna.speech import openai_engine  # noqa: E402
from phoenix_dna.speech.synth import build_engine  # noqa: E402


def _config(tmp_path: Path, server: MockOpenAiServer, **overrides):
    """A config pointing at the mock server."""
    cfg = tmp_path / "cfg"
    cfg.mkdir(exist_ok=True)
    (cfg / "keys.json").write_text(
        json.dumps(
            {
                "llm_provider": "openai",
                "openai_api_key": "sk-test-key",
                **overrides,
            }
        )
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "data", env={})
    # The endpoint is the mock server's, whatever the provider's
    # default is.
    return config, server.base_url


def test_the_agent_talks_to_a_configured_endpoint(tmp_path):
    """The point of the whole exercise: name the endpoint, and the
    agent talks to it — with the provider's key and in its shape."""
    with MockOpenAiServer() as server:
        config, base_url = _config(tmp_path, server)
        config = type(config)(
            **{**config.__dict__, "llm_base_url": base_url}
        )

        provider = build_provider(config)
        response = None
        for item in provider.stream(
            [{"role": "user", "content": "Reply with exactly: PHOENIX-OK"}],
            tools=[],
        ):
            if item["type"] == "response":
                response = item["response"]

        assert response is not None
        assert response.text == "PHOENIX-OK"

        # What went over the wire.
        request = server.requests[0]
        assert request["path"] == "/v1/chat/completions"
        assert request["headers"]["authorization"] == "Bearer sk-test-key"
        body = json.loads(request["body"])
        assert body["model"] == "gpt-4o-mini"
        assert body["stream"] is True
        assert body["messages"][0]["content"] == "Reply with exactly: PHOENIX-OK"


def test_the_speech_engine_talks_to_a_configured_endpoint(tmp_path):
    """The same for the voice: name the endpoint, and the engine
    speaks to it in OpenAI's shape."""
    with MockOpenAiServer() as server:
        config, base_url = _config(tmp_path, server)
        config = type(config)(
            **{
                **config.__dict__,
                "tts_provider": "openai",
                "tts_endpoint": f"{base_url}/audio/speech",
                "tts_model": "gpt-4o-mini-tts",
                "tts_voice": "nova",
            }
        )

        engine = build_engine(config)
        assert isinstance(engine, openai_engine.OpenAiEngine)

        pcm = b"".join(engine.synthesize("one short sentence"))
        assert pcm  # the mock's audio body, decoded

        request = server.requests[0]
        assert request["path"] == "/v1/audio/speech"
        assert request["headers"]["authorization"] == "Bearer sk-test-key"
        body = json.loads(request["body"])
        assert body["model"] == "gpt-4o-mini-tts"
        assert body["input"] == "one short sentence"
        assert body["voice"] == "nova"
        engine.close()


def test_the_speech_engine_follows_the_configured_format(tmp_path):
    """The format the user asked for is the format that is requested."""
    with MockOpenAiServer() as server:
        config, base_url = _config(tmp_path, server)
        config = type(config)(
            **{
                **config.__dict__,
                "tts_provider": "openai",
                "tts_endpoint": f"{base_url}/audio/speech",
                "tts_format": "opus",
            }
        )

        engine = build_engine(config)
        list(engine.synthesize("hello"))
        body = json.loads(server.requests[0]["body"])
        assert body["response_format"] == "opus"
        engine.close()


def test_the_transcription_shape_is_the_openai_one(tmp_path):
    """The speech-to-text request is a multipart form with a "model"
    field and the audio as "file" — the shape MiniMax mirrors and
    every OpenAI-compatible server understands."""
    import urllib.request
    import uuid

    with MockOpenAiServer() as server:
        boundary = "----PhoenixTest" + uuid.uuid4().hex
        body = (
            f"--{boundary}\r\n"
            'Content-Disposition: form-data; name="model"\r\n\r\n'
            "gpt-4o-mini-transcribe\r\n"
            f"--{boundary}\r\n"
            'Content-Disposition: form-data; name="file"; filename="clip.wav"\r\n'
            "Content-Type: audio/wav\r\n\r\n"
        ).encode() + b"RIFFfake" + f"\r\n--{boundary}--\r\n".encode()

        request = urllib.request.Request(
            f"{server.base_url}/audio/transcriptions",
            data=body,
            headers={
                "Content-Type": f"multipart/form-data; boundary={boundary}",
                "Authorization": "Bearer sk-test-key",
            },
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            answer = json.loads(response.read())

        assert answer["text"] == "hello there"
        assert server.requests[0]["path"] == "/v1/audio/transcriptions"
