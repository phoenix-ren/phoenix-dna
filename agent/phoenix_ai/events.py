# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Event kinds shared by the agent loop, daemon and D-Bus layer.

Events are plain dicts so they can be persisted as JSON, queued across
threads and serialized onto D-Bus without a translation layer::

    {"kind": "tool_call_started", "run_id": ..., "call_id": ..., "tool": ...}
"""

from __future__ import annotations

RUN_STARTED = "run_started"
TURN_STARTED = "turn_started"
TEXT_DELTA = "text_delta"
TOOL_CALL_STARTED = "tool_call_started"
TOOL_CALL_FINISHED = "tool_call_finished"
AUTH_REQUESTED = "auth_requested"
AUTH_FINISHED = "auth_finished"
RUN_FINISHED = "run_finished"
STATE_CHANGED = "state_changed"

#: Terminal statuses for a run.
RUN_STATUSES = ("completed", "cancelled", "error", "max_turns")


def make(kind: str, **data) -> dict:
    event = {"kind": kind}
    event.update(data)
    return event
