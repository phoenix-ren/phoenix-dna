# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Web search tools (web mode): Tavily and Exa.

Both are simple JSON-over-HTTPS POST APIs, so they are implemented with
urllib - no SDK dependency. If a key is missing, the corresponding tool is
simply not registered for web mode.
"""

from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request

from . import ToolError, ToolResult, ToolSpec, validate_required


def _snippet(text: str, limit: int = 400) -> str:
    text = " ".join((text or "").split())
    return text[:limit] + ("..." if len(text) > limit else "")


def _post_json(url: str, payload: dict, api_key: str, timeout: float = 30.0) -> dict:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
            "x-api-key": api_key,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as exc:
        try:
            body = (exc.read() or b"")[:200].decode(errors="replace")
        except Exception:
            body = ""
        raise ToolError(f"search failed: HTTP {exc.code} {body}".rstrip()) from exc
    except urllib.error.URLError as exc:
        raise ToolError(f"search failed: {exc.reason}") from exc
    except ValueError as exc:
        raise ToolError(f"search returned malformed JSON: {exc}") from exc


class TavilySearchTool:
    spec = ToolSpec(
        name="tavily_search",
        description=(
            "Search the web with Tavily. Best for research questions: returns "
            "a short synthesized answer plus result snippets with URLs."
        ),
        parameters={
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "The search query"},
                "max_results": {
                    "type": "integer",
                    "description": "Number of results (1-10, default 5)",
                    "minimum": 1,
                    "maximum": 10,
                },
            },
            "required": ["query"],
        },
    )

    def __init__(self, api_key: str, base_url: str = "https://api.tavily.com"):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult:
        validate_required(self.spec, args)
        query = args["query"]
        max_results = max(1, min(int(args.get("max_results", 5)), 10))
        data = _post_json(
            self.base_url + "/search",
            {
                "query": query,
                "max_results": max_results,
                "include_answer": True,
            },
            self.api_key,
        )
        answer = data.get("answer") or ""
        lines = []
        if answer:
            lines.append(f"ANSWER: {answer}")
        for i, result in enumerate(data.get("results", []), start=1):
            lines.append(
                f"{i}. {result.get('title', '(untitled)')} - {result.get('url', '')}"
            )
            lines.append(f"   {_snippet(result.get('content', ''))}")
        return ToolResult(
            output="\n".join(lines) or "no results",
            details={"query": query, "raw": data},
        )


class ExaSearchTool:
    spec = ToolSpec(
        name="exa_search",
        description=(
            "Neural/semantic web search with page text excerpts (Exa). Best "
            "for finding specific pages, documentation and technical content."
        ),
        parameters={
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "The search query"},
                "num_results": {
                    "type": "integer",
                    "description": "Number of results (1-10, default 5)",
                    "minimum": 1,
                    "maximum": 10,
                },
            },
            "required": ["query"],
        },
    )

    def __init__(self, api_key: str, base_url: str = "https://api.exa.ai"):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult:
        validate_required(self.spec, args)
        query = args["query"]
        num = max(1, min(int(args.get("num_results", 5)), 10))
        data = _post_json(
            self.base_url + "/search",
            {
                "query": query,
                "numResults": num,
                "contents": {"text": {"maxCharacters": 1500}},
            },
            self.api_key,
        )
        lines = []
        for i, result in enumerate(data.get("results", []), start=1):
            lines.append(
                f"{i}. {result.get('title', '(untitled)')} - {result.get('url', '')}"
            )
            lines.append(
                f"   {_snippet(result.get('text', '') or result.get('summary', ''), 600)}"
            )
        return ToolResult(
            output="\n".join(lines) or "no results",
            details={"query": query, "raw": data},
        )
