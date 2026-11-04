# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Live tests against the real model, speech and search endpoints.

Which provider is exercised is the configured one (see llm_provider), so
the same tests run against MiniMax, OpenAI or any other endpoint the
user has configured. Disabled by default - they cost tokens and need
network. Enable with:

    PHOENIX_LIVE_TEST=1 python -m pytest tests/test_live_apis.py -q -s
"""

from __future__ import annotations

import os

import pytest

pytestmark = pytest.mark.skipif(
    os.environ.get("PHOENIX_LIVE_TEST") != "1",
    reason="set PHOENIX_LIVE_TEST=1 to run live API tests",
)


def _config():
    from phoenix_dna.config import load_config

    config = load_config()
    if not config.llm_api_key:
        pytest.skip(f"no API key configured for the '{config.llm_provider}' provider")
    return config


def test_minimax_streaming_completion():
    from phoenix_dna.provider import OpenAICompatProvider

    config = _config()
    provider = OpenAICompatProvider(
        config.llm_base_url, config.llm_api_key, config.model
    )
    deltas = []
    response = None
    for item in provider.stream(
        [{"role": "user", "content": "Reply with exactly: PHOENIX-OK"}], tools=[]
    ):
        if item["type"] == "text_delta":
            deltas.append(item["text"])
        elif item["type"] == "response":
            response = item["response"]
    assert response is not None
    assert "PHOENIX-OK" in response.text
    assert response.usage is None or response.usage.output_tokens > 0


def test_example_question():
    """The canonical date-arithmetic question, run through the full loop."""
    from phoenix_dna.loop import AgentLoop
    from phoenix_dna.prompts import build_system_prompt
    from phoenix_dna.provider import build_provider
    from phoenix_dna.sessions import SessionManager, SessionStore
    from phoenix_dna.toolset import build_toolset

    config = _config()
    events = []
    manager = SessionManager(SessionStore(config.data_dir / "sessions"))
    loop = AgentLoop(
        run_id="run_live_user",
        provider=build_provider(config),
        tools=build_toolset(config),
        system_prompt=build_system_prompt(),
        conversation=manager.get(),
        session_manager=manager,
        event_sink=lambda e: events.append(e),
    )
    summary = loop.run(
        "How many days until the 5th of November next year? "
        "Compute it exactly with the bash tool (date) and answer briefly."
    )
    print("FINAL:", summary["final_text"])
    tools_used = [e["tool"] for e in events if e["kind"] == "tool_call_started"]
    print("TOOLS USED:", tools_used)
    assert summary["status"] == "completed", summary
    assert summary["final_text"].strip()
    assert "bash" in tools_used  # the model should compute the date


def test_search_tools_live():
    from phoenix_dna.tools.search import ExaSearchTool, TavilySearchTool

    config = _config()
    tavily_key = config.keys.get("tavily_api_key")
    exa_key = config.keys.get("exa_api_key")
    if tavily_key:
        result = TavilySearchTool(tavily_key).execute(
            {"query": "Fedora 44 release date"}
        )
        print("TAVILY:", result.output[:300])
        assert "http" in result.output
    if exa_key:
        result = ExaSearchTool(exa_key).execute({"query": "KDE Plasma 6 release notes"})
        print("EXA:", result.output[:300])
        assert "http" in result.output
