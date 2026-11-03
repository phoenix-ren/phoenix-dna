# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Interactive REPL for exercising the Phoenix DNA agent daemon.

Two ways to run it:

  # embedded (default): the daemon runs in this process, real provider
  python -m phoenix_dna.repl
  python -m phoenix_dna.repl --provider fake     # no API key needed

  # D-Bus client: drive a running `phoenix-dna-agent` daemon exactly like the
  # plasmoid will (same methods and signals)
  python -m phoenix_dna.repl --dbus

Anything you type is submitted as a prompt in the current mode; slash
commands control the REPL (see /help). While a run is in flight, Ctrl+C
cancels it.
"""

from __future__ import annotations

import argparse
import atexit
import json
import os
import queue
import sys
from pathlib import Path

from . import __version__
from .config import describe, load_config

try:  # GNU readline (Linux) or libedit (macOS); Windows needs pyreadline3
    import readline as _readline
except ImportError:  # pragma: no cover - non-readline platforms
    _readline = None


# ---------------------------------------------------------------------------
# readline: history + tab completion
# ---------------------------------------------------------------------------

SLASH_COMMANDS = (
    "/help",
    "/h",
    "?",
    "/mode",
    "/new",
    "/session",
    "/modes",
    "/state",
    "/cancel",
    "/quit",
    "/exit",
    "/q",
)
MODE_NAMES = ("root", "user")


def _complete_path(text: str) -> list[str]:
    """Filename completion that keeps the user's leading path text intact."""
    slash = text.rfind("/")
    if slash == -1:
        lead, prefix, dirname = "", text, "."
    else:
        lead, prefix = text[: slash + 1], text[slash + 1 :]
        dirname = os.path.expanduser(lead) or "/"
    try:
        entries = os.listdir(dirname)
    except OSError:
        return []
    matches = []
    for name in sorted(entries):
        if not name.startswith(prefix):
            continue
        if name.startswith(".") and not prefix.startswith("."):
            continue  # hidden files only on explicit "."
        full = lead + name
        if os.path.isdir(os.path.join(dirname, name)):
            full += "/"
        matches.append(full)
    return matches


class Completer:
    """Tab completion: slash commands, /mode arguments, and file paths."""

    def __init__(self, commands=SLASH_COMMANDS, modes=MODE_NAMES):
        self.commands = list(commands)
        self.modes = list(modes)

    def matches(self, text: str, buf: str | None = None) -> list[str]:
        if buf is None:
            buf = ""
            if _readline is not None:
                try:
                    buf = _readline.get_line_buffer()
                except Exception:
                    buf = ""
        before = buf[: len(buf) - len(text)] if text else buf
        stripped_before = before.rstrip()

        if stripped_before == "/mode":
            return [m for m in self.modes if m.startswith(text)]
        if text.startswith("/") and not stripped_before:
            return [c for c in self.commands if c.startswith(text)]
        return _complete_path(text)

    def complete(self, text: str, state: int):
        matches = self.matches(text)
        if state < len(matches):
            return matches[state]
        return None  # end of matches


class HistoryTracker:
    """Skips empty/consecutive-duplicate lines; mirrors into readline."""

    def __init__(self, add_fn=None, entries=None):
        self._add_fn = add_fn
        self._entries: list[str] = list(entries or [])
        self._last: str | None = None

    def add(self, line: str) -> None:
        line = line.strip()
        if not line or line == self._last:
            return
        self._last = line
        self._entries.append(line)
        if self._add_fn is not None:
            try:
                self._add_fn(line)
            except Exception:
                pass

    @property
    def entries(self) -> list[str]:
        return self._entries


def save_history(path: Path, entries: list[str]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "\n".join(entries) + ("\n" if entries else ""), encoding="utf-8"
        )
    except OSError as exc:  # history is best-effort
        print(f"warning: could not save REPL history: {exc}", file=sys.stderr)


