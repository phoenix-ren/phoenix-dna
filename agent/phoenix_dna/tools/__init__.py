# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tool protocol and shared helpers."""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Protocol


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


@dataclass
class ToolResult:
    output: str  # what the LLM sees
    details: dict = field(default_factory=dict)  # extra data for the UI pane
    status: str = "success"


class ToolError(Exception):
    """Expected failure; the message is fed back to the LLM as tool output."""


class Tool(Protocol):
    spec: ToolSpec

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult: ...


def validate_required(spec: ToolSpec, args: dict) -> None:
    if not isinstance(args, dict):
        raise ToolError(f"arguments must be an object, got {type(args).__name__}")
    missing = [k for k in spec.parameters.get("required", []) if k not in args]
    if missing:
        raise ToolError(f"missing required argument(s): {', '.join(missing)}")


def trim_output(text: str, limit: int) -> str:
    """Shorten a tool's output to what the conversation may carry.

    The head and the tail are kept: the head says what was there, the
    tail says what came of it (the error, the last lines). The middle
    goes, with a marker that says how much - the tool can be asked
    again for the rest.

    The conversation is sent again with every turn, so what a tool
    brings back is paid for over and over: a tool that returns a
    megabyte would fill the model's window on its own.
    """
    if limit <= 0 or len(text) <= limit:
        return text
    head = int(limit * 0.6)
    tail = limit - head
    omitted = len(text) - head - tail
    return (
        text[:head]
        + f"\n... [{omitted} characters left out] ...\n"
        + text[-tail:]
    )
