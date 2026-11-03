# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The ask_user tool: a human-in-the-loop tool call.

The same pattern OpenCode's ``question`` and Claude Code's
``AskUserQuestion`` tools use: the model calls the tool with a question
and 2-6 options, the tool *blocks the run* (exactly like a bash command
would) while the frontend renders the options, and the user's answer
returns as the tool result. The model never sees UI - only the validated
JSON of the call and the answer that came back.

``AskCoordinator`` is the daemon-side half: it owns the pending asks,
emits the question_asked/question_answered events and hands answers
(coming in over D-Bus, the REPL, or any other frontend) to the blocked
tool. One coordinator serves the whole daemon because runs are
sequential; each question gets its own ``ask_id``.
"""

from __future__ import annotations

import json
import logging
import threading
import time
import uuid
from dataclasses import dataclass

from .. import events
from . import ToolError, ToolResult, ToolSpec, validate_required

log = logging.getLogger("phoenix.ask")

#: How often a blocked ask wakes up to check cancel/deadline (seconds).
#: Same order as the bash tool's subprocess poll slice.
_POLL_SECONDS = 0.1

#: Guards against model-emitted wall-of-text options: labels must stay
#: UI-shaped (radio buttons, spoken aloud).
_MAX_LABEL_CHARS = 120
_MAX_QUESTION_CHARS = 2000


def _spec() -> ToolSpec:
    return ToolSpec(
        name="ask_user",
        description=(
            "Ask the user one question with multiple-choice options and "
            "wait for their answer; the tool result reports what they "
            "chose and anything they typed themselves. Use it whenever a "
            "decision belongs to the user (which of several targets, "
            "formats, scopes, schedules...); choose a sensible default "
            "yourself only when the choice genuinely has one. Give "
            "2 to 6 options with short labels "
            "(a few words) and a longer description when it helps; if "
            'you recommend one, put it first and add "(recommended)" to '
            'its label. Do not add an "other" or "type my own" option: '
            "the user can always write their own answer. A task with "
            "several such decisions is one short series of questions: "
            "pass goal (what you are collecting) plus step and "
            "est_steps on every question so the interface can show "
            "progress; revise est_steps as you learn."
        ),
        parameters={
            "type": "object",
            "properties": {
                "question": {
                    "type": "string",
                    "description": "The complete question, as shown to the user",
                },
                "options": {
                    "type": "array",
                    "description": "2 to 6 choices",
                    "items": {
                        "type": "object",
                        "properties": {
                            "label": {
                                "type": "string",
                                "description": "Concise choice, 1-5 words",
                            },
                            "description": {
                                "type": "string",
                                "description": "What choosing this means",
                            },
                        },
                        "required": ["label"],
                    },
                },
                "header": {
                    "type": "string",
                    "description": "Short label for the question (a few words)",
                },
                "multiple": {
                    "type": "boolean",
                    "description": "Allow several choices (default: one)",
                },
                "goal": {
                    "type": "string",
                    "description": "Wizard: what the series of questions collects",
                },
                "step": {
                    "type": "integer",
                    "description": "Wizard: which question this is, from 1",
                },
                "est_steps": {
                    "type": "integer",
                    "description": "Wizard: roughly how many questions in total",
                },
            },
            "required": ["question", "options"],
        },
    )


# ---------------------------------------------------------------------------
# coordinator: pending asks, answers, timeouts, cancellation
# ---------------------------------------------------------------------------


@dataclass
class _PendingAsk:
    question: dict
    answer: dict | None = None
    status: str = "pending"  # pending -> answered | timeout | cancelled
    run_id: str = ""


class AskCoordinator:
    """Tracks pending ask_user questions and delivers answers to them."""

    def __init__(self, emit):
        self._emit_fn = emit
        self._cond = threading.Condition()
        self._pending: dict[str, _PendingAsk] = {}

    # -- tool side (worker thread) ------------------------------------------

    def ask(
        self,
        run_id: str,
        question: dict,
        *,
        cancel: threading.Event | None = None,
        timeout: float | None = None,
    ) -> tuple[dict, str]:
        """Block until the user answers, the run is cancelled or time is up.

        Returns ``(answer, status)`` with status ``answered``, ``timeout``
        or ``cancelled``; the answer dict is normalized
        (``{"selected": [labels], "text": str}``) and empty unless answered.
        """
        ask_id = "ask_" + uuid.uuid4().hex[:12]
        pending = _PendingAsk(question=question, run_id=run_id)
        with self._cond:
            self._pending[ask_id] = pending

        self._emit(
            events.make(
                events.QUESTION_ASKED,
                run_id=run_id,
                ask_id=ask_id,
                question=question,
            )
        )

        deadline = (
            None if not timeout or timeout <= 0 else time.monotonic() + timeout
        )
        status = "timeout"
        with self._cond:
            while True:
                if pending.status == "answered":
                    status = "answered"
                    break
                if cancel is not None and cancel.is_set():
                    status = "cancelled"
                    break
                if deadline is not None and time.monotonic() >= deadline:
                    status = "timeout"
                    break
                self._cond.wait(timeout=_POLL_SECONDS)
            answer = (
                pending.answer
                if status == "answered"
                else {"selected": [], "text": ""}
            )
            self._pending.pop(ask_id, None)

        self._emit(
            events.make(
                events.QUESTION_ANSWERED,
                run_id=run_id,
                ask_id=ask_id,
                ask_status=status,
                answer=answer,
            )
        )
        return answer, status

    # -- frontend side (D-Bus main loop, REPL, tests) ------------------------

    def answer(self, ask_id: str, answer_json: str) -> bool:
        """Answer a pending question; False when the answer cannot be used.

        False covers an unknown ``ask_id`` (already answered, timed out,
        cancelled - or never asked) and payloads that do not resolve
        against the question (bad indices, unknown labels, several picks
        on a single-choice question, empty answers). The question keeps
        waiting either way.
        """
        try:
            payload = _parse_payload(answer_json)
        except ValueError as exc:
            log.warning("answer for %s is not usable: %s", ask_id, exc)
            return False

        with self._cond:
            pending = self._pending.get(ask_id)
            if pending is None or pending.status != "pending":
                return False
            try:
                resolved = _resolve(pending.question, payload)
            except ValueError as exc:
                log.warning("answer for %s does not resolve: %s", ask_id, exc)
                return False
            pending.answer, pending.status = resolved, "answered"
            self._cond.notify_all()
        return True

    # -- introspection --------------------------------------------------------

    def pending_ask_ids(self) -> list[str]:
        with self._cond:
            return list(self._pending)

    def _emit(self, event: dict) -> None:
        try:
            self._emit_fn(event)
        except Exception:  # pragma: no cover - an emitter must not wedge a run
            log.exception("question event emitter failed")


def _parse_payload(answer_json: str) -> dict:
    try:
        payload = json.loads(answer_json)
    except ValueError as exc:
        raise ValueError(f"not valid JSON ({exc})") from exc
    # A bare string is accepted as the user's own words ("gdbus" one-liners).
    if isinstance(payload, str):
        return {"text": payload}
    if not isinstance(payload, dict):
        raise ValueError("the answer must be an object or a string")
    return payload


def _resolve(question: dict, payload: dict) -> dict:
    """Normalize a payload against its question; ValueError when invalid."""
    options = question.get("options", [])
    raw = payload.get("selected", [])
    if isinstance(raw, (int, str)):
        raw = [raw]

    labels: list[str] = []
    for item in raw:
        if isinstance(item, bool) or not isinstance(item, (int, str)):
            raise ValueError("selected entries must be indices or labels")
        if isinstance(item, int):
            if not 0 <= item < len(options):
                raise ValueError(f"option index {item} is out of range")
            labels.append(options[item]["label"])
            continue
        label = _match_label(options, item)
        if label is None:
            raise ValueError(f"no option labeled {item!r}")
        labels.append(label)

    if len(labels) > 1 and not question.get("multiple"):
        raise ValueError("several choices given for a single-choice question")

    text = payload.get("text", "")
    if not isinstance(text, str):
        raise ValueError("text must be a string")
    text = text.strip()

    if not labels and not text:
        raise ValueError("the answer is empty")
    return {"selected": labels, "text": text}


def _match_label(options: list[dict], text: str) -> str | None:
    for option in options:
        if option["label"] == text:
            return option["label"]
    folded = text.strip().casefold()
    for option in options:
        if option["label"].strip().casefold() == folded:
            return option["label"]
    return None


# ---------------------------------------------------------------------------
# the tool
# ---------------------------------------------------------------------------


class AskUserTool:
    """Blocks the run until the user answers (or cancel/timeout)."""

    spec = _spec()

    def __init__(
        self,
        coordinator: AskCoordinator,
        *,
        run_id: str = "",
        timeout: float | None = None,
    ):
        self._coordinator = coordinator
        self._run_id = run_id
        self._timeout = timeout

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult:
        validate_required(self.spec, args)
        question = _normalize_question(args)

        answer, status = self._coordinator.ask(
            self._run_id, question, cancel=cancel, timeout=self._timeout
        )
        if status == "cancelled":
            raise ToolError("question cancelled before it was answered")

        details = {"question": question, "answer": None, "ask_status": status}
        if status == "timeout":
            seconds = self._timeout if self._timeout is not None else 0
            return ToolResult(
                output=(
                    f"The user did not answer within {seconds:.0f} seconds - "
                    "they may be away. Proceed with your best judgment and "
                    "say which assumption you made in your final answer."
                ),
                details=details,
            )

        details["answer"] = answer
        return ToolResult(output=_compose_output(answer), details=details)


def _normalize_question(args: dict) -> dict:
    """Validate the model's arguments and build the UI-facing payload."""

    def bad(message: str) -> ToolError:
        return ToolError(f"the tool call was not executed: {message}")

    question_text = args.get("question")
    if not isinstance(question_text, str) or not question_text.strip():
        raise bad("question must be a non-empty string")
    question_text = question_text.strip()
    if len(question_text) > _MAX_QUESTION_CHARS:
        question_text = question_text[:_MAX_QUESTION_CHARS]

    raw_options = args.get("options")
    if not isinstance(raw_options, list) or not 2 <= len(raw_options) <= 6:
        raise bad("options must be a list of 2 to 6 choices")

    options = []
    for position, option in enumerate(raw_options, start=1):
        if not isinstance(option, dict):
            raise bad(f"option {position} must be an object with a label")
        label = option.get("label")
        if not isinstance(label, str) or not label.strip():
            raise bad(f"option {position} needs a non-empty label")
        label = label.strip()[:_MAX_LABEL_CHARS]
        description = option.get("description") or ""
        if not isinstance(description, str):
            raise bad(f"option {position} description must be a string")
        entry = {"label": label}
        if description.strip():
            entry["description"] = description.strip()
        options.append(entry)

    question = {"question": question_text, "options": options}

    header = args.get("header")
    if header is not None:
        if not isinstance(header, str):
            raise bad("header must be a string")
        if header.strip():
            question["header"] = header.strip()[:_MAX_LABEL_CHARS]

    multiple = args.get("multiple", False)
    if not isinstance(multiple, bool):
        raise bad("multiple must be true or false")
    if multiple:
        question["multiple"] = True

    goal = args.get("goal")
    if goal is not None:
        if not isinstance(goal, str) or not goal.strip():
            raise bad("goal must be a non-empty string when given")
        question["goal"] = goal.strip()[:_MAX_QUESTION_CHARS]

    for name in ("step", "est_steps"):
        value = args.get(name)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            raise bad(f"{name} must be a whole number of 1 or more")
        question[name] = value

    return question


def _compose_output(answer: dict) -> str:
    """What the model reads as the tool result."""
    selected = answer.get("selected", [])
    text = answer.get("text", "")
    parts = []
    if selected:
        labels = ", ".join(f'"{label}"' for label in selected)
        one = len(selected) == 1
        parts.append(("chose " if one else "chose several: ") + labels)
    if text:
        parts.append(f'said "{text}"')
    return "The user " + " and ".join(parts) + "."
