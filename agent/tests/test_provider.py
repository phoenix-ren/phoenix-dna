# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Provider tests: tool-call argument parsing over the SSE wire format.

The interesting case is malformed arguments: models occasionally emit an
invalid JSON escape (e.g. '\\d' in a grep pattern), and that must degrade
to a per-tool error, never abort the run.
"""

from __future__ import annotations

import json

from phoenix_dna.provider import OpenAICompatProvider


class _FakeStream:
    """Just enough of an HTTP response for _consume(): iterable lines."""

    def __init__(self, lines):
        self._lines = lines

    def __iter__(self):
        return iter(self._lines)

    def close(self):
        pass


def _sse(obj) -> bytes:
    return b"data: " + json.dumps(obj).encode() + b"\n"


def _tool_call_chunk(args_text: str) -> bytes:
    return _sse(
        {
            "choices": [
                {
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": "call_1",
                                "function": {"name": "bash", "arguments": args_text},
                            }
                        ]
                    }
                }
            ]
        }
    )


def _consume(items):
    provider = OpenAICompatProvider("http://localhost", "key", "model")
    stream = _FakeStream(items + [b"data: [DONE]\n"])
    return list(provider._consume(stream, None))


def _response(items):
    responses = [i for i in _consume(items) if i["type"] == "response"]
    assert len(responses) == 1
    return responses[0]["response"]


# -- the wire traffic --------------------------------------------------------


def test_received_bytes_are_counted():
    # Every byte of the response body passes the provider on its way in;
    # the count is what the frontend meters the rate from.
    provider = OpenAICompatProvider("http://localhost", "key", "model")
    line = _sse({"choices": [{"delta": {"content": "hello"}}]})
    list(provider._consume(_FakeStream([line, b"data: [DONE]\n"]), None))
    assert provider.wire.received == len(line) + len(b"data: [DONE]\n")


def test_deltas_carry_their_own_size():
    items = _consume(
        [_sse({"choices": [{"delta": {"reasoning_content": "thinking"}}]}),
         _sse({"choices": [{"delta": {"content": "answer"}}]})]
    )
    deltas = {i["type"]: i for i in items if i["type"] != "response"}
    assert deltas["thinking_delta"]["bytes_in"] == len(b"thinking")
    assert deltas["text_delta"]["bytes_in"] == len(b"answer")


def test_multi_byte_characters_count_as_bytes():
    # The size of a delta is its size on the wire: a character outside
    # ASCII takes more than one byte, and the meter would be wrong if it
    # counted characters.
    items = _consume([_sse({"choices": [{"delta": {"content": "héllo"}}]})])
    delta = next(i for i in items if i["type"] == "text_delta")
    assert delta["bytes_in"] == len("héllo".encode())
    assert delta["bytes_in"] != len("héllo")


def test_the_response_carries_the_call_totals(monkeypatch):
    # The request body goes out in one piece; the count is what the
    # frontend meters the send rate from.
    provider = OpenAICompatProvider("http://localhost", "key", "model")
    captured = {}

    def fake_urlopen(request, timeout=None):
        captured["body"] = request.data
        return _FakeStream([b"data: [DONE]\n"])

    monkeypatch.setattr("phoenix_dna.provider.urllib.request.urlopen", fake_urlopen)
    list(provider.stream([{"role": "user", "content": "hi"}], []))
    assert provider.wire.sent == len(captured["body"])
    assert provider.wire.sent > 0


def test_wellformed_arguments_parse():
    response = _response([_tool_call_chunk('{"command": "ls"}')])
    call = response.tool_calls[0]
    assert call.name == "bash"
    assert call.arguments == {"command": "ls"}
    assert call.arguments_error is None


def test_malformed_arguments_become_tool_error():
    # single backslash before 'd' is not a valid JSON escape
    response = _response([_tool_call_chunk('{"command": "grep \\d file"}')])
    call = response.tool_calls[0]
    assert call.arguments == {}
    assert call.arguments_error is not None
    assert "escape" in call.arguments_error.lower()


def test_non_object_arguments_become_tool_error():
    response = _response([_tool_call_chunk('["not", "an", "object"]')])
    call = response.tool_calls[0]
    assert call.arguments == {}
    assert "json object" in call.arguments_error.lower()


def test_arguments_streamed_in_pieces_are_reassembled():
    provider = OpenAICompatProvider("http://localhost", "key", "model")
    first = _tool_call_chunk('{"com')
    # the continuation chunk carries no id/name, like real streams
    second = _sse(
        {
            "choices": [
                {
                    "delta": {
                        "tool_calls": [
                            {"index": 0, "function": {"arguments": 'mand": "ls"}'}}
                        ]
                    }
                }
            ]
        }
    )
    stream = _FakeStream([first, second, b"data: [DONE]\n"])
    responses = [i for i in provider._consume(stream, None) if i["type"] == "response"]
    call = responses[0]["response"].tool_calls[0]
    assert call.arguments == {"command": "ls"}
    assert call.arguments_error is None


# --- thinking split out of the content ---------------------------------------


def _content_chunk(text: str) -> bytes:
    return _sse({"choices": [{"delta": {"content": text}}]})


def _reasoning_chunk(text: str) -> bytes:
    return _sse({"choices": [{"delta": {"reasoning_content": text}}]})


def _deltas(items):
    return [
        (i["type"], i.get("text", "")) for i in _consume(items) if i["type"] != "response"
    ]


def test_think_block_split_from_the_answer():
    response = _response([_content_chunk("<think>pondering</think>The answer.")])
    assert response.text == "The answer."
    assert response.thinking == "pondering"
    # the history keeps the content as the model sent it
    assert response.raw_text == "<think>pondering</think>The answer."


def test_think_tag_split_across_deltas():
    items = [
        _content_chunk("<th"),
        _content_chunk("ink>deep "),
        _content_chunk("thought</th"),
        _content_chunk("ink>\n\nAnswer"),
    ]
    response = _response(items)
    assert response.thinking == "deep thought"
    assert response.text == "Answer"
    # the reasoning streams as it arrives, the answer only once it is sure
    assert _deltas(items) == [
        ("thinking_delta", "deep "),
        ("thinking_delta", "thought"),
        ("text_delta", "Answer"),
    ]


def test_reasoning_content_field_is_thinking():
    response = _response([_reasoning_chunk("hmm "), _content_chunk("Hi there")])
    assert response.thinking == "hmm"
    assert response.text == "Hi there"
    assert response.raw_text == "Hi there"
    assert _deltas([_reasoning_chunk("hmm "), _content_chunk("Hi")]) == [
        ("thinking_delta", "hmm "),
        ("text_delta", "Hi"),
    ]


def test_plain_content_stays_the_answer():
    # a '<' that never becomes a tag must not hold the stream hostage
    items = [_content_chunk("Just the answer, < with a bracket."), _content_chunk(" More")]
    response = _response(items)
    assert response.text == "Just the answer, < with a bracket. More"
    assert response.thinking == ""
    assert response.raw_text == response.text


def test_two_think_blocks_around_one_answer():
    response = _response([_content_chunk("<think>a</think>mid<think>b</think>end")])
    assert response.text == "midend"
    assert response.thinking == "ab"


def test_unclosed_think_block_is_all_thinking():
    response = _response([_content_chunk("<think>never finished")])
    assert response.text == ""
    assert response.thinking == "never finished"