def load_history(path: Path) -> list[str]:
    try:
        return [
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    except OSError:
        return []


def default_history_path() -> Path:
    data_dir = os.environ.get("PHOENIX_DATA_DIR") or "~/.local/state/phoenix-dna"
    return Path(data_dir).expanduser() / "repl_history"


def install_readline(history_path: Path | str | None = None):
    """Enable tab completion and persistent history; returns HistoryTracker
    (or None when readline is unavailable)."""
    if _readline is None:
        return None

    _readline.set_completer(Completer().complete)
    _readline.set_completer_delims(" \t\n")  # keep whole paths as one word
    if "libedit" in (_readline.__doc__ or ""):
        _readline.parse_and_bind("bind ^I rl_complete")
    else:
        _readline.parse_and_bind("tab: complete")

    tracker = HistoryTracker(add_fn=_readline.add_history)
    if history_path is not None:
        path = Path(history_path).expanduser()
        for entry in load_history(path):
            tracker.add(entry)  # also pushes into readline for arrow-key recall
        _readline.set_auto_history(False)  # we control what gets recorded
        atexit.register(save_history, path, tracker.entries)
    return tracker


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------


class EventPrinter:
    """Renders agent events to the terminal (shared by both REPL modes).

    ``answer_fn``, when given, is called with ``(ask_id, question)`` for
    every question the agent asks: it collects the user's answer (blocking
    is fine - the daemon is waiting too) and submits it. Both REPL modes
    wire one in; without it the question is only printed.
    """

    def __init__(self, color: bool = True, out=None, answer_fn=None):
        self.color = color
        self._out = out or sys.stdout
        self._line_open = False
        self.answer_fn = answer_fn

    # -- low level ------------------------------------------------------------

    def _c(self, code: str, text: str) -> str:
        return f"\x1b[{code}m{text}\x1b[0m" if self.color else text

    def line(self, text: str = "") -> None:
        self.close_line()
        self._out.write(text + "\n")
        self._out.flush()

    def stream(self, text: str) -> None:
        self._out.write(text)
        self._out.flush()
        self._line_open = True

    def close_line(self) -> None:
        if self._line_open:
            self._out.write("\n")
            self._out.flush()
            self._line_open = False

    # -- event handling ---------------------------------------------------------

    def on_event(self, event: dict) -> None:
        kind = event.get("kind")
        if kind == "run_started":
            self.line(
                self._c(
                    "2",
                    f"── run {event.get('run_id', '')} · "
                    f"{event.get('mode', '')} · session {event.get('session_id', '')}",
                )
            )
        elif kind == "text_delta":
            self.stream(event.get("text", ""))
        elif kind == "thinking_delta":
            # The reasoning, dim: the trail of the run, not its answer.
            self.stream(self._c("2", event.get("text", "")))
        elif kind == "tool_call_started":
            self.close_line()
            args = json.dumps(event.get("args", {}), ensure_ascii=False)
            if len(args) > 90:
                args = args[:87] + "..."
            self.line(self._c("36", f"  ⚒ {event.get('tool', '')} {args}"))
        elif kind == "tool_call_finished":
            self.close_line()
            mark = (
                self._c("32", "  ✓")
                if event.get("status") == "success"
                else self._c("31", "  ✗")
            )
            output = (event.get("output") or "").strip().splitlines() or [""]
            head = " ⎪ ".join(l[:110] for l in output[:3])
            if len(output) > 3:
                head += f" ⎪ (+{len(output) - 3} more lines)"
            self.line(f"{mark} {head}")
        elif kind == "question_asked":
            self.close_line()
            self._render_question(event.get("question", {}))
            if self.answer_fn is not None:
                self.answer_fn(event.get("ask_id", ""), event.get("question", {}))
        # question_answered: the tool_call_finished of ask_user shows the
        # outcome; a second rendering would only repeat it
        elif kind == "auth_requested":
            self.close_line()
            self.line(
                self._c("33", "  🔑 administrator password requested (KDE dialog)")
            )
        elif kind == "auth_finished":
            ok = bool(event.get("success"))
            self.line(
                self._c(
                    "32" if ok else "31",
                    f"     password {'accepted' if ok else 'failed or cancelled'}",
                )
            )
        elif kind == "run_finished":
            self.close_line()
            status = event.get("status", "")
            colored = (
                self._c("32", status)
                if status == "completed"
                else self._c("31", status)
            )
            self.line(
                f"  ● {colored} · {event.get('turns', 0)} turns · "
                f"{event.get('tokens_in', 0)}+{event.get('tokens_out', 0)} tokens"
            )
        # turn_started / state_changed: too chatty for the REPL

    def _render_question(self, question: dict) -> None:
        """Print the question, numbered options, and how to answer."""
        if question.get("harness"):
            # A daemon-authored confirmation (destructive root command):
            # worth naming as such before the command it shows.
            self.line(
                self._c("31", "  ⚠ system confirmation — required before the command runs")
            )
        context = ""
        if question.get("step"):
            context = f"(step {question['step']} of ~{question.get('est_steps', '?')}"
            if question.get("goal"):
                context += f": {question['goal']}"
            context += ") "
        self.line(self._c("33", f"  ❓ {context}{question.get('question', '')}"))
        for position, option in enumerate(question.get("options", []), start=1):
            line = f"     {position}) {option.get('label', '')}"
            if option.get("description"):
                line += f" — {option['description']}"
            self.line(line)
        hint = "a number"
        if question.get("multiple"):
            hint = "numbers, comma-separated"
        self.line(
            self._c("2", f"     (answer with {hint}, a label, or your own words)")
        )


# ---------------------------------------------------------------------------
# answering questions from the terminal (unit-tested helpers)
# ---------------------------------------------------------------------------


def parse_answer_line(line: str, question: dict) -> dict:
    """Turn a typed reply into an answer payload for AnswerQuestion.

    Numbers pick options (1-based, as printed), labels match their text,
    comma-separated entries pick several, and anything else is the user's
    own words. An empty line yields ``{}`` - the caller re-prompts.
    """
    options = question.get("options", [])
    selected: list[int] = []
    texts: list[str] = []
    for token in (part.strip() for part in line.split(",")):
        if not token:
            continue
        if token.isdigit():
            index = int(token)
            if 1 <= index <= len(options):
                selected.append(index - 1)
                continue
        label = token.casefold()
        for index, option in enumerate(options):
            if str(option.get("label", "")).strip().casefold() == label:
                selected.append(index)
                break
        else:
            texts.append(token)
    answer: dict = {}
    if selected:
        answer["selected"] = selected
    if texts:
        answer["text"] = ", ".join(texts)
    return answer


def read_answer(question: dict, input_fn=input) -> dict:
    """Prompt for an answer until a non-empty one is given.

    Ctrl+C propagates (the REPL turns it into cancelling the run - the
    blocked ask_user is released the same way); Ctrl+D degrades to an
    explicit "no answer came".
    """
    while True:
        try:
            line = input_fn("answer> ").strip()
        except EOFError:
            return {"text": "(the input was closed without an answer)"}
        answer = parse_answer_line(line, question)
        if answer:
            return answer


# ---------------------------------------------------------------------------
# event plumbing helpers (unit-tested)
# ---------------------------------------------------------------------------


def wait_for_run(events: queue.Queue, run_id: str, *, on_event=None) -> dict:
    """Consume events until the RunFinished for run_id arrives; return it."""
    while True:
        event = events.get()
        if on_event is not None:
            on_event(event)
        if event.get("kind") == "run_finished" and event.get("run_id") == run_id:
            return event


def dbus_signal_to_event(name: str, *args) -> dict | None:
    """Convert a D-Bus signal into the daemon's event-dict shape."""
    if name == "RunStarted":
        return {
            "kind": "run_started",
            "run_id": str(args[0]),
            "mode": str(args[1]),
            "session_id": str(args[2]),
        }
    if name == "TextDelta":
        return {"kind": "text_delta", "run_id": str(args[0]), "text": str(args[1])}
    if name == "ToolCallStarted":
        try:
            parsed = json.loads(str(args[3]))
        except ValueError:
            parsed = {}
        return {
            "kind": "tool_call_started",
            "run_id": str(args[0]),
            "call_id": str(args[1]),
            "tool": str(args[2]),
            "args": parsed,
        }
    if name == "ToolCallFinished":
        return {
            "kind": "tool_call_finished",
            "run_id": str(args[0]),
            "call_id": str(args[1]),
            "tool": str(args[2]),
            "status": str(args[3]),
            "output": str(args[4]),
        }
    if name == "AuthRequested":
        return {
            "kind": "auth_requested",
            "run_id": str(args[0]),
            "message": str(args[1]),
        }
    if name == "AuthFinished":
        return {
            "kind": "auth_finished",
            "run_id": str(args[0]),
            "success": bool(args[1]),
        }
    if name == "QuestionAsked":
        try:
            parsed = json.loads(str(args[2]))
        except ValueError:
            parsed = {}
        return {
            "kind": "question_asked",
            "run_id": str(args[0]),
            "ask_id": str(args[1]),
            "question": parsed,
        }
    if name == "QuestionAnswered":
        try:
            parsed = json.loads(str(args[3]))
        except ValueError:
            parsed = {}
        return {
            "kind": "question_answered",
            "run_id": str(args[0]),
            "ask_id": str(args[1]),
            "ask_status": str(args[2]),
            "answer": parsed,
        }
    if name == "RunFinished":
        return {
            "kind": "run_finished",
            "run_id": str(args[0]),
            "status": str(args[1]),
            "final_text": str(args[2]),
            "turns": int(args[3]),
            "tokens_in": int(args[4]),
            "tokens_out": int(args[5]),
        }
    return None  # StateChanged and anything unknown


HELP = """\
Commands (Tab completes commands, /mode arguments and file paths):
  /mode <root|user>      switch operating mode
  /new                   start a fresh session in the current mode
  /session               show the current session id
  /modes                 list operating modes
  /state                 show daemon state
  /cancel                cancel the running prompt (also Ctrl+C while waiting)
  /help                  this help
  /quit                  exit (also Ctrl+D)
Anything else is submitted as a prompt in the current mode.
When the agent asks a question, answer with a number (or several,
comma-separated), an option's label, or your own words; Ctrl+C cancels
the run instead of answering.
History: ↑/↓ recalls earlier prompts and persists between sessions."""


# ---------------------------------------------------------------------------
# shared REPL loop
# ---------------------------------------------------------------------------


def repl_loop(
    *,
    submit,  # (prompt, mode, session_id) -> run_id
    cancel_run,  # (run_id) -> bool
    state_fn=None,  # () -> str description, or None
    modes_fn=None,  # () -> [(name, description, dir)]
    events: queue.Queue,  # event source consumed while waiting
    printer: EventPrinter,
    initial_mode: str = "user",
    initial_session: str = "",
    history: HistoryTracker | None = None,
):
    sessions: dict[str, str] = {}
    if initial_session:
        sessions[initial_mode] = initial_session
    mode = initial_mode
    bold = "\x1b[1m" if printer.color else ""
    reset = "\x1b[0m" if printer.color else ""
    printer.line(
        f"phoenix-dna-agent REPL {__version__} — /help for commands, Ctrl+D to quit"
    )

    while True:
        try:
            raw = input(f"{bold}phoenix:{mode}{reset}> ")
        except EOFError:
            printer.line()
            return
        except KeyboardInterrupt:
            printer.line()
            continue

        if history is not None:
            history.add(raw)

        line = raw.strip()
        if not line:
            continue

        # -- slash commands -------------------------------------------------
        if line in ("/quit", "/exit", "/q"):
            return
        if line in ("/help", "/h", "?"):
            printer.line(HELP)
            continue
        if line == "/new":
            sessions.pop(mode, None)
            printer.line(f"new session for mode {mode}")
            continue
        if line == "/session":
            printer.line(f"session: {sessions.get(mode, '(new each run)')}")
            continue
        if line == "/modes":
            if modes_fn:
                for name, description, workdir in modes_fn():
                    mark = "*" if name == mode else " "
                    printer.line(f"{mark} {name:5} {workdir:12} {description}")
            else:
                printer.line("root / user (built-in)")
            continue
        if line == "/state":
            printer.line(state_fn() if state_fn else "state not available")
            continue
        if line == "/cancel":
            # without a known active run this is a no-op hint
            printer.line("use Ctrl+C while a prompt is running to cancel it")
            continue
        if line.startswith("/mode"):
            parts = line.split()
            if len(parts) != 2 or parts[1] not in MODE_NAMES:
                printer.line(f"usage: /mode <{'|'.join(MODE_NAMES)}>")
                continue
            mode = parts[1]
            printer.line(f"mode → {mode} (session: {sessions.get(mode, 'new')})")
            continue
        if line.startswith("/"):
            printer.line(f"unknown command {line!r} — /help")
            continue

        # -- submit a prompt --------------------------------------------------
        try:
            run_id = submit(line, mode, sessions.get(mode, ""))
        except Exception as exc:  # ValueError (bad mode) or D-Bus errors
            printer.line(f"submit failed: {exc}")
            continue

        cancelled_once = False
        while True:
            try:
                finished = wait_for_run(events, run_id, on_event=printer.on_event)
                break
            except KeyboardInterrupt:
                if not cancelled_once:
                    cancelled_once = True
                    printer.line(cancel_notice())
                    try:
                        cancel_run(run_id)
                    except Exception:
                        pass
                else:
                    printer.line("(already cancelling — waiting for the agent to stop)")
        # remember the session for follow-ups in this mode
        session_id = _session_for_run(run_id)
        if session_id:
            sessions[mode] = session_id


def cancel_notice() -> str:
    return "  ^C — cancelling current run…"


_sessions_by_run: dict[str, str] = {}


def _remember_run_session(run_id: str, session_id: str) -> None:
    if session_id:
        _sessions_by_run[run_id] = session_id


def _session_for_run(run_id: str) -> str:
    return _sessions_by_run.pop(run_id, "")


# ---------------------------------------------------------------------------
# embedded mode
# ---------------------------------------------------------------------------


def run_embedded(args, history=None) -> int:
    from .daemon import AgentDaemon
    from .provider import build_provider

    config = load_config(config_dir=args.config_dir, data_dir=args.data_dir)
    if args.provider:
        import dataclasses

        config = dataclasses.replace(config, provider=args.provider)
    try:
        provider = build_provider(config)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    events: queue.Queue = queue.Queue()
    daemon = AgentDaemon(config, provider=provider, emitter=events.put)
    printer = EventPrinter(
        color=not args.no_color, answer_fn=_make_answer_fn(daemon)
    )

    submit = daemon.submit

    state = lambda: json.dumps(daemon.state())  # noqa: E731

    def modes():
        from .modes import MODE_PROFILES

        return [
            (p.mode.value, p.description, str(p.working_dir(config)))
            for p in MODE_PROFILES.values()
        ]

    _patch_printer_for_sessions(printer)
    try:
        repl_loop(
            submit=submit,
            cancel_run=daemon.cancel,
            state_fn=state,
            modes_fn=modes,
            events=events,
            printer=printer,
            initial_mode=args.mode,
            initial_session=args.session,
            history=history,
        )
    finally:
        daemon.shutdown()
    return 0


def _make_answer_fn(daemon):
    """Collect and submit terminal answers for questions the agent asks."""

    def answer(ask_id: str, question: dict) -> None:
        payload = read_answer(question)
        try:
            daemon.answer_question(
                ask_id, json.dumps(payload, ensure_ascii=False)
            )
        except Exception as exc:
            print(f"answer failed: {exc}", file=sys.stderr)

    return answer


def _patch_printer_for_sessions(printer: EventPrinter) -> None:
    original = printer.on_event

    def patched(event: dict) -> None:
        if event.get("kind") == "run_started":
            _remember_run_session(event.get("run_id", ""), event.get("session_id", ""))
        original(event)

    printer.on_event = patched


# ---------------------------------------------------------------------------
# D-Bus client mode
# ---------------------------------------------------------------------------


def run_dbus(args, history=None) -> int:
    try:
        import dbus
        from dbus.mainloop.glib import DBusGMainLoop
    except ImportError:
        print("error: python3-dbus is required for --dbus", file=sys.stderr)
        return 1

    from .service import BUS_NAME, INTERFACE, OBJECT_PATH

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    if not bus.name_has_owner(BUS_NAME):
        print(
            f"error: no daemon is running as {BUS_NAME}.\n"
            "start it first:  python -m phoenix_dna   (in another terminal)",
            file=sys.stderr,
        )
        return 1

    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    iface = dbus.Interface(proxy, INTERFACE)

    events: queue.Queue = queue.Queue()
    printer = EventPrinter(
        color=not args.no_color, answer_fn=_make_dbus_answer_fn(iface)
    )
    _patch_printer_for_sessions(printer)

    signal_names = [
        "RunStarted",
        "TextDelta",
        "ToolCallStarted",
        "ToolCallFinished",
        "AuthRequested",
        "AuthFinished",
        "QuestionAsked",
        "QuestionAnswered",
        "RunFinished",
    ]
    for name in signal_names:
        bus.add_signal_receiver(
            lambda *a, _n=name: _enqueue_signal(events, printer, _n, *a),
            signal_name=name,
            dbus_interface=INTERFACE,
        )

    # run the GLib main loop so signals get dispatched
    from gi.repository import GLib

    loop = GLib.MainLoop()
    import threading

    threading.Thread(target=loop.run, daemon=True).start()

    def state():
        state_, current, qlen = iface.GetState()
        return f"state={state_} current={current or '-'} queue={qlen}"

    def modes():
        return [(str(m[0]), str(m[1]), str(m[2])) for m in iface.GetModes()]

    try:
        repl_loop(
            submit=lambda p, m, s: str(iface.SubmitPrompt(p, m, s)),
            cancel_run=lambda run_id: bool(iface.Cancel(run_id)),
            state_fn=state,
            modes_fn=modes,
            events=events,
            printer=printer,
            initial_mode=args.mode,
            initial_session=args.session,
            history=history,
        )
    finally:
        loop.quit()
    return 0


def _make_dbus_answer_fn(iface):
    """Like _make_answer_fn, but the answer travels over D-Bus."""

    def answer(ask_id: str, question: dict) -> None:
        payload = read_answer(question)
        try:
            iface.AnswerQuestion(ask_id, json.dumps(payload, ensure_ascii=False))
        except Exception as exc:
            print(f"answer failed: {exc}", file=sys.stderr)

    return answer


def _enqueue_signal(
    events: queue.Queue, printer: EventPrinter, name: str, *args
) -> None:
    event = dbus_signal_to_event(name, *args)
    if event is not None:
        events.put(event)


# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="phoenix-dna-repl",
        description="Interactive REPL for the Phoenix DNA agent daemon",
    )
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument(
        "--dbus",
        action="store_true",
        help="connect to a running daemon over D-Bus (like the plasmoid) "
        "instead of embedding the daemon in this process",
    )
    parser.add_argument("--mode", choices=list(MODE_NAMES), default="user")
    parser.add_argument("--session", default="", help="continue a session id")
    parser.add_argument(
        "--provider", choices=["minimax", "fake"], help="LLM provider (embedded mode)"
    )
    parser.add_argument("--config-dir")
    parser.add_argument("--data-dir")
    parser.add_argument("--no-color", action="store_true")
    parser.add_argument(
        "--history",
        default=None,
        metavar="FILE",
        help=f"history file (default: {default_history_path()})",
    )
    parser.add_argument(
        "--no-history",
        action="store_true",
        help="do not persist history between sessions",
    )
    args = parser.parse_args(argv)

    history_path = (
        None
        if args.no_history
        else (
            Path(args.history).expanduser() if args.history else default_history_path()
        )
    )
    history = install_readline(history_path)

    if args.dbus:
        return run_dbus(args, history=history)
    return run_embedded(args, history=history)


if __name__ == "__main__":
    raise SystemExit(main())
