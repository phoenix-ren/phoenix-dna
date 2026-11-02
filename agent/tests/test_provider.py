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
