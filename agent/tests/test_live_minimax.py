# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Live tests against real MiniMax / Tavily / Exa endpoints.

Disabled by default - they cost tokens and need network. Enable with:

    PHOENIX_LIVE_TEST=1 python -m pytest tests/test_live_minimax.py -q -s
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
        pytest.skip("no MiniMax API key configured")
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


def test_web_mode_example_question():
    """The canonical web-mode question, run through the full agent loop."""
    from phoenix_dna.loop import AgentLoop
    from phoenix_dna.modes import MODE_PROFILES, Mode, build_toolset
    from phoenix_dna.prompts import build_system_prompt
    from phoenix_dna.provider import build_provider
    from phoenix_dna.sessions import SessionManager, SessionStore

    config = _config()
    profile = MODE_PROFILES[Mode.WEB]
    events = []
    loop = AgentLoop(
        run_id="run_live_web",
        mode_name="web",
        provider=build_provider(config),
        tools=build_toolset(profile, config),
        system_prompt=build_system_prompt(profile),
        conversation=SessionManager(SessionStore(config.data_dir / "sessions")).get(
            "web"
        ),
        session_manager=SessionManager(SessionStore(config.data_dir / "sessions")),
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
