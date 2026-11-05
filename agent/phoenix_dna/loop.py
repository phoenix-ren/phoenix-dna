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
from .tools import Tool, ToolError, validate_required

log = logging.getLogger("phoenix.loop")


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

        tokens_in = tokens_out = 0
        cache_tokens = reasoning_tokens = 0
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
                    status = "max_turns"
                    final_text = (
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

                for call in response.tool_calls:
                    if self._cancel.is_set():
                        status, final_text = "cancelled", ""
                        break
                    output, status_ok = self._execute_tool(call)
                    if not status_ok and self._cancel.is_set():
                        status, final_text = "cancelled", ""
                        break
                if status == "cancelled":
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
        }
        # The run is recorded before it is announced: the context view
        # asks the daemon for the session's tokens the moment the run
        # is over, and the answer must already carry this run - the
        # audit trail is where the tokens come from.
        self._sessions.record(self._conversation, "run_finished", dict(summary))
        self._emit(events.make(events.RUN_FINISHED, **summary))
        return summary

    # -- internals ---------------------------------------------------------------

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
