# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Search tool tests with a faked HTTP layer (no network)."""

from __future__ import annotations

import json
import urllib.error

import pytest

import phoenix_dna.tools.search as search
from phoenix_dna.tools import ToolError
from phoenix_dna.tools.search import ExaSearchTool, TavilySearchTool


class FakeResponse:
    def __init__(self, payload: bytes):
        self._payload = payload

    def read(self) -> bytes:
        return self._payload

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


@pytest.fixture
def http(monkeypatch):
    """Capture (url, payload, headers) and reply with canned responses."""
    calls = []
    replies: list = []

    def fake_urlopen(request, timeout=None):
        calls.append(
            {
                "url": request.full_url,
                "payload": json.loads(request.data.decode()),
                "headers": dict(request.header_items()),
            }
        )
        if replies:
            reply = replies.pop(0)
            if isinstance(reply, Exception):
                raise reply
            return FakeResponse(json.dumps(reply).encode())
        return FakeResponse(b"{}")

    monkeypatch.setattr(search.urllib.request, "urlopen", fake_urlopen)
    return calls, replies


def test_tavily_success(http):
    calls, replies = http
    replies.append(
        {
            "answer": "Fedora 44 shipped in October 2025.",
            "results": [
                {
                    "title": "Fedora release",
                    "url": "https://fedoraproject.org",
                    "content": "announced",
                }
            ],
        }
    )
    tool = TavilySearchTool("tvly-key")
    result = tool.execute({"query": "fedora 44 release", "max_results": 3})

    assert calls[0]["url"] == "https://api.tavily.com/search"
    assert calls[0]["payload"]["query"] == "fedora 44 release"
    assert calls[0]["payload"]["max_results"] == 3
    assert calls[0]["payload"]["include_answer"] is True
    assert calls[0]["headers"].get("Authorization") == "Bearer tvly-key"
    assert "ANSWER: Fedora 44 shipped" in result.output
    assert "https://fedoraproject.org" in result.output
    assert result.details["raw"]["answer"].startswith("Fedora 44")


def test_tavily_error(http):
    calls, replies = http
    replies.append(urllib.error.HTTPError("url", 429, "rate limited", None, None))
    tool = TavilySearchTool("tvly-key")
    with pytest.raises(ToolError, match="429"):
        tool.execute({"query": "anything"})


def test_exa_success(http):
    calls, replies = http
    replies.append(
        {
            "results": [
                {
                    "title": "Qt 6.11 docs",
                    "url": "https://doc.qt.io",
                    "text": "release notes",
                }
            ]
        }
    )
    tool = ExaSearchTool("exa-key")
    result = tool.execute({"query": "qt 6.11 release notes", "num_results": 2})

    assert calls[0]["url"] == "https://api.exa.ai/search"
    assert calls[0]["payload"]["numResults"] == 2
    assert calls[0]["payload"]["contents"]["text"]["maxCharacters"] == 1500
    assert "Qt 6.11 docs" in result.output
    assert "release notes" in result.output


def test_validation(http):
    tool = TavilySearchTool("k")
    with pytest.raises(ToolError, match="required"):
        tool.execute({})
