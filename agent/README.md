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
`../design_docs/CODING_AGENT_ANALYSIS.md`), wrapped in a daemon with three operating modes.

```
plasmoid (C++/QML)  ──D-Bus──▶  phoenix-dna-agent (this daemon)
   speech, UI                       AgentDaemon ──▶ AgentLoop ──▶ MiniMax LLM
                                    (queue, sudo cache)   │
                                                          ▼
                                        tools: bash / read / write / edit / ls
                                               tavily_search / exa_search
```

## Operating modes

| Mode | Auth | Working dir | Tools | Purpose |
|---|---|---|---|---|
| `root` | KDE password dialog (`kdialog`), cached 15 min | `/` | bash (via `sudo`), read, write, edit, ls | system administration; one password covers multiple prompts |
| `user` | none | `~` | bash, read, write, edit, ls | everyday desktop tasks |
| `web` | none | hidden scratch dir (`~/.local/state/phoenix-dna/scratch/web`) | bash (scratch only), tavily_search, exa_search | general-knowledge questions and web research |

The mode also selects the system prompt (e.g. root mode instructs the model to
inspect before changing and never reboot unasked; web mode tells it to compute
date arithmetic with `date`/`python3` instead of guessing). Shared by all
modes: commands run without a terminal and stdin is closed, so the prompt (and
the bash tool's description) demand non-interactive flags (`dnf -y`,
`flatpak install -y`); when a command still fails from asking a question on
stdin, the bash tool appends a hint so the next attempt fixes itself.

## D-Bus API

Bus name `dna.phoenix.Agent`, object path `/dna/phoenix/Agent`,
interface `dna.phoenix.Agent`.

### Methods

| Method | In → Out | Description |
|---|---|---|
| `SubmitPrompt` | `sss → s` | `(prompt, mode, session_id)` → `run_id`. Empty `session_id` starts a new session; pass a previous `session_id` (from `RunStarted`) to continue it. Prompts submitted while busy are queued and run in order. |
| `SubmitPromptWithAttachments` | `ssss → s` | `(prompt, mode, session_id, attachments)` → `run_id`. The same, plus the files the user attached to the prompt: a JSON array of `{"name", "path", "mimeType"}` objects. The daemon copies them into `<data_dir>/attachments/<run_id>/`, inlines the images when the model can see them and extracts the text of PDFs — see `phoenix_dna/attachments.py`. |
| `Cancel` | `s → b` | Cancel a running prompt (kills its process group) or remove a queued one. |
| `SetMode` | `s → s` | `(mode)` → normalized name. Records the widget's mode selection. Selecting `root` shows the KDE password dialog immediately (off the main loop; the credential is cached as usual), switching away from `root` drops the cached password. The plasmoid calls this whenever its mode buttons change state. |
| `GetModes` | ` → a(sss)` | `(name, description, working_dir)` for root/user/web. |
| `GetState` | ` → (ssu)` | `(state, current_run_id, queue_length)`; state is `idle` or `running`. |
| `Ping` | ` → s` | Version string. |

### Signals

| Signal | Signature | Emitted when |
|---|---|---|
| `RunStarted` | `sss` | run_id, mode, session_id |
| `TextDelta` | `ss` | streamed answer text (concatenate for partial replies) |
| `ToolCallStarted` | `ssss` | run_id, call_id, tool name, JSON args |
| `ToolCallFinished` | `sssss` | run_id, call_id, tool, `success|error`, output text |
| `AuthRequested` / `AuthFinished` | `ss` / `sb` | the root-mode password dialog opened / closed |
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
| `minimax_api_key` | `MINIMAX_API_KEY` | LLM (`https://api.minimax.io/v1`, model `MiniMax-Text-01`) |
| `tavily_api_key` | `TAVILY_API_KEY` | web-mode search (tool omitted if unset) |
| `exa_api_key` | `EXA_API_KEY` | web-mode search (tool omitted if unset) |
| `model`, `llm_base_url`, `max_turns`, `credential_ttl_seconds`, bash timeouts … | `PHOENIX_MODEL`, `PHOENIX_BASE_URL`, … | see `config-example.toml` |

Optional `~/.config/phoenix-dna/PHOENIX.md` (or `AGENTS.md`) is appended to the
system prompt as extra user instructions.

State lives under `~/.local/state/phoenix-dna/`: `sessions/<mode>/<id>.jsonl`
(append-only transcripts) and `scratch/web/`.

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

## Trying it out: the REPL

`phoenix-dna-repl` (or `python -m phoenix_dna.repl`) is an interactive terminal for
exercising the agent without the plasmoid:

```sh
./.venv/bin/python -m phoenix_dna.repl                  # embedded daemon, real MiniMax
./.venv/bin/python -m phoenix_dna.repl --mode web       # start in web mode
./.venv/bin/python -m phoenix_dna.repl --provider fake  # no API key needed
./.venv/bin/python -m phoenix_dna.repl --dbus           # drive a running daemon via D-Bus
```

Commands: `/mode root|user|web`, `/new` (fresh session), `/session`, `/modes`,
`/state`, `/help`, `/quit`. Plain text is submitted in the current mode;
follow-ups continue the same session; **Ctrl+C while a run is active cancels
it**; root mode pops the real kdialog password prompt (cached for 15 min).

**Readline** (GNU readline; macOS uses libedit, Windows would need
`pyreadline3`): `↑`/`↓` recalls prompts and history persists between sessions
in `<data_dir>/repl_history` (`--history FILE` to override, `--no-history` to
disable; blank lines and consecutive duplicates are not recorded). **TAB**
completes slash commands (`/mo<TAB>` → `/mode`, `/modes`), the `/mode`
argument (`/mode we<TAB>` → `web`), and file paths — including `~` expansion
and a trailing `/` on directories (`read ~/Doc<TAB>`).

Live example (real MiniMax, web mode):

```
phoenix:web> How many days until the 5th of November next year? ...
── run run_f688b923bdb3 · web · session c3e3e33918ea
  ⚒ bash {"command": "date -d '2027-11-05' +%j; date +%j"}
  ✓ 309 ⎪ 305 ⎪ [exit 0]
The 5th of November next year is 4 days away from today.
  ● completed · 2 turns · 1290+49 tokens
```

## Testing

```sh
./.venv/bin/pip install pytest
./.venv/bin/python -m pytest -q            # unit + daemon + D-Bus tests (uses your session bus)
PHOENIX_LIVE_TEST=1 ./.venv/bin/python -m pytest tests/test_live_minimax.py -q -s   # real API calls
```

Notes for D-Bus tests: they own `dna.phoenix.Agent` on your session bus for the
duration of the test run, and use `SessionBus(private=True)` for the client
side because `dbus.SessionBus()` is a process-wide singleton (sharing it
between server and client deadlocks).

## Security notes (v1, honest)

- Root mode runs **arbitrary commands as root** once the user types the
  password — same trust model as a terminal. The password is kept in memory
  only (never logged/written), validated immediately via `sudo -S -k -v`, and
  re-used for 15 minutes (configurable) so multi-prompt sessions ask once.
- No filesystem sandbox exists in any mode. A prompt-injected `rm -rf` in root
  mode is as dangerous as it sounds. Mitigations to consider later: a
  confirm-in-pane step for destructive root commands, per-project trust, or
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

## Roadmap (not built yet)

- session listing/branching methods for the pane (data model already supports it)
- compaction for long conversations
- MiniMax speech (STT/TTS) — belongs to the plasmoid, not this daemon
- "claw-like" desktop automation duties (window/clipboard/notification tools)
