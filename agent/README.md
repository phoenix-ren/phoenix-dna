<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA — agent daemon (`agent/`)

The Python **desktop agent daemon** behind the Phoenix DNA plasmoid. It is a pure
D-Bus service: the plasmoid (panel widget, dictation overlay, results pane)
handles speech and UI, and sends **already-transcribed prompts** here over the
session bus.

Architecture in one line: **LLM + system prompt + tools, in a loop** (the
minimal-agent design validated by pi and friends — see
`../design_docs/CODING_AGENT_ANALYSIS.md`), wrapped in a daemon with two operating modes.

```
plasmoid (C++/QML)  ──D-Bus──▶  phoenix-dna-agent (this daemon)
   speech, UI                       AgentDaemon ──▶ AgentLoop ──▶ MiniMax LLM
                                    (queue, sudo cache)   │
                                                          ▼
                                        tools: bash / read / write / edit / ls / ask_user
                                               tavily_search / exa_search
```

## Operating modes

| Mode | Auth | Working dir | Tools | Purpose |
|---|---|---|---|---|
| `root` | KDE password dialog (`kdialog`), cached 15 min | `/` | bash (via `sudo`), read, write, edit, ls, tavily_search, exa_search, ask_user | system administration; one password covers multiple prompts |
| `user` | none | `~` | bash, read, write, edit, ls, tavily_search, exa_search, ask_user | everyday desktop tasks and questions that need a web lookup |

