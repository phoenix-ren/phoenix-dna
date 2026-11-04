# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""LLM provider abstraction.

``OpenAICompatProvider`` speaks the OpenAI chat-completions wire format with
SSE streaming, which is what MiniMax (api.minimax.io/v1) and most other
providers implement. ``FakeProvider`` is a scripted provider used by the test
suite and by ``--provider fake`` to smoke-test the daemon without network.

The streaming contract: ``stream()`` yields progress dicts

    {"type": "thinking_delta", "text": "..."}
    {"type": "text_delta", "text": "..."}
    ...
    {"type": "response", "response": LLMResponse(...)}

exactly one ``response`` per call, as the final item. Reasoning models wrap
their thinking around the answer — in ``<think>…</think>`` blocks inside the
content, or in a separate ``reasoning_content`` field — and the provider
splits it out as ``thinking_delta`` items, so frontends can show the answer
as the result of the run and the thinking as its trail.

Wire traffic is counted on the way, so a frontend can meter the data rate
of the connection: the request body goes out in one piece, the answer
comes back in pieces. The counting is deliberately approximate — it does
not include the fixed-size HTTP headers, nor the TLS framing beneath them
— but it is exact about the payload bytes that carry the conversation.
The counts ride along with the progress dicts as ``bytes_in`` /
``bytes_out`` (a frontend that does not care about them ignores them).
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


@dataclass
class WireCounter:
    """Counts the bytes that cross the wire for one API call.

    The request goes out in one piece (the JSON body of the request), the
    answer comes back in many (the SSE lines of the response). Both are
    counted here so a frontend can meter the rate of the connection.

    The counts are the payload bytes only: the fixed-size HTTP headers
    and the TLS framing beneath them are not included, which makes the
    numbers an approximation of the wire's true cost — good enough to
    see whether a connection is busy, which is what they are for.
    """

    #: The bytes of the request body sent to the server.
    sent: int = 0
    #: The bytes of the response body received from the server.
    received: int = 0

    def add_sent(self, count: int) -> None:
        self.sent += count

    def add_received(self, count: int) -> None:
        self.received += count


@dataclass(frozen=True)
class LLMResponse:
    #: The model's answer: its content with the thinking taken out (the
    #: ``<think>…</think>`` blocks and the ``reasoning_content`` channel).
    #: This is the text a frontend shows as the result of the run.
    text: str = ""
    #: The thinking that came wrapped around the answer, kept apart so it
    #: can be shown as the trail of the run instead of its result.
    thinking: str = ""
    #: The content as the model sent it, thinking included: what the
    #: conversation history carries, where the model reads its own past
    #: reasoning.
    raw_text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    usage: Usage | None = None
    finish_reason: str | None = None
    #: The bytes this call put on the wire: the request body that went
    #: out, the response body that came in. The frontend meters the rate
    #: of the connection from them.
    bytes_sent: int = 0
    bytes_received: int = 0


class LLMProvider(Protocol):
    def stream(
        self,
        messages: list[dict],
        tools: list[ToolSpec],
        should_cancel: Callable[[], bool] | None = None,
    ) -> Iterator[dict]: ...


