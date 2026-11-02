# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""LLM provider abstraction.

``OpenAICompatProvider`` speaks the OpenAI chat-completions wire format with
SSE streaming, which is what MiniMax (api.minimax.io/v1) and most other
providers implement. ``FakeProvider`` is a scripted provider used by the test
suite and by ``--provider fake`` to smoke-test the daemon without network.

The streaming contract: ``stream()`` yields progress dicts

    {"type": "text_delta", "text": "..."}
    ...
    {"type": "response", "response": LLMResponse(...)}

exactly one ``response`` per call, as the final item.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable, Iterator, Protocol


class CancelledError(Exception):
    """Raised when a provider call is aborted mid-stream."""


class LLMError(Exception):
    """Any provider failure (transport, HTTP, or malformed payload)."""


@dataclass(frozen=True)
class ToolSpec:
    name: str
    description: str
    parameters: dict  # JSON schema

    def to_openai(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "parameters": self.parameters,
        }


@dataclass(frozen=True)
class ToolCall:
    id: str
    name: str
    arguments: dict
    #: Set when the model's arguments could not be parsed as JSON; the call
    #: is then not executed and the error is fed back to the model, which
    #: retries with valid JSON (a malformed escape must not abort the run).
    arguments_error: str | None = None

    def to_openai(self) -> dict:
        return {
            "id": self.id,
            "type": "function",
            "function": {
                "name": self.name,
                "arguments": json.dumps(self.arguments),
            },
        }


@dataclass(frozen=True)
class Usage:
    input_tokens: int = 0
    output_tokens: int = 0


@dataclass(frozen=True)
class LLMResponse:
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    usage: Usage | None = None
    finish_reason: str | None = None


class LLMProvider(Protocol):
    def stream(
        self,
        messages: list[dict],
        tools: list[ToolSpec],
        should_cancel: Callable[[], bool] | None = None,
    ) -> Iterator[dict]: ...


class OpenAICompatProvider:
    """Streaming chat-completions client for OpenAI-compatible endpoints."""

    def __init__(self, base_url: str, api_key: str, model: str, timeout: float = 120.0):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.model = model
        self.timeout = timeout

    # -- public API ---------------------------------------------------------

    def stream(self, messages, tools, should_cancel=None) -> Iterator[dict]:
        payload = {
            "model": self.model,
            "messages": messages,
            "stream": True,
            "stream_options": {"include_usage": True},
        }
        if tools:
            payload["tools"] = [
                {"type": "function", "function": t.to_openai()} for t in tools
            ]
        try:
            response = self._open(payload)
        except urllib.error.HTTPError as exc:
            if exc.code == 400:
                body = self._error_body(exc)
                if b"stream_options" in body:
                    # Some servers reject stream_options; retry without it.
                    payload.pop("stream_options", None)
                    try:
                        response = self._open(payload)
                    except urllib.error.HTTPError as exc2:
                        raise LLMError(
                            f"HTTP {exc2.code}: {self._error_body(exc2)[:300].decode(errors='replace')}"
                        ) from exc2
                    except (urllib.error.URLError, OSError) as exc2:
                        raise LLMError(str(exc2)) from exc2
                else:
                    raise LLMError(
                        f"HTTP 400: {body[:300].decode(errors='replace')}"
                    ) from exc
            else:
                raise LLMError(
                    f"HTTP {exc.code}: {self._error_body(exc)[:300].decode(errors='replace')}"
                ) from exc
        except urllib.error.URLError as exc:
            raise LLMError(f"cannot reach {self.base_url}: {exc.reason}") from exc

        yield from self._consume(response, should_cancel)

    # -- internals ----------------------------------------------------------

    def _open(self, payload: dict):
        request = urllib.request.Request(
            self.base_url + "/chat/completions",
            data=json.dumps(payload).encode(),
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
                "Accept": "text/event-stream",
            },
            method="POST",
        )
        return urllib.request.urlopen(request, timeout=self.timeout)

    @staticmethod
    def _error_body(exc: urllib.error.HTTPError) -> bytes:
        try:
            return exc.read() or b""
        except Exception:  # pragma: no cover - defensive
            return b""

    def _consume(self, response, should_cancel) -> Iterator[dict]:
        text_parts: list[str] = []
        tool_calls: dict[int, dict] = {}
        usage = None
        finish_reason = None

        try:
            for raw_line in response:
                if should_cancel is not None and should_cancel():
                    response.close()
                    raise CancelledError("cancelled by user")
                line = raw_line.strip()
                if not line.startswith(b"data:"):
                    continue
                data_str = line[5:].strip()
                if not data_str or data_str == b"[DONE]":
                    if data_str == b"[DONE]":
                        break
                    continue
                try:
                    chunk = json.loads(data_str)
                except ValueError:
                    continue
                if isinstance(chunk.get("usage"), dict) and chunk["usage"]:
                    usage = Usage(
                        input_tokens=int(chunk["usage"].get("prompt_tokens", 0) or 0),
                        output_tokens=int(
                            chunk["usage"].get("completion_tokens", 0) or 0
                        ),
                    )
                choices = chunk.get("choices") or []
                if not choices:
                    continue
                choice = choices[0]
                if choice.get("finish_reason"):
                    finish_reason = choice["finish_reason"]
                delta = choice.get("delta") or {}
                content = delta.get("content")
                if content:
                    text_parts.append(content)
                    yield {"type": "text_delta", "text": content}
                for tc in delta.get("tool_calls") or []:
                    index = tc.get("index", 0)
                    slot = tool_calls.setdefault(
                        index, {"id": None, "name": None, "arguments": ""}
                    )
                    if tc.get("id"):
                        slot["id"] = tc["id"]
                    function = tc.get("function") or {}
                    if function.get("name"):
                        slot["name"] = function["name"]
                    if function.get("arguments"):
                        slot["arguments"] += function["arguments"]
        except urllib.error.URLError as exc:  # pragma: no cover - network flake
            raise LLMError(str(exc)) from exc
        finally:
            try:
                response.close()
            except Exception:
                pass

        calls: list[ToolCall] = []
        for index in sorted(tool_calls):
            slot = tool_calls[index]
            call_id = slot["id"] or f"call_{index}"
            name = slot["name"] or ""
            arguments_error = None
            try:
                arguments = json.loads(slot["arguments"] or "{}")
                if not isinstance(arguments, dict):
                    raise ValueError("arguments must be a JSON object")
            except ValueError as exc:
                # A model-emitted typo (e.g. '\d' in a pattern) must not kill
                # the run: keep the call with empty arguments and let the
                # loop feed the parse error back as a tool error.
                arguments, arguments_error = {}, str(exc)
            calls.append(
                ToolCall(
                    id=call_id,
                    name=name,
                    arguments=arguments,
                    arguments_error=arguments_error,
                )
            )

        yield {
            "type": "response",
            "response": LLMResponse(
                text="".join(text_parts),
                tool_calls=calls,
                usage=usage,
                finish_reason=finish_reason,
            ),
        }