The mode also selects the system prompt (e.g. root mode instructs the model to
inspect before changing and never reboot unasked). The search tools are
registered in both modes whenever their API keys are configured, and the
prompt then tells the model to reach for them on anything that depends on
current or specific facts (so it never answers "I cannot search the web").
Shared by both modes: commands run without a terminal and stdin is closed, so
the prompt (and the bash tool's description) demand non-interactive flags
(`dnf -y`, `apt-get -y`, `flatpak install -y`); when a command still fails
from asking a question on stdin, the bash tool appends a hint so the next
attempt fixes itself.

## Asking the user (`ask_user`)

Sometimes the right action depends on a decision only the user can make. The
`ask_user` tool (all modes) shows the user one multiple-choice question and
**blocks the run until it is answered** — the same human-in-the-loop pattern
OpenCode's `question` and Claude Code's `AskUserQuestion` tools use, so
models are already trained on its shape:

1. the model calls `ask_user` with a question, 2-6 short options (each with
   an optional description) and a `multiple` flag;
2. the daemon emits `QuestionAsked(run_id, ask_id, question_json)` and the
   tool waits — exactly like a slow bash command would;
3. the frontend renders the options (the REPL prints them numbered) and
   calls `AnswerQuestion(ask_id, answer_json)`;
4. the tool result tells the model what was chosen, plus anything the user
   wrote in their own words. Free text is passed through verbatim, so an
   answer can also be dictated — "the second one" needs no extra parsing,
   the model interprets it.

`answer_json` is `{"selected": [...], "text": "..."}` where `selected`
entries are 0-based option indices or exact labels; either part may carry
the answer alone, and a bare JSON string counts as free text. Answers that
cannot be used (unknown `ask_id`, already answered, index out of range,
unknown label, several picks on a single-choice question, empty) return
`False` — the question keeps waiting and the caller may retry.

An unanswered question releases the run after `ask_timeout_seconds`
(default 300, `0` waits forever): the model is told the user may be away
and asked to proceed on its best judgment. Cancelling the run releases it
too. The question and its answer are recorded in the session transcript
like any tool call.

**Harness confirmations.** The questions above are *model-authored* —
fine for clarifications, wrong for consent. When a root-mode bash
command matches the destructive screen (`rm -rf`, `mkfs`, `dd of=/dev/…`,
package removal, unit masking, …; extendable via `confirm_patterns`), the
daemon itself asks a question with the `harness` marker: the command and
the reason it matched are shown, the options are fixed (`Allow` / `Deny`)
and no model output can change them. The results window badges it as a
*system confirmation* (warning tint), and it is **fail-closed**: a Deny,
a timeout or a cancellation means the command does not run — the model
is told it was not run and must not retry it unchanged. Set
`confirm_destructive = false` in `agent.toml` to disable the gate
(`dnf remove` and friends then run as before, on the non-interactive
flags alone).

**Wizard metadata and the wizard view:** when the model expects several
questions, it adds `goal` (what is being collected), `step` and
`est_steps` to every call — `est_steps` may be revised as the agent
learns, which is why the progress reads "~". A question carrying `step`
opens the results window's **wizard view**: the conversation gives way
to one question at a time, with a progress bar, the answers collected
so far, and a "Skip the rest" escape hatch that answers with a text the
model reads as "proceed with your best judgment". The bridge keeps the
view up across the gaps between the series' questions and returns to
the conversation when the agent stops asking and goes to work (its
first non-`ask_user` tool call), the question times out or is
cancelled, or the run ends.

The results window renders a pending question as a card above its input
bar: one radio button per option (checkboxes when `multiple` is set), a
field for the user's own words, and the wizard progress line when the
model is asking a series. The window opens on the question by itself —
a question is activity like any other — and the card gives way when the
question is answered, times out or its run ends. Answering routes through
`AnswerQuestion` as option indices plus any typed text; the Q&A also
shows in the run's tool log. The REPL prints the same question with
numbered options and answers by number, label or free words.

## D-Bus API

Bus name `dna.phoenix.Agent`, object path `/dna/phoenix/Agent`,
interface `dna.phoenix.Agent`.

### Methods

| Method | In → Out | Description |
|---|---|---|
| `SubmitPrompt` | `sss → s` | `(prompt, mode, session_id)` → `run_id`. Empty `session_id` starts a new session; pass a previous `session_id` (from `RunStarted`) to continue it. Prompts submitted while busy are queued and run in order. |
| `SubmitPromptWithAttachments` | `ssss → s` | `(prompt, mode, session_id, attachments)` → `run_id`. The same, plus the files the user attached to the prompt: a JSON array of `{"name", "path", "mimeType"}` objects. The daemon copies them into `<data_dir>/attachments/<run_id>/`, inlines the images when the model can see them and extracts the text of PDFs — see `phoenix_dna/attachments.py`. |
| `Cancel` | `s → b` | Cancel a running prompt (kills its process group) or remove a queued one. |
| `AnswerQuestion` | `ss → b` | `(ask_id, answer_json)` → whether a pending question was answered. `answer_json` is `{"selected": [0-based index or label, …], "text": "…"}` — see [Asking the user](#asking-the-user-ask_user). |
| `SetMode` | `s → s` | `(mode)` → normalized name. Records the widget's mode selection. Selecting `root` shows the KDE password dialog immediately (off the main loop; the credential is cached as usual), switching away from `root` drops the cached password. The plasmoid calls this whenever its mode buttons change state. |
| `GetModes` | ` → a(sss)` | `(name, description, working_dir)` for root/user. |
| `GetState` | ` → (ssu)` | `(state, current_run_id, queue_length)`; state is `idle` or `running`. |
| `SearchSessions` | `ss → s` | `(user, query)` → JSON array of the user's sessions whose summary name matches the query. The match is fuzzy — every character of the query must occur in the name, in order — so typing narrows the list down; an empty query lists every session, newest first. Each entry carries `id`, `mode`, `title`, `summary` (the name the session is listed under: the title, or the first query when the session was never named), `preview`, `createdAt`, `updatedAt` and `messageCount`. |
| `GetSessionTranscript` | `ss → s` | `(user, session_id)` → JSON array of the session's messages: `role` (`user` or `assistant`), `content` and `ts`. |
| `SpeakText` | `s → ` | Speak an arbitrary string on demand (the results pane's "Read aloud"). The text is normalized and segmented exactly like an answer — math is spoken as mathematics, code fences are skipped. A no-op when speech is not enabled. |
| `StopSpeaking` | ` → ` | Interrupt playback immediately and drop what is queued. |
| `GetSpeechState` | ` → bs` | `(enabled, state)`; state is `idle` or `speaking`. |
| `Ping` | ` → s` | Version string. |

### Signals

| Signal | Signature | Emitted when |
|---|---|---|
| `RunStarted` | `sss` | run_id, mode, session_id |
| `TextDelta` | `ss` | streamed answer text (concatenate for partial replies) |
| `ToolCallStarted` | `ssss` | run_id, call_id, tool name, JSON args |
| `ToolCallFinished` | `sssss` | run_id, call_id, tool, `success|error`, output text |
| `AuthRequested` / `AuthFinished` | `ss` / `sb` | the root-mode password dialog opened / closed |
| `QuestionAsked` | `sss` | run_id, ask_id, JSON of the question: `question`, `header`, `options` (`[{label, description}]`), `multiple` — plus `goal`/`step`/`est_steps` wizard metadata when given. The run blocks until answered, cancelled or timed out. |
| `QuestionAnswered` | `ssss` | run_id, ask_id, status (`answered\|timeout\|cancelled`), JSON of the normalized answer (`{"selected": [labels], "text": "…"}`) |
| `SpeechStateChanged` | `s` | the daemon's read-aloud switched between `speaking` and `idle` |
| `StateChanged` | `ssu` | state, current_run_id, queue_length |
| `RunFinished` | `sssuuu` | run_id, status (`completed|cancelled|error|max_turns`), final text, turns, tokens in, tokens out |

Quick test from a shell:

```sh
gdbus call --session --dest dna.phoenix.Agent --object-path /dna/phoenix/Agent \
  --method dna.phoenix.Agent.SubmitPrompt "what time is it?" user ""
gdbus monitor --session --dest dna.phoenix.Agent
```

## Configuration

No keys live in this repository. The daemon reads, in order of precedence:
environment → `~/.config/phoenix-dna/keys.json` (mode 0600) for keys, and
`~/.config/phoenix-dna/agent.toml` for settings (see `config-example.toml`).

| Key / setting | Env var | Purpose |
|---|---|---|
| `minimax_api_key` | `MINIMAX_API_KEY` | LLM (`https://api.minimax.io/v1`, model `MiniMax-Text-01`; set `model = "MiniMax-M3"` for a multimodal model) |
| `tavily_api_key` | `TAVILY_API_KEY` | web search in every mode (tool omitted if unset) |
| `exa_api_key` | `EXA_API_KEY` | web search in every mode (tool omitted if unset) |
| `stt_provider`, `openai_api_key` | `PHOENIX_STT_PROVIDER`, `OPENAI_API_KEY` | the plasmoid's speech-to-text: provider (`minimax` default) and its key — read by the widget, not the daemon |
| `model`, `llm_base_url`, `max_turns`, `credential_ttl_seconds`, bash timeouts … | `PHOENIX_MODEL`, `PHOENIX_BASE_URL`, … | see `config-example.toml` |
| `ask_timeout_seconds` | — | how long an unanswered `ask_user` question may block the run before the model proceeds on its own (default 300; `0` waits forever) |
| `confirm_destructive` | — | root mode: destructive bash commands (screen in `phoenix_dna/confirm.py`) block on a harness-owned Allow/Deny, fail closed (default true) |
| `confirm_patterns` | — | extra regexes that count as destructive, e.g. `["\\bflatpak\\s+uninstall\\b"]` |

Images attached to a prompt reach the model as image parts only when it can
see them. With `vision = "auto"` (the default) that is decided from the model
name: `MiniMax-M3` and `MiniMax-M3.1` are multimodal, `MiniMax-Text-01` is
not. Set `model = "MiniMax-M3"` and `vision = true` in `agent.toml` to be
explicit. A model that cannot see images still gets the file paths, marked
as "image content is not sent to this model", so it does not try to read the
binary file and conclude that it has no vision at all.

Optional `~/.config/phoenix-dna/PHOENIX.md` (or `AGENTS.md`) is appended to the
system prompt as extra user instructions.

State lives under `~/.local/state/phoenix-dna/`: `users/<user>/sessions.db`
(the per-user SQLite history; `sessions/<mode>/<id>.jsonl` is the legacy
append-only store, imported once), `attachments/<run_id>/` (the files staged
for a prompt) and `repl_history`.

## Running

```sh
cd agent
python3 -m venv --system-site-packages .venv   # once
./.venv/bin/python -m phoenix_dna               # or: phoenix-dna-agent after pip install -e .

# no API key yet? smoke-test the daemon + D-Bus API:
./.venv/bin/python -m phoenix_dna --provider fake
./.venv/bin/python -m phoenix_dna --check-config
```

Requires distro packages `python3-dbus`, `python3-gobject` (present on Fedora).

### As a systemd service

In normal use the daemon is **not** started from a terminal: it runs as a
systemd service of the desktop user (`michelle`).

```sh
agent/systemd/install-user-service.sh          # writes ~/.config/systemd/user/phoenix.service
systemctl --user status phoenix.service
journalctl --user -u phoenix.service -f
```

The unit is enabled for `default.target`, so the daemon comes up with the
session and the plasmoid finds `dna.phoenix.Agent` on the bus without
anything being started by hand. `agent/systemd/phoenix.service.in` is the
template; the installer fills in the interpreter and the agent directory,
so the service follows the checkout it was installed from.

**Why the user manager and not a root system service:**

- The daemon owns a name on the **session bus** — that bus only exists
  inside the user's session, so a system unit would have no bus to join.
- Root mode elevates *on demand*: the daemon asks for the password with
  `kdialog` (which needs the GUI session: `DISPLAY`/`WAYLAND_DISPLAY`) and
  runs the command through `sudo`. As root, `sudo` would succeed silently
  and the password prompt would be theatre — the daemon would already have
  all the power it asks for.
- Config (`~/.config/phoenix-dna`) and state
  (`~/.local/state/phoenix-dna`) belong to the user, and `keys.json` is
  expected to be mode 0600. As root the daemon would litter the home
  directory with root-owned files.

The daemon needs a MiniMax key; without one it exits with an error, and
systemd retries a few times per minute before the unit stays in `failed`
state (`systemctl --user restart phoenix.service` after fixing the key).

## Trying it out: the REPL

`phoenix-dna-repl` (or `python -m phoenix_dna.repl`) is an interactive terminal for
exercising the agent without the plasmoid:

```sh
./.venv/bin/python -m phoenix_dna.repl                  # embedded daemon, real MiniMax
./.venv/bin/python -m phoenix_dna.repl --mode root      # start in root mode
./.venv/bin/python -m phoenix_dna.repl --provider fake  # no API key needed
./.venv/bin/python -m phoenix_dna.repl --dbus           # drive a running daemon via D-Bus
```

Commands: `/mode root|user`, `/new` (fresh session), `/session`, `/modes`,
`/state`, `/help`, `/quit`. Plain text is submitted in the current mode;
follow-ups continue the same session; **Ctrl+C while a run is active cancels
it**; root mode pops the real kdialog password prompt (cached for 15 min).
When the model asks a question, it is printed with numbered options — answer
with a number (or several, comma-separated), a label, or your own words:

```
phoenix:user> back up my disk
  ❓ (step 1 of ~2: backup setup) Which disk should the backup use?
     1) /dev/sda — the SSD
     2) /dev/sdb — the spinning one
answer> 2
  ✓ The user chose "/dev/sdb".
Backing up to /dev/sdb.
```

**Readline** (GNU readline; macOS uses libedit, Windows would need
`pyreadline3`): `↑`/`↓` recalls prompts and history persists between sessions
in `<data_dir>/repl_history` (`--history FILE` to override, `--no-history` to
disable; blank lines and consecutive duplicates are not recorded). **TAB**
completes slash commands (`/mo<TAB>` → `/mode`, `/modes`), the `/mode`
argument (`/mode us<TAB>` → `user`), and file paths — including `~` expansion
and a trailing `/` on directories (`read ~/Doc<TAB>`).

Live example (real MiniMax, user mode):

```
phoenix:user> How many days until the 5th of November next year? ...
── run run_f688b923bdb3 · user · session c3e3e33918ea
  ⚒ bash {"command": "date -d '2027-11-05' +%j; date +%j"}
  ✓ 309 ⎪ 305 ⎪ [exit 0]
The 5th of November next year is 4 days away from today.
  ● completed · 2 turns · 1290+49 tokens
```

## Testing

```sh
./.venv/bin/pip install pytest
./.venv/bin/python -m pytest -q            # unit + daemon + D-Bus tests
PHOENIX_LIVE_TEST=1 ./.venv/bin/python -m pytest tests/test_live_minimax.py -q -s   # real API calls
```

Notes for D-Bus tests: they start a **private D-Bus daemon** of their own,
so they can run while `phoenix.service` is up — they neither fight the
running daemon for the name `dna.phoenix.Agent` nor receive its signals.
Server and client use two separate connections to that bus, because sharing
one connection between them deadlocks (`call_blocking` vs. dispatch on the
same connection).

## Security notes (v1, honest)

- Root mode runs **arbitrary commands as root** once the user types the
  password — same trust model as a terminal. The password is kept in memory
  only (never logged/written), validated immediately via `sudo -S -k -v`, and
  re-used for 15 minutes (configurable) so multi-prompt sessions ask once.
- Destructive root commands (recursive forced deletion, filesystem formats,
  raw device writes, package removal, unit masking…) are gated on a
  **harness-owned Allow/Deny** — the daemon asks, the options are fixed, and
  it fails closed on timeout (see *Asking the user*). The screen is a
  conservative pattern list, not a sandbox: a destructive shape it does not
  know runs without asking (`confirm_patterns` extends it).
- No filesystem sandbox exists in any mode. A prompt-injected `rm -rf` in
  root mode is as dangerous as it sounds — the confirmation gate stops the
  known-destructive shapes, but mitigations still to consider: a
  confirm-in-pane step for *every* root write, per-project trust, or
  running the daemon in a `bubbles`/podman sandbox.
- Commands run in their own process group so cancellation and timeouts kill
  the whole tree; bash output is truncated to `max_output_chars`. Commands
  that fail because they asked for confirmation on closed stdin get a hint
  appended, nudging the model to retry with a non-interactive flag.

## Design decisions carried over from the pi research

- Four filesystem/shell tools with pi/Claude-Code-shaped schemas (models are
  RL-trained on these shapes).
- Minimal system prompt (per-mode block + current date, well under 1000
  tokens).
- No todo tool, no plan mode, no sub-agents — write a file if state must
  persist.
- Events-first: everything the agent does is an event, so the pane can render
  it and sessions record it.
- Search tools are plain JSON POST clients, not a plugin framework.
- `ask_user` is a human-in-the-loop tool with an OpenCode/Claude-Code-shaped
  schema: the loop blocks exactly as for bash, the answer returns as the tool
  result, and wizard metadata (`goal`/`step`/`est_steps`) rides along for a
  future wizard view in the results window.

## Roadmap (not built yet)

- session listing/branching methods for the pane (data model already supports it)
- compaction for long conversations
- MiniMax speech (STT/TTS) — belongs to the plasmoid, not this daemon
- "claw-like" desktop automation duties (window/clipboard/notification tools)
