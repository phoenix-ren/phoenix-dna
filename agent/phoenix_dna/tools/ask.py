# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The ask_user tool: a human-in-the-loop tool call.

The same pattern OpenCode's ``question`` and Claude Code's
``AskUserQuestion`` tools use: the model calls the tool, the tool
*blocks the run* (exactly like a bash command would) while the frontend
renders the questions, and the user's answers return as the tool result.
The model never sees UI - only the validated JSON of the call and the
answers that came back.

**Batch shape.** One call carries every question the model already knows
it needs (``questions``), and every answer returns together: the
interface walks the user through them with a progress bar and a back
button - all client-side, no round-trip per question - and submits once.
A call with a single question is a form of one (the inline card);
follow-up questions, asked only when earlier answers open decisions the
model could not have written in advance, are just another (small) call.
The wizard metadata of the sequential era (step/est_steps) is gone: the
frontend knows the count exactly.

``AskCoordinator`` is the daemon-side half: it owns the pending ask,
emits the question_asked/question_answered events and hands answers
(coming in over D-Bus, the REPL, or any other frontend) to the blocked
tool. One coordinator serves the whole daemon because runs are
sequential; each form gets its own ``ask_id``.
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

#: Guards against model-emitted wall-of-text questions: labels and
#: questions must stay UI-shaped (radio buttons, spoken aloud).
_MAX_LABEL_CHARS = 120
_MAX_QUESTION_CHARS = 2000

#: How many questions one form may carry, and options per question.
_MAX_QUESTIONS = 6
_MIN_OPTIONS = 2
_MAX_OPTIONS = 6


#: The tool description is the model's user manual - it carries when to
#: ask, the batch discipline, and the option conventions.
_DESCRIPTION = (
    "Ask the user questions with multiple-choice options and wait for "
    "their answers; the tool result reports what they chose and "
    "anything they typed themselves. Use it whenever a decision "
    "belongs to the user (which of several targets, formats, scopes, "
    "schedules...); choose a sensible default yourself only when the "
    "choice genuinely has one. Put EVERY question you already know you "
    "need into ONE call - the interface walks the user through them "
    "with a progress bar and a back button, and all answers return "
    "together. Ask follow-ups (batched again when you can) only when "
    "an earlier answer opens a decision you could not have written in "
    "advance. Give each question 2 to 6 options with short labels (a "
    "few words) and a longer description when it helps; if you "
    'recommend one, put it first and add "(recommended)" to its '
    'label. Do not add an "other" or "type my own" option: the user '
    "can always write their own answer. A question the user leaves "
    "blank comes back as (no answer) - do not ask it again, decide "
    "yourself and say so."
)

#: The JSON schema: one call, one form.
_PARAMETERS = {
    "type": "object",
    "properties": {
        "questions": {
            "type": "array",
            "minItems": 1,
            "maxItems": 6,
            "description": "1 to 6 questions, all asked in this one call",
            "items": {
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
                },
                "required": ["question", "options"],
            },
        },
        "goal": {
            "type": "string",
            "description": "What the questions collect, shown as the form's title",
        },
    },
    "required": ["questions"],
}


# ---------------------------------------------------------------------------
# coordinator: the pending form, answers, timeouts, cancellation
# ---------------------------------------------------------------------------


@dataclass
class _PendingAsk:
    form: dict
    answer: dict | None = None
    status: str = "pending"  # pending -> answered | timeout | cancelled
    run_id: str = ""


