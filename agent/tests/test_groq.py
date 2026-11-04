# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The Groq provider, end to end against a mock server.

Groq serves the OpenAI shape on its own base URL, so the whole point
is that the ordinary OpenAI-compatible machinery serves it unchanged:
the same request shapes, the same answer parsing, the same decoders.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from mock_openai_server import MockOpenAiServer  # noqa: E402

from phoenix_dna.config import load_config  # noqa: E402
from phoenix_dna.provider import build_provider  # noqa: E402
from phoenix_dna.speech.synth import build_engine  # noqa: E402


def _config(tmp_path: Path, server: MockOpenAiServer, **extra):
    cfg = tmp_path / "cfg"
    cfg.mkdir(exist_ok=True)
    (cfg / "keys.json").write_text(
        json.dumps({"llm_provider": "groq", "groq_api_key": "gsk-test", **extra})
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "data", env={})
    # The mock stands in for Groq; the base URL is all that changes.
    return type(config)(
        **{**config.__dict__, "llm_base_url": server.base_url}
    )


def test_the_model_calls_go_to_the_named_endpoint(tmp_path):
    with MockOpenAiServer() as server:
        config = _config(tmp_path, server)
        assert config.llm_provider == "groq"
        assert config.model == "llama-3.3-70b-versatile"

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

        request = server.requests[0]
        assert request["path"] == "/v1/chat/completions"
        assert request["headers"]["authorization"] == "Bearer gsk-test"
        body = json.loads(request["body"])
        assert body["model"] == "llama-3.3-70b-versatile"


def test_the_speech_goes_to_the_named_endpoint(tmp_path):
    with MockOpenAiServer() as server:
        config = _config(tmp_path, server, tts_provider="groq")
        assert config.tts_endpoint == "https://api.groq.com/openai/v1/audio/speech"
        assert config.tts_model == "canopylabs/orpheus-v1-english"

        config = type(config)(
            **{**config.__dict__, "tts_endpoint": f"{server.base_url}/audio/speech"}
        )
        engine = build_engine(config)
        pcm = b"".join(engine.synthesize("one short sentence"))
        assert pcm

        request = server.requests[0]
        assert request["path"] == "/v1/audio/speech"
        assert request["headers"]["authorization"] == "Bearer gsk-test"
        body = json.loads(request["body"])
        assert body["model"] == "canopylabs/orpheus-v1-english"
        engine.close()
