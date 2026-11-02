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