class _ThinkSplitter:
    """Splits ``<think>…</think>`` blocks out of streamed content.

    Reasoning models (MiniMax-M2 and friends) wrap their thinking in these
    tags inside the ordinary ``content`` field, and a tag can arrive split
    across any number of deltas. Text is therefore held at a boundary only
    while it could still turn into a tag — a few characters — and streams
    through immediately otherwise.

    Repeated blocks are handled: ``<think>a</think>mid<think>b</think>end``
    is thinking ``ab`` around the answer ``midend``.
    """

    OPEN = "<think>"
    CLOSE = "</think>"

    def __init__(self) -> None:
        # Received but not yet classified: it may hold the start of a tag.
        self._pending = ""
        # Whether the stream is currently inside a think block.
        self._in_think = False
        # Whether the next answer text should lose its leading whitespace
        # (it does right after a think block closes).
        self._trim_answer = False

    def feed(self, delta: str) -> tuple[str, str]:
        """Classifies one delta; returns (thinking, answer)."""
        self._pending += delta
        thinking: list[str] = []
        answer: list[str] = []
        while self._pending:
            if not self._in_think:
                stripped = self._pending.lstrip()
                if not stripped:
                    break  # whitespace may still precede a tag: hold it
                state = self._tag_state(stripped, self.OPEN)
                if state == "partial":
                    break  # could still become "<think>": hold it
                if state == "complete":
                    # The whitespace before the tag was never answer text.
                    self._pending = stripped[len(self.OPEN) :]
                    self._in_think = True
                    continue
                cut = self._open_before_end()
                text = self._pending[:cut]
                self._pending = self._pending[cut:]
                if self._trim_answer:
                    trimmed = text.lstrip()
                    if trimmed:
                        self._trim_answer = False
                    text = trimmed
                if text:
                    answer.append(text)
                if not cut:
                    break  # nothing could be classified yet
                continue
            close = self._pending.find(self.CLOSE)
            if close >= 0:
                thinking.append(self._pending[:close])
                self._pending = self._pending[close + len(self.CLOSE) :]
                self._in_think = False
                self._trim_answer = True
                continue
            # Hold back only a tail that might be the start of the tag.
            keep = self._held_back(self._pending, self.CLOSE)
            if keep < len(self._pending):
                thinking.append(self._pending[: len(self._pending) - keep])
                self._pending = self._pending[len(self._pending) - keep :]
            break
        return "".join(thinking), "".join(answer)

    def finish(self) -> tuple[str, str]:
        """Classifies what is left at the end of the stream."""
        thinking, answer = self.feed("")
        if self._in_think:
            # A think block that never closed: all of it thinking.
            thinking += self._pending
        else:
            text = self._pending
            if self._trim_answer:
                text = text.lstrip()
            answer += text
        self._pending = ""
        return thinking, answer

    def _open_before_end(self) -> int:
        """Where the answer text of ``_pending`` ends.

        The next think block may start inside this delta, and a partial
        tag at the end is held back until it is decided.
        """
        open_at = self._pending.find(self.OPEN)
        if open_at > 0:
            return open_at
        return len(self._pending) - self._held_back(self._pending, self.OPEN)

    @classmethod
    def _tag_state(cls, text: str, tag: str) -> str:
        """``complete``, ``partial`` or ``none``: whether text begins tag."""
        n = min(len(text), len(tag))
        if text[:n] != tag[:n]:
            return "none"
        return "complete" if len(text) >= len(tag) else "partial"

    @classmethod
    def _held_back(cls, text: str, tag: str) -> int:
        """How many trailing characters of text might start tag."""
        for n in range(min(len(text), len(tag) - 1), 0, -1):
            if tag.startswith(text[-n:]):
                return n
        return 0


