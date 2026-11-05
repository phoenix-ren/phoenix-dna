# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The agent loop: LLM + system prompt + tools, run until answered.

This is the pi-derived core: append the user message, request a completion,
execute tool calls sequentially, append their results, and repeat until the
model answers without tool calls. Everything that happens is emitted as an
event dict through ``event_sink`` so any frontend (D-Bus today, whatever
tomorrow) can render it.
"""

from __future__ import annotations

import logging
import threading

from . import events
from .attachments import build_user_message
from .provider import CancelledError, LLMError, LLMProvider, ToolSpec
from .sessions import Conversation, SessionManager
from .tools import Tool, ToolError, trim_output, validate_required

log = logging.getLogger("phoenix.loop")


#: What the agent is told when a run has to stop: the turns are spent
#: or the window is filling, and it must answer with what it has
#: rather than call another tool.
_WRAP_UP_PROMPT = """\
Stop working on this task and answer now: {reason}.

Say what you did, what you found, and what is left to do. Do not call \
any tool - you may not use one any more. Be brief: this is the answer \
the user is waiting for."""


class AgentLoop:
    def __init__(
        self,
        *,
        run_id: str,
        provider: LLMProvider,
        tools: dict[str, Tool],
        system_prompt: str,
        conversation: Conversation,
        session_manager: SessionManager,
        event_sink,
        max_turns: int = 24,
        cancel_event: threading.Event | None = None,
        vision: bool = False,
        context_window: int = 0,
        context_budget: float = 0.8,
        tool_output_chars: int = 6000,
        compactor=None,
    ):
        self.run_id = run_id
        self._provider = provider
        self._tools = tools
        self._system_prompt = system_prompt
        self._conversation = conversation
        self._sessions = session_manager
        self._emit = event_sink
        self.max_turns = max_turns
        self._cancel = cancel_event or threading.Event()
        # Whether the model may be sent image parts (see attachments.py).
        self._vision = vision
        # The model's context window, and the share of it a request's
        # prompt may take: a run that comes near it stops calling tools
        # and answers with what it has, rather than being refused by
        # the model mid-flight.
        self._context_window = context_window
        self._context_budget = context_budget
        # What one tool call may add to the conversation.
        self._tool_output_chars = tool_output_chars
        # The callable that summarises the conversation when it has
        # grown too big to carry (see the daemon's compaction).
        self._compactor = compactor

    # -- public API -----------------------------------------------------------

    def cancel(self) -> None:
        self._cancel.set()

    def run(self, prompt: str, attachments: list | None = None) -> dict:
        """Run the loop to completion. Returns the run summary.

        ``attachments`` are the files the user added to the prompt; they
        become part of the user message (inlined images, extracted PDF
        text, and a list of the files for the agent to inspect).
        """
        self._emit(
            events.make(
                events.RUN_STARTED,
                run_id=self.run_id,
                session_id=self._conversation.id,
            )
        )
        self._sessions.record(
            self._conversation,
            "run_started",
            {"run_id": self.run_id},
        )
        self._sessions.append(
            self._conversation,
            build_user_message(prompt, list(attachments or []), vision=self._vision),
        )

        # The conversation may have grown too big to carry: the older
        # turns are summarised before the run goes on, so that the
        # window is not filled by what happened long ago.
        self._maybe_compact()

        tokens_in = tokens_out = 0
        cache_tokens = reasoning_tokens = 0
        context_tokens = 0
        turns = 0
        last_text = ""
        status = "error"
        final_text = ""

        try:
            while True:
                if self._cancel.is_set():
                    status, final_text = "cancelled", ""
                    break
                turns += 1
                if turns > self.max_turns:
                    # The turns are spent: the agent answers with what
                    # it has, instead of the run ending in nothing.
                    status = "max_turns"
                    final_text = self._wrap_up(
                        "you have used up the turns you had for this task"
                    ) or (
                        last_text or "Stopped: too many tool turns without an answer."
                    )
                    break
                self._emit(
                    events.make(events.TURN_STARTED, run_id=self.run_id, turn=turns)
                )

                stream = self._provider.stream(
                    self._request_messages(),
                    [tool.spec for tool in self._tools.values()],
                    should_cancel=self._cancel.is_set,
                )
                response = None
                for item in stream:
                    if item.get("type") == "text_delta":
                        self._emit(
                            events.make(
                                events.TEXT_DELTA,
                                run_id=self.run_id,
                                text=item["text"],
                                bytes_in=int(item.get("bytes_in", 0) or 0),
                            )
                        )
                    elif item.get("type") == "thinking_delta":
                        self._emit(
                            events.make(
                                events.THINKING_DELTA,
                                run_id=self.run_id,
                                text=item["text"],
                                bytes_in=int(item.get("bytes_in", 0) or 0),
                            )
                        )
                    elif item.get("type") == "response":
                        response = item["response"]
                if response is None:  # pragma: no cover - providers must send one
                    raise LLMError("provider stream ended without a response")

                if response.usage is not None:
                    tokens_in += response.usage.input_tokens
                    tokens_out += response.usage.output_tokens
                    cache_tokens += response.usage.cache_tokens
                    reasoning_tokens += response.usage.reasoning_tokens
                    # How full the window was for this request: the
                    # prompt the whole conversation made. The largest
                    # one of the run is how full it got - which is what
                    # the context view shows, as opposed to the sum
                    # above, which counts the same tokens once per
                    # request.
                    context_tokens = max(context_tokens, response.usage.input_tokens)
                last_text = response.text or last_text

                # The turn's wire traffic: the request that went out and
                # the answer that came in. The frontend meters the rate
                # of the connection from these.
                self._emit(
                    events.make(
                        events.WIRE_BYTES,
                        run_id=self.run_id,
                        turn=turns,
                        bytes_sent=int(getattr(response, "bytes_sent", 0) or 0),
                        bytes_received=int(
                            getattr(response, "bytes_received", 0) or 0
                        ),
                    )
                )

                self._sessions.append(
                    self._conversation,
                    self._assistant_message(response),
                )

                if not response.tool_calls:
                    status, final_text = "completed", response.text
                    break

                answered: set[str] = set()
                for call in response.tool_calls:
                    if self._cancel.is_set():
                        status, final_text = "cancelled", ""
                        break
                    output, status_ok = self._execute_tool(call)
                    answered.add(call.id)
                    if not status_ok and self._cancel.is_set():
                        status, final_text = "cancelled", ""
                        break
                if status == "cancelled":
                    # The calls that were not run need an answer too: a
                    # tool call without a result is a conversation the
                    # model refuses to read, and the next run replays
                    # this one.
                    for call in response.tool_calls:
                        if call.id not in answered:
                            self._sessions.append(
                                self._conversation,
                                {
                                    "role": "tool",
                                    "tool_call_id": call.id,
                                    "content": "the run was cancelled before this call "
                                    "was run",
                                },
                            )
                    break

                # The window: the request that just went out filled it
                # this much, and the next one would fill it more. A run
                # that comes near the limit stops here and answers with
                # what it has - the model would refuse the next request
                # outright, and the work done so far would be lost.
                #
                # The check is here, after the tools have run, so that
                # the conversation the wrap-up sends is whole.
                if self._window_is_full(context_tokens):
                    status = "max_context"
                    final_text = self._wrap_up(
                        "the conversation is about to fill the model's context window"
                    ) or last_text
                    break
        except CancelledError:
            status, final_text = "cancelled", ""
        except LLMError as exc:
            log.warning("run %s failed: %s", self.run_id, exc)
            status, final_text = "error", f"error: {exc}"
        except Exception as exc:  # defensive: never leak a crash to the frontend
            log.exception("run %s crashed", self.run_id)
            status, final_text = "error", f"internal error: {exc}"

        summary = {
            "run_id": self.run_id,
            "status": status,
            "final_text": final_text,
            "turns": turns,
            "tokens_in": tokens_in,
            "tokens_out": tokens_out,
            "cache_tokens": cache_tokens,
            "reasoning_tokens": reasoning_tokens,
            # The biggest prompt of the run: how full the model's
            # context window was when it answered.
            "context_tokens": context_tokens,
        }
        # The run is recorded before it is announced: the context view
        # asks the daemon for the session's tokens the moment the run
        # is over, and the answer must already carry this run - the
        # audit trail is where the tokens come from.
        self._sessions.record(self._conversation, "run_finished", dict(summary))
        self._emit(events.make(events.RUN_FINISHED, **summary))
        return summary

    # -- internals ---------------------------------------------------------------

    def _window_is_full(self, context_tokens: int) -> bool:
        """Whether the next request would not fit in the window.

        The size of the request that just went out is known exactly -
        the provider counted it - and the next one would be a little
        bigger. A window that is not known (the model has none, or the
        user named none) is never full.
        """
        if self._context_window <= 0 or context_tokens <= 0:
            return False
        return context_tokens >= self._context_window * self._context_budget

    def _wrap_up(self, reason: str) -> str:
        """One last call, without tools: the answer to what was done.

        A run that stops - because the turns ran out, or because the
        window is filling - ends with what the agent has, not with
        nothing: the model is asked to say what it did, what it found
        and what is left to do, and it may not call a tool while doing
        so.

        Returns the answer, or an empty string when the call failed (the
        run then falls back to whatever it had).
        """
        messages = self._request_messages() + [
            {"role": "user", "content": _WRAP_UP_PROMPT.format(reason=reason)}
        ]
        try:
            parts: list[str] = []
            for item in self._provider.stream(
                messages, [], should_cancel=self._cancel.is_set
            ):
                if item.get("type") == "text_delta":
                    parts.append(item["text"])
            return "".join(parts).strip()
        except (CancelledError, LLMError) as exc:
            log.warning("wrap-up of run %s failed: %s", self.run_id, exc)
            return ""
        except Exception:  # defensive: a wrap-up must never crash the run
            log.exception("wrap-up of run %s crashed", self.run_id)
            return ""

    def _maybe_compact(self) -> None:
        """Summarise the older turns when the conversation is too big.

        The daemon's compactor decides whether it is time (it knows the
        model's context window) and does the summarising; the loop only
        asks, once the prompt is part of the conversation. It gets the
        summary back, and says how many messages it stands for.
        """
        if self._compactor is None:
            return
        try:
            summarised = self._compactor(self._conversation)
        except Exception:  # defensive: a failed compaction is not a failed run
            log.exception("compaction of session %s failed", self._conversation.id)
            return
        if not summarised:
            return
        log.info(
            "session %s was summarised: %d messages became a summary",
            self._conversation.id,
            summarised,
        )
        self._emit(
            events.make(
                events.COMPACTION,
                run_id=self.run_id,
                session_id=self._conversation.id,
                summarised=summarised,
            )
        )

    def _request_messages(self) -> list[dict]:
        return [{"role": "system", "content": self._system_prompt}] + list(
            self._conversation.messages
        )

    @staticmethod
    def _assistant_message(response) -> dict:
        # The history carries the content as the model sent it, thinking
        # included: the model reads its own past reasoning. Frontends get
        # the answer separately (final_text is response.text).
        message = {"role": "assistant", "content": response.raw_text or response.text}
        if response.tool_calls:
            message["tool_calls"] = [call.to_openai() for call in response.tool_calls]
        return message

    def _execute_tool(self, call) -> tuple[str, bool]:
        """Execute one tool call; returns (output, ok). Never raises."""
        self._emit(
            events.make(
                events.TOOL_CALL_STARTED,
                run_id=self.run_id,
                call_id=call.id,
                tool=call.name,
                args=call.arguments,
            )
        )
        tool = self._tools.get(call.name)
        try:
            if call.arguments_error is not None:
                raise ToolError(
                    "the tool call was not executed: its arguments are not "
                    f"valid JSON ({call.arguments_error}). Re-emit this "
                    "tool call with valid JSON arguments."
                )
            if tool is None:
                raise ToolError(
                    f"unknown tool '{call.name}' (available: "
                    f"{', '.join(sorted(self._tools))})"
                )
            validate_required(tool.spec, call.arguments)
            result = tool.execute(call.arguments, cancel=self._cancel)
            output, status, details = result.output, "success", result.details
        except ToolError as exc:
            output, status, details = str(exc), "error", {}
        except Exception as exc:
            log.exception("tool %s crashed", call.name)
            output, status, details = f"internal error in tool: {exc}", "error", {}

        # What the tool brought back is cut to what the conversation may
        # carry: the conversation is sent again with every turn, so a
        # tool that returns a megabyte would fill the model's window on
        # its own. The tool's own details (what the pane shows) are
        # untouched.
        output = trim_output(output, self._tool_output_chars)

        self._emit(
            events.make(
                events.TOOL_CALL_FINISHED,
                run_id=self.run_id,
                call_id=call.id,
                tool=call.name,
                status=status,
                output=output,
                details=details,
            )
        )
        self._sessions.append(
            self._conversation,
            {"role": "tool", "tool_call_id": call.id, "content": output},
        )
        return output, status == "success"