class AskCoordinator:
    """Tracks the pending ask_user form and delivers its answers."""

    def __init__(self, emit):
        self._emit_fn = emit
        self._cond = threading.Condition()
        self._pending: dict[str, _PendingAsk] = {}

    # -- tool side (worker thread) ------------------------------------------

    def ask(
        self,
        run_id: str,
        form: dict,
        *,
        cancel: threading.Event | None = None,
        timeout: float | None = None,
    ) -> tuple[dict, str]:
        """Block until the user answers, the run is cancelled or time is up.

        Returns ``(answer, status)`` with status ``answered``, ``timeout``
        or ``cancelled``; the answer dict is normalized
        (``{"answers": [{selected, text}, ...]}``) and empty unless
        answered.
        """
        ask_id = "ask_" + uuid.uuid4().hex[:12]
        pending = _PendingAsk(form=form, run_id=run_id)
        with self._cond:
            self._pending[ask_id] = pending

        self._emit(
            events.make(
                events.QUESTION_ASKED,
                run_id=run_id,
                ask_id=ask_id,
                # The payload is the form: {questions, goal, harness?}.
                # The key keeps its historical name - the wire is stable.
                question=form,
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
                else {"answers": []}
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
        """Answer a pending form; False when the answer cannot be used.

        False covers an unknown ``ask_id`` (already answered, timed out,
        cancelled - or never asked) and payloads that do not resolve
        against the form (wrong answer count, bad indices, unknown
        labels, several picks on a single-choice question). The form
        keeps waiting either way.
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
                resolved = _resolve(pending.form, payload)
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
    # A bare string is accepted as the user's own words on the first
    # question ("gdbus" one-liners, a dictated one-word reply).
    if isinstance(payload, str):
        return {"answers": [{"text": payload}]}
    if not isinstance(payload, dict):
        raise ValueError("the answer must be an object or a string")
    return payload


def _resolve(form: dict, payload: dict) -> dict:
    """Normalize an answer payload against its form; ValueError when invalid.

    The payload is ``{"answers": [...]}`` with one entry per question, in
    order; each entry carries ``selected`` (indices or labels) and/or
    ``text``. A blank entry is a valid answer - the user saw the
    question and chose not to answer it.
    """
    questions = form.get("questions", [])
    entries = payload.get("answers", [])
    if not isinstance(entries, list):
        raise ValueError("answers must be a list, one entry per question")
    if len(entries) != len(questions):
        raise ValueError(
            f"{len(questions)} questions were asked, "
            f"{len(entries)} answers arrived"
        )

    resolved: list[dict] = []
    for position, (question, entry) in enumerate(zip(questions, entries), start=1):
        if not isinstance(entry, dict):
            raise ValueError(f"answer {position} must be an object")
        raw = entry.get("selected", [])
        if isinstance(raw, (int, str)):
            raw = [raw]

        labels: list[str] = []
        for item in raw:
            if isinstance(item, bool) or not isinstance(item, (int, str)):
                raise ValueError(
                    f"answer {position}: selected entries must be indices or labels"
                )
            if isinstance(item, int):
                if not 0 <= item < len(question.get("options", [])):
                    raise ValueError(
                        f"answer {position}: option index {item} is out of range"
                    )
                labels.append(question["options"][item]["label"])
                continue
            label = _match_label(question.get("options", []), item)
            if label is None:
                raise ValueError(f"answer {position}: no option labeled {item!r}")
            labels.append(label)

        if len(labels) > 1 and not question.get("multiple"):
            raise ValueError(
                f"answer {position}: several choices for a single-choice question"
            )

        text = entry.get("text", "")
        if not isinstance(text, str):
            raise ValueError(f"answer {position}: text must be a string")
        resolved.append({"selected": labels, "text": text.strip()})
    return {"answers": resolved}


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
    """Blocks the run until the user answers the form (or cancel/timeout)."""

    spec = ToolSpec(name="ask_user", description=_DESCRIPTION, parameters=_PARAMETERS)

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
        form = _normalize_form(args)

        answer, status = self._coordinator.ask(
            self._run_id, form, cancel=cancel, timeout=self._timeout
        )
        if status == "cancelled":
            raise ToolError("question cancelled before it was answered")

        details = {"form": form, "answer": None, "ask_status": status}
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
        return ToolResult(output=_compose_output(form, answer), details=details)


def _normalize_form(args: dict) -> dict:
    """Validate the model's arguments and build the UI-facing payload."""

    def bad(message: str) -> ToolError:
        return ToolError(f"the tool call was not executed: {message}")

    raw_questions = args.get("questions")
    if not isinstance(raw_questions, list) or not 1 <= len(raw_questions) <= _MAX_QUESTIONS:
        raise bad(f"questions must be a list of 1 to {_MAX_QUESTIONS} questions")

    questions = []
    for position, raw in enumerate(raw_questions, start=1):
        if not isinstance(raw, dict):
            raise bad(f"question {position} must be an object")
        where = f"question {position}"

        question_text = raw.get("question")
        if not isinstance(question_text, str) or not question_text.strip():
            raise bad(f"{where}: question must be a non-empty string")
        question_text = question_text.strip()[:_MAX_QUESTION_CHARS]

        raw_options = raw.get("options")
        if not isinstance(raw_options, list) or not _MIN_OPTIONS <= len(raw_options) <= _MAX_OPTIONS:
            raise bad(f"{where}: options must be a list of 2 to 6 choices")

        options = []
        for oposition, option in enumerate(raw_options, start=1):
            if not isinstance(option, dict):
                raise bad(f"{where}: option {oposition} must be an object with a label")
            label = option.get("label")
            if not isinstance(label, str) or not label.strip():
                raise bad(f"{where}: option {oposition} needs a non-empty label")
            label = label.strip()[:_MAX_LABEL_CHARS]
            description = option.get("description") or ""
            if not isinstance(description, str):
                raise bad(f"{where}: option {oposition} description must be a string")
            entry = {"label": label}
            if description.strip():
                entry["description"] = description.strip()
            options.append(entry)

        entry = {"question": question_text, "options": options}

        header = raw.get("header")
        if header is not None:
            if not isinstance(header, str):
                raise bad(f"{where}: header must be a string")
            if header.strip():
                entry["header"] = header.strip()[:_MAX_LABEL_CHARS]

        multiple = raw.get("multiple", False)
        if not isinstance(multiple, bool):
            raise bad(f"{where}: multiple must be true or false")
        if multiple:
            entry["multiple"] = True

        questions.append(entry)

    form = {"questions": questions}

    goal = args.get("goal")
    if goal is not None:
        if not isinstance(goal, str) or not goal.strip():
            raise bad("goal must be a non-empty string when given")
        form["goal"] = goal.strip()[:_MAX_QUESTION_CHARS]

    return form


def _compose_output(form: dict, answer: dict) -> str:
    """What the model reads as the tool result: one line per question."""
    lines = []
    entries = answer.get("answers", [])
    for position, (question, entry) in enumerate(
        zip(form.get("questions", []), entries), start=1
    ):
        labels = entry.get("selected", [])
        text = entry.get("text", "")
        if labels:
            outcome = "chose " + ", ".join(f'"{label}"' for label in labels)
            if text:
                outcome += f' and said "{text}"'
        elif text:
            outcome = f'said "{text}"'
        else:
            outcome = "(no answer)"
        lines.append(f"{position}. {question['question']} → {outcome}")
    return "The user answered your questions:\n" + "\n".join(lines)
