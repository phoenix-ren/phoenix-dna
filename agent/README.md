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
                                        tools: bash / read / write / edit / ls
                                               tavily_search / exa_search
```

## Operating modes

| Mode | Auth | Working dir | Tools | Purpose |
|---|---|---|---|---|
| `root` | KDE password dialog (`kdialog`), cached 15 min | `/` | bash (via `sudo`), read, write, edit, ls, tavily_search, exa_search | system administration; one password covers multiple prompts |
| `user` | none | `~` | bash, read, write, edit, ls, tavily_search, exa_search | everyday desktop tasks and questions that need a web lookup |

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
| `GetModes` | ` → a(sss)` | `(name, description, working_dir)` for root/user. |
| `GetState` | ` → (ssu)` | `(state, current_run_id, queue_length)`; state is `idle` or `running`. |
| `SearchSessions` | `ss → s` | `(user, query)` → JSON array of the user's sessions whose summary name matches the query. The match is fuzzy — every character of the query must occur in the name, in order — so typing narrows the list down; an empty query lists every session, newest first. Each entry carries `id`, `mode`, `title`, `summary` (the name the session is listed under: the title, or the first query when the session was never named), `preview`, `createdAt`, `updatedAt` and `messageCount`. |
| `GetSessionTranscript` | `ss → s` | `(user, session_id)` → JSON array of the session's messages: `role` (`user` or `assistant`), `content` and `ts`. |
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
| `tavily_api_key` | `TAVILY_API_KEY` | web search in every mode (tool omitted if unset) |
| `exa_api_key` | `EXA_API_KEY` | web search in every mode (tool omitted if unset) |
| `stt_provider`, `openai_api_key` | `PHOENIX_STT_PROVIDER`, `OPENAI_API_KEY` | the plasmoid's speech-to-text: provider (`minimax` default) and its key — read by the widget, not the daemon |
| `model`, `llm_base_url`, `max_turns`, `credential_ttl_seconds`, bash timeouts … | `PHOENIX_MODEL`, `PHOENIX_BASE_URL`, … | see `config-example.toml` |

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