class FakeProvider:
    """Scripted provider for tests and daemon smoke runs.

    Script items (a plain string is shorthand for final text):

    * ``"final answer"`` / ``{"text": "..."}``
    * ``{"tool": "bash", "args": {...}}``           one tool call
    * ``{"tools": [{"tool": ..., "args": {...}}, ...]}`` several calls per turn
    * ``{"text": "...", "tool": ..., "args": {...}}``  text + one tool call
    * ``{"sleep": 1.5}``                            delay, then plain reply
    * ``{"raise": "cancelled"}``                    raises CancelledError
    * ``{"raise": "llm", "message": "boom"}``       raises LLMError
    * ``callable(messages, tools) -> item``         dynamic scripts (loops)

    Every ``stream()`` call replays the script from the start, so a shared
    instance serves any number of runs. ``last_messages`` exposes what the
    loop most recently sent (handy for session-continuity assertions).
    """

    def __init__(self, script):
        self.script = list(script)
        self.last_messages: list[dict] | None = None
        self.last_tools: list[ToolSpec] | None = None
        self._cursor = 0

    def stream(self, messages, tools, should_cancel=None) -> Iterator[dict]:
        self.last_messages = messages
        self.last_tools = tools
        if should_cancel is not None and should_cancel():
            raise CancelledError("cancelled by user")

        item = (
            self.script[min(self._cursor, len(self.script) - 1)] if self.script else ""
        )
        self._cursor += 1
        if callable(item):
            item = item(messages, tools)
        if isinstance(item, str):
            item = {"text": item}

        if item.get("sleep"):
            time.sleep(float(item["sleep"]))
            if should_cancel is not None and should_cancel():
                raise CancelledError("cancelled by user")

        raised = item.get("raise")
        if raised == "cancelled":
            raise CancelledError("cancelled by user")
        if raised == "llm":
            raise LLMError(item.get("message", "scripted LLM failure"))

        text = item.get("text", "")
        if text:
            half = max(1, len(text) // 2)
            yield {"type": "text_delta", "text": text[:half]}
            yield {"type": "text_delta", "text": text[half:]}

        calls = []
        if item.get("tool"):
            calls.append(self._call(item["tool"], item.get("args", {})))
        for sub in item.get("tools", []):
            calls.append(self._call(sub.get("tool", ""), sub.get("args", {})))

        yield {
            "type": "response",
            "response": LLMResponse(
                text=text,
                tool_calls=calls,
                usage=Usage(input_tokens=11, output_tokens=7),
                finish_reason="tool_calls" if calls else "stop",
            ),
        }

    @staticmethod
    def _call(name: str, args: dict) -> ToolCall:
        FakeProvider._counter = getattr(FakeProvider, "_counter", 0) + 1
        return ToolCall(
            id=f"fakecall_{FakeProvider._counter}", name=name, arguments=dict(args)
        )


def build_provider(config):
    """Factory used by the daemon and the CLI."""
    if config.provider == "fake":
        return FakeProvider(["This is a canned response from the fake provider."])
    if not config.llm_api_key:
        raise RuntimeError(
            "no MiniMax API key configured: set MINIMAX_API_KEY or create "
            f"{config.config_dir / 'keys.json'} (see agent/README.md)"
        )
    return OpenAICompatProvider(
        base_url=config.llm_base_url,
        api_key=config.llm_api_key,
        model=config.model,
        timeout=config.request_timeout,
    )
