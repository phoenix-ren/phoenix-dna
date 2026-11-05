# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
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
#: The model's reasoning, streamed apart from its answer (MiniMax and
#: friends wrap it in <think> tags inside the content; the provider
#: splits it out).
THINKING_DELTA = "thinking_delta"
TOOL_CALL_STARTED = "tool_call_started"
TOOL_CALL_FINISHED = "tool_call_finished"
#: The wire traffic of one turn of the conversation with the model:
#: the bytes of the request that went out and the bytes of the answer
#: that came in. The frontend meters the rate of the connection from
#: these (see the UI's WireMeter).
WIRE_BYTES = "wire_bytes"
#: A bash tool call's process began. ``pid`` is the command's session
#: leader (the daemon spawns commands with start_new_session=True), so
#: it doubles as the process-group id: the UI meters the whole command
#: by reading /proc for that group.
TOOL_PROCESS_STARTED = "tool_process_started"
#: The process of a bash tool call ended — normally, by timeout or by
#: cancellation. Emitted from a finally block, so it is reliable.
TOOL_PROCESS_FINISHED = "tool_process_finished"
#: The ask_user tool is showing the user a question; the run blocks until
#: the answer arrives (answer_question), the run is cancelled or it times out.
QUESTION_ASKED = "question_asked"
#: The bytes a transfer tool call has moved so far: the cumulative count
#: of the call's transfer, the total when the far end names one (0 when
#: it does not). The frontend meters the rate of the transfer from
#: these (see the UI's TransferMeter), the way WIRE_BYTES feeds the
#: meter of the conversation itself.
TRANSFER_PROGRESS = "transfer_progress"
#: A pending question ended: answered, timed out or cancelled.
QUESTION_ANSWERED = "question_answered"
AUTH_REQUESTED = "auth_requested"
AUTH_FINISHED = "auth_finished"
RUN_FINISHED = "run_finished"
#: The conversation was summarised: the older turns went into a summary
#: and the run goes on with that. The frontend says so in the run's
#: trail - the agent does not read the older turns any more.
COMPACTION = "compaction"
STATE_CHANGED = "state_changed"
#: A submitted transcript cleanup completed (successfully or not).
CLEANUP_FINISHED = "cleanup_finished"
#: A session's LLM-generated title was (re)generated.
SESSION_TITLE_CHANGED = "session_title_changed"
#: The daemon's speech playback switched between "speaking" and "idle"
#: (emitted by the Speaker; the D-Bus layer maps it onto the
#: SpeechStateChanged signal so the UI can reflect read-aloud state).
SPEECH_STATE_CHANGED = "speech_state_changed"

#: Terminal statuses for a run.
RUN_STATUSES = ("completed", "cancelled", "error", "max_turns")


def make(kind: str, **data) -> dict:
    event = {"kind": kind}
    event.update(data)
    return event