class OpenAICompatProvider:
    """Streaming chat-completions client for OpenAI-compatible endpoints."""

    def __init__(self, base_url: str, api_key: str, model: str, timeout: float = 120.0):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.model = model
        self.timeout = timeout
        # The bytes of the calls this provider made: what went out, what
        # came in. The frontend meters the rate of the connection from
        # these.
        self.wire = WireCounter()

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
        # The body is the request's whole payload: the conversation, the
        # model, the tools. Its size is what goes over the wire (apart
        # from the headers urllib adds), and it is counted here, where it
        # is materialised. A request that is retried counts twice: both
        # bodies really were sent.
        body = json.dumps(payload).encode()
        self.wire.add_sent(len(body))
        request = urllib.request.Request(
            self.base_url + "/chat/completions",
            data=body,
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
        thinking_parts: list[str] = []
        raw_parts: list[str] = []
        tool_calls: dict[int, dict] = {}
        usage = None
        finish_reason = None
        splitter = _ThinkSplitter()

        try:
            for raw_line in response:
                if should_cancel is not None and should_cancel():
                    response.close()
                    raise CancelledError("cancelled by user")
                # Every byte of the response body passes through here,
                # one line at a time: the SSE events, their framing and
                # their payloads. Counted as they arrive, so a frontend
                # can watch the rate the answer comes in at.
                self.wire.add_received(len(raw_line))
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
                # Some models keep the thinking in its own field.
                reasoning = delta.get("reasoning_content")
                if reasoning:
                    thinking_parts.append(reasoning)
                    yield {
                        "type": "thinking_delta",
                        "text": reasoning,
                        "bytes_in": len(reasoning.encode()),
                    }
                content = delta.get("content")
                if content:
                    raw_parts.append(content)
                    # Others wrap it in <think> tags inside the content.
                    thinking, answer = splitter.feed(content)
                    if thinking:
                        thinking_parts.append(thinking)
                        yield {
                            "type": "thinking_delta",
                            "text": thinking,
                            "bytes_in": len(thinking.encode()),
                        }
                    if answer:
                        text_parts.append(answer)
                        yield {
                            "type": "text_delta",
                            "text": answer,
                            "bytes_in": len(answer.encode()),
                        }
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

        # Whatever the splitter was still holding (a trailing partial tag,
        # an unclosed think block) belongs to the stream just as much.
        tail_thinking, tail_answer = splitter.finish()
        if tail_thinking:
            thinking_parts.append(tail_thinking)
            yield {
                "type": "thinking_delta",
                "text": tail_thinking,
                "bytes_in": len(tail_thinking.encode()),
            }
        if tail_answer:
            text_parts.append(tail_answer)
            yield {
                "type": "text_delta",
                "text": tail_answer,
                "bytes_in": len(tail_answer.encode()),
            }

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
                thinking="".join(thinking_parts).strip(),
                raw_text="".join(raw_parts),
                tool_calls=calls,
                usage=usage,
                finish_reason=finish_reason,
                bytes_sent=self.wire.sent,
                bytes_received=self.wire.received,
            ),
        }


class FakeProvider:
    """Scripted provider for tests and daemon smoke runs.

    Script items (a plain string is shorthand for final text):

    * ``"final answer"`` / ``{"text": "..."}``
    * ``{"thinking": "...", "text": "..."}``   reasoning around the answer
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
        thinking = item.get("thinking", "")
        if thinking:
            half = max(1, len(thinking) // 2)
            yield {
                "type": "thinking_delta",
                "text": thinking[:half],
                "bytes_in": len(thinking[:half].encode()),
            }
            yield {
                "type": "thinking_delta",
                "text": thinking[half:],
                "bytes_in": len(thinking[half:].encode()),
            }
        if text:
            half = max(1, len(text) // 2)
            yield {
                "type": "text_delta",
                "text": text[:half],
                "bytes_in": len(text[:half].encode()),
            }
            yield {
                "type": "text_delta",
                "text": text[half:],
                "bytes_in": len(text[half:].encode()),
            }

        calls = []
        if item.get("tool"):
            calls.append(self._call(item["tool"], item.get("args", {})))
        for sub in item.get("tools", []):
            calls.append(self._call(sub.get("tool", ""), sub.get("args", {})))

        yield {
            "type": "response",
            "response": LLMResponse(
                text=text,
                thinking=thinking,
                raw_text=text,
                tool_calls=calls,
                usage=Usage(input_tokens=11, output_tokens=7),
                finish_reason="tool_calls" if calls else "stop",
                # The fake carries no wire, but the counts are part of
                # the contract now: a scripted run still has a turn, and
                # the meter must have something to show.
                bytes_sent=0,
                bytes_received=len((text + thinking).encode()),
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
            f"no API key configured for the '{config.llm_provider}' provider: "
            f"set its API key (see agent/README.md) or create "
            f"{config.config_dir / 'keys.json'}"
        )
    if not config.llm_base_url or not config.model:
        raise RuntimeError(
            "the LLM endpoint and model are not configured: name them in "
            f"{config.config_dir / 'agent.toml'} as llm_base_url and model, "
            "or pick a known provider as llm_provider"
        )
    return OpenAICompatProvider(
        base_url=config.llm_base_url,
        api_key=config.llm_api_key,
        model=config.model,
        timeout=config.request_timeout,
    )
