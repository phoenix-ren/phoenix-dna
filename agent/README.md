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
`../design_docs/CODING_AGENT_ANALYSIS.md`), wrapped in a daemon.

```
plasmoid (C++/QML)  ──D-Bus──▶  phoenix-dna-agent (this daemon)
   speech, UI                       AgentDaemon ──▶ AgentLoop ──▶ MiniMax LLM
                                    (queue, sudo cache)   │
                                                          ▼
                                        tools: bash / read / write / edit / ls / ask_user
                                               transfer / tavily_search / exa_search
                                               tavily_search / exa_search
```

## The working context

The agent works as the desktop user, in their home directory (`~`). The
tools are the filesystem quartet (read, write, edit, ls), the web search
pair (tavily_search, exa_search — registered whenever their API keys are
configured, and the prompt then tells the model to reach for them on
anything that depends on current or specific facts, so it never answers
"I cannot search the web"), the transfer tool, the question tool
(ask_user) and bash.

The transfer tool (see `phoenix_dna/tools/transfer.py`) is the metered
mover of bytes: downloads from http/https/ftp URLs and torrent `magnet:`
links, uploads of a local file to an http(s) URL or onto an FTP server.
It exists because a `wget` through bash times out on anything slower
than the bash cap, and the workaround the model reaches for — a
detached command — is invisible to the UI; the tool instead reports the
bytes as they move (TransferProgress), and the pane meters the pace and
the share of the thing that is here. Torrents need libtorrent
(`python3-libtorrent`) and say so clearly when it is missing.

Commands run without a terminal and stdin is closed, so the prompt (and the
bash tool's description) demand non-interactive flags (`dnf -y`,
`apt-get -y`, `flatpak install -y`); when a command still fails from asking
a question on stdin, the bash tool appends a hint so the next attempt fixes
itself.

### Reading the answers aloud

The daemon can speak its own answers (opt-in, off by default): the answer
is normalized for the ear — math is spoken as mathematics (`$\pi r^2$`
becomes "pi r squared", tied to the renderer's own span detector by a
shared test fixture), code fences and URLs are not read out — then
synthesized sentence by sentence and played through a PipeWire sink.

Playback can be interrupted at any moment: the stop control next to the
record button, closing the results window (or pressing `Esc` in it), or
pressing record itself. The results window also has a per-answer "Read
aloud" button.

The settings window (the panel's gear button → **Read aloud**) owns the
on/off switch, the read mode, the voice and the **playback speed**:

| Setting | Where | Meaning |
|---|---|---|
| `tts_enabled` | keys.json | whether the answers are spoken at all |
| `tts_read_mode` | keys.json | `final` (only the finished answer) or `stream` (as it is generated) |
| `tts_voice` | keys.json | the voice id the service synthesizes with |
| `tts_playback_speed` | keys.json | how fast the audio is **played back** |

The playback speed is a time-stretch, not a resampling: the words come
out faster or slower and the **pitch stays** — 1.5× is the same voice in
half less time, not a higher voice. It is applied by ffmpeg's `atempo`
filter in the playback chain (`speech/player.py`), so it works for every
engine and needs no re-synthesis. Every setting applies at once: the
window asks the daemon to rebuild its speech pipeline over D-Bus
(`ReloadSpeech`), so no service restart is needed.

### Administrator rights, on demand

A command that needs administrator rights is written with `sudo`:

```
sudo dnf install -y btop
```

The daemon sees the `sudo` call, asks the user for their password with the
KDE password dialog (`kdialog`) and runs the command as root. The password
is cached for 15 minutes (like sudo's own timestamp cache), so a session
that needs privileges only asks once; it lives in memory only, is validated
immediately with `sudo -S -k -v`, and three failed attempts give up.

A destructive command (`rm -rf`, `dd` to a device, a filesystem format, a
package removal, ...) that escalates is confirmed by the user first — an
Allow/Deny card that the harness owns, not the model (see
[confirmations](#harness-confirmations)).

## Asking the user (`ask_user`)

Sometimes the right action depends on a decision only the user can make. The
`ask_user` tool shows the user one multiple-choice question and
**blocks the run until it is answered** — the same human-in-the-loop pattern
OpenCode's `question` and Claude Code's `AskUserQuestion` tools use, so
models are already trained on its shape:

1. the model calls `ask_user` with **every question it already knows it
   needs** — 1-6 questions, each with 2-6 short options (with optional
   descriptions) and a `multiple` flag — plus an optional `goal`;
2. the daemon emits `QuestionAsked(run_id, ask_id, question_json)` with
   the whole form and the tool waits — exactly like a slow bash command
   would;
3. the frontend walks the user through the form (the results window's
   wizard: one question at a time, instant back, exact progress — all
   client-side, since the whole form arrived at once; the REPL prints
   the questions one by one, `b` goes back, a blank line skips) and
   calls `AnswerQuestion(ask_id, answer_json)` **once**;
4. the tool result tells the model what was chosen per question, plus
   anything the user wrote in their own words. Free text is passed
   through verbatim, so an answer can also be dictated — "the second
   one" needs no extra parsing, the model interprets it.

`answer_json` is `{"answers": [...]}` with one entry per question, in
order; each entry is `{"selected": [...], "text": "..."}` where
`selected` entries are 0-based option indices or exact labels, either
part may carry the answer alone, and an **empty entry is a deliberate
skip** (the model is told there was no answer, not asked again). A bare
JSON string counts as free text on the first question. Answers that
cannot be used (unknown `ask_id`, wrong answer count, index out of
range, unknown label, several picks on a single-choice question) return
`False` — the form keeps waiting and the caller may retry. Follow-up
questions — asked only when an answer opens a decision the model could
not have written in advance — are just another (small) form.

An unanswered question releases the run after `ask_timeout_seconds`
(default 300, `0` waits forever): the model is told the user may be away
and asked to proceed on its best judgment. Cancelling the run releases it
too. The question and its answer are recorded in the session transcript
like any tool call.

**Harness confirmations.** The questions above are *model-authored* —
fine for clarifications, wrong for consent. When a bash command that
escalates to root matches the destructive screen (`rm -rf`, `mkfs`,
`dd of=/dev/…`, package removal, unit masking, …; extendable via
`confirm_patterns`), the daemon itself asks a question with the `harness`
marker: the command and the reason it matched are shown, the options are
fixed (`Allow` / `Deny`) and no model output can change them. The results
window badges it as a *system confirmation* (warning tint), and it is
**fail-closed**: a Deny, a timeout or a cancellation means the command
does not run — the model is told it was not run and must not retry it
unchanged. Set `confirm_destructive = false` in `agent.toml` to disable
the gate (`dnf remove` and friends then run as before, on the
non-interactive flags alone).

**The wizard overlay:** a form of several questions opens a large,
transient, centred overlay of its own (themed like the session search:
near-black in the dark scheme, light in the light one) — not a pane in
the results window. One question at a time, exact progress (the form
arrived whole, so the bar stands full on the last question), a **back
button** that is instant because everything is client-side, a
final-question gate (Next stays disabled until the question is answered;
the own-words choice, a radio option of its own that picks itself when
you type, counts), and a "Submit answers" that hands every answer over
**optimistically**: the overlay closes and the conversation returns at
once, with the answers summarized in the run's tool log. A rejected
payload restores the form **with every answer kept** — only a genuinely
new form starts the walker over. Esc or the overlay's close button
dismisses it; the run keeps waiting, and the results window shows a card
that brings the questions back. The pane's input bar answers only a form
of one question; while a wizard is pending its text goes on as a prompt
instead (a wizard's answers leave as one submission or not at all — the
daemon refuses a mismatched answer count, and so does the widget before
anything is sent).

The results window renders a pending question as a card above its input
bar: one radio button per option (checkboxes when `multiple` is set), a
field for the user's own words, and the wizard progress line when the
model is asking a series. The window opens on the question by itself —
a question is activity like any other — and the card gives way when the
question is answered, times out or its run ends. Answering routes through
`AnswerQuestion` as option indices plus any typed text; the Q&A also
shows in the run's trail. The REPL prints the same question with
numbered options and answers by number, label or free words.

## Sessions and forks

Every conversation is a **session**: a row in the user's own SQLite
history (`<data_dir>/users/<user>/sessions.db`), holding the queries and
the answers, the tool calls in between (for a faithful replay into the
model) and the audit trail of the runs. The session search lists them;
the results window continues one.

A session can be **forked** at one of the user's queries: `ForkSession`
copies the conversation up to that query — the query itself and the
answer that followed it — into a *new* session, and the session it was
forked from keeps everything it had, including what came after. The two
grow apart from there.

The fork is a session of its own: it has its own id, its own name (the
name of the session it came from, marked as a fork, until the model names
it after what was said in it) and its own place in the history. The
follow-ups typed after the fork continue the fork, not the conversation it
came from. A session knows the one it was forked from (`parent_id`) and
the query it was forked at (`fork_index`), so the branches can be shown
as the branches they are.

## D-Bus API

Bus name `dna.phoenix.Agent`, object path `/dna/phoenix/Agent`,
interface `dna.phoenix.Agent`.

### Methods

| Method | In → Out | Description |
|---|---|---|
| `SubmitPrompt` | `ss → s` | `(prompt, session_id)` → `run_id`. Empty `session_id` starts a new session; pass a previous `session_id` (from `RunStarted`) to continue it. Prompts submitted while busy are queued and run in order. |
| `SubmitPromptWithAttachments` | `sss → s` | `(prompt, session_id, attachments)` → `run_id`. The same, plus the files the user attached to the prompt: a JSON array of `{"name", "path", "mimeType"}` objects. The daemon copies them into `<data_dir>/attachments/<run_id>/`, inlines the images when the model can see them and extracts the text of PDFs — see `phoenix_dna/attachments.py`. |
| `Cancel` | `s → b` | Cancel a running prompt (kills its process group) or remove a queued one. |
| `AnswerQuestion` | `ss → b` | `(ask_id, answer_json)` → whether the pending form was answered. `answer_json` is `{"answers": [{"selected": [0-based index or label, …], "text": "…"}, …]}`, one entry per question — see [Asking the user](#asking-the-user-ask_user). |
| `GetState` | ` → (ssu)` | `(state, current_run_id, queue_length)`; state is `idle` or `running`. |
| `SearchSessions` | `ss → s` | `(user, query)` → JSON array of the user's sessions whose summary name matches the query. The match is fuzzy — every character of the query must occur in the name, in order — so typing narrows the list down; an empty query lists every session, newest first. Each entry carries `id`, `mode` (the session's mode — `user` today, `root` for the sessions of the older daemon), `title`, `summary` (the name the session is listed under: the title, or the first query when the session was never named), `preview`, `createdAt`, `updatedAt` and `messageCount`. |
| `GetSessionTranscript` | `ss → s` | `(user, session_id)` → JSON array of the session's messages: `role` (`user` or `assistant`), `content` and `ts`. |
| `RenameSession` | `sss → s` | `(user, session_id, title)` → the session's entry as the search lists it now (the same shape a search answer carries), or the empty string when there was no such session. The name the user gives stands: the model's own name — which is rewritten after every completed run — never overwrites it. An empty title takes the user's name away, and the session is listed under its first query again. |
| `ForkSession` | `ssi → s` | `(user, session_id, exchange_index)` → the fork's entry as the search lists it (the same shape a search answer carries), or the empty string when there was no such session or no such query. `exchange_index` counts the user's queries: 1 is the first thing the user asked in the session, 2 the next one. The fork holds the conversation up to that query — the query itself and the answer that followed it — and what came after stays in the session it was forked from. The fork is a session of its own: it has its own id and name, and the follow-ups typed after it continue it. Its entry carries `parentId` (the session it was forked from) and `forkIndex` (the query it was forked at). |
| `DeleteSession` | `ss → b` | `(user, session_id)` → whether there was a session to delete. The session's messages and the audit trail of its runs go with it: what is left behind would be an orphan nobody ever sees again. A session of another user is not touched. |
| `GetSessionContext` | `ss → s` | `(user, session_id)` → JSON object: what the session spent of the model's context window. It carries the session's `title`, `createdAt` and `updatedAt`, how many `userMessages`, `assistantMessages` and `messageCount` it holds, the tokens its runs spent (`tokensIn` — the fresh input, `tokensOut`, `cacheTokens` and `reasoningTokens` — and `totalTokens`), the model in force (`provider`, `model`), the size of its context window (`contextLimit`), the share of that window the session has used (`usage`, in per cent) and what the tokens cost (`cost`, and `priced` — false when the model has no price). The tokens come from the audit trail of the session's runs, so a session opened from the history reports the same numbers as the one that is running. An empty object means there is no such session. |
| `SpeakText` | `s → ` | Speak an arbitrary string on demand (the results pane's "Read aloud"). The text is normalized and segmented exactly like an answer — math is spoken as mathematics, code fences are skipped. A no-op when speech is not enabled. |
| `StopSpeaking` | ` → ` | Interrupt playback immediately and drop what is queued. |
| `GetSpeechState` | ` → bs` | `(enabled, state)`; state is `idle` or `speaking`. |
| `ReloadSpeech` | ` → ` | Re-read the configuration (keys.json's `tts_*` entries included) and rebuild the speech pipeline. What the settings window calls after saving, so changes apply without a service restart. |
| `Ping` | ` → s` | Version string. |

### Signals

| Signal | Signature | Emitted when |
|---|---|---|
| `RunStarted` | `ss` | run_id, session_id |
| `TextDelta` | `ss` | streamed answer text (concatenate for partial replies) |
| `ToolCallStarted` | `ssss` | run_id, call_id, tool name, JSON args |
| `ToolCallFinished` | `sssss` | run_id, call_id, tool, `success|error`, output text |
| `ToolProcessStarted` | `sssu` | run_id, call_id, command, pid — a bash tool call's process began. The pid is the command's session leader (commands are spawned with `start_new_session`), so it doubles as the process-group id: the UI meters the command's CPU usage by reading `/proc` for that group while the call runs. |
| `ToolProcessFinished` | `ssu` | run_id, call_id, pid — that process ended (normally, by timeout or by cancellation; the report is emitted from a `finally` block, so it is reliable) |
| `WireBytes` | `suuu` | run_id, turn, bytes_sent, bytes_received — the wire traffic of one turn of the conversation with the model: the bytes of the request that went out and the bytes of the answer that came in. The counts are the payload bytes only (the fixed-size HTTP headers are not included), and the UI meters the rate of the connection from them. |
| `TransferProgress` | `ssstt` | run_id, call_id, direction (`download\|upload`), done, total — the bytes a transfer tool call has moved so far. `done` is cumulative for the call and `total` the bytes the far end promised (0 when it did not name them); the counts are 64-bit because an ISO outruns a 32-bit count. Emitted at most a few times a second while the transfer runs, and once more at its end; the UI meters the pace and the share of the thing that is here from these (see the transfer tool in `phoenix_dna/tools/transfer.py`). |
| `AuthRequested` / `AuthFinished` | `ss` / `sb` | the administrator password dialog opened / closed |
| `QuestionAsked` | `sss` | run_id, ask_id, JSON of the form: `questions` (`[{question, header, options: [{label, description}], multiple}]`), an optional `goal`, and `harness: true` for daemon-authored confirmations. The run blocks until answered, cancelled or timed out. |
| `QuestionAnswered` | `ssss` | run_id, ask_id, status (`answered\|timeout\|cancelled`), JSON of the normalized answers (`{"answers": [{"selected": [labels], "text": "…"}, …]}`) |
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
| `llm_base_url`, `model` | `PHOENIX_BASE_URL`, `PHOENIX_MODEL` | the endpoint the model calls go to and the model they name. The host in the URL decides the service: a known one brings its own default model and key field, any other is the user's own server |
| `context_window` | `PHOENIX_CONTEXT_WINDOW` | how many tokens the model holds at once — its context window. The provider knows the size of the models it serves (a million for MiniMax's, 128 000 for OpenAI's and Groq's); this is for a server of one's own, and for another model of a known service. The context view (the ring in the corner of the results window) measures the conversation against it |
| `price_input`, `price_output` | `PHOENIX_PRICE_INPUT`, `PHOENIX_PRICE_OUTPUT` | what a million tokens cost, in US dollars. Without them the context view says that the model is not priced, rather than quoting a price of nothing |
| `minimax_api_key` | `MINIMAX_API_KEY` | the MiniMax key: the LLM by default, and the speech-to-text and speech synthesis when those use MiniMax |
| `openai_api_key` | `OPENAI_API_KEY` | the OpenAI key: the LLM when the endpoint is OpenAI's, and the speech when the endpoint is |
| `groq_api_key` | `GROQ_API_KEY` | the Groq key, in the same way |
| `openrouter_api_key` | `OPENROUTER_API_KEY` | the OpenRouter key, in the same way |
| `llm_api_key` | `PHOENIX_LLM_API_KEY` | a key of one's own, for a gateway or a self-hosted server — beats the provider's key |
| `tavily_api_key` | `TAVILY_API_KEY` | web search (tool omitted if unset) |
| `exa_api_key` | `EXA_API_KEY` | web search (tool omitted if unset) |
| `stt_endpoint`, `stt_model` | `PHOENIX_STT_ENDPOINT`, `PHOENIX_STT_MODEL` | the endpoint the dictation is transcribed at and the model it names — read by the widget, not the daemon |
| `stt_api_key` | `PHOENIX_STT_API_KEY` | the key of a speech-to-text endpoint of one's own |
| `max_turns`, `credential_ttl_seconds`, bash timeouts … | — | see `config-example.toml` |
| `tts_enabled`, `tts_voice`, `tts_read_mode` … | `PHOENIX_TTS_ENABLED`, … | the read-aloud settings (see *Reading the answers aloud*) |
| `tts_endpoint`, `tts_model` | `PHOENIX_TTS_ENDPOINT`, … | the endpoint the voice is made at and the model it names |
| `tts_provider` | `PHOENIX_TTS_PROVIDER` | which engine speaks: `minimax` (default), `openai`, `groq`, `openrouter` or `local`. Only MiniMax and the on-device one need naming — the rest follow the endpoint |
| `tts_playback_speed` | `PHOENIX_TTS_PLAYBACK_SPEED` | how fast the audio is played back: 1.0 is the speed it was spoken at, 1.5 half again as fast. The stretch keeps the pitch, so the voice stays the voice (default 1.0) |
| `ask_timeout_seconds` | — | how long an unanswered `ask_user` question may block the run before the model proceeds on its own (default 300; `0` waits forever) |
| `confirm_destructive` | — | destructive commands that escalate to root (screen in `phoenix_dna/confirm.py`) block on a harness-owned Allow/Deny, fail closed (default true) |
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

A system install (`sudo cmake --install build`) installs the same template
filled in for the package, together with the `phoenix-dna-agent` launcher and
a D-Bus activation file. The plasmoid's first call then starts the daemon even
if the user unit has not been enabled; the checkout installer above remains
the way to run the agent from a working tree without installing it. See
[`../PACKAGING.md`](../PACKAGING.md) for the packaging switches.

**Why the user manager and not a root system service:**

- The daemon owns a name on the **session bus** — that bus only exists
  inside the user's session, so a system unit would have no bus to join.
- A command that needs administrator rights elevates *on demand*: the
  daemon asks for the password with `kdialog` (which needs the GUI
  session: `DISPLAY`/`WAYLAND_DISPLAY`) and runs the command through
  `sudo`. As root, `sudo` would succeed silently and the password prompt
  would be theatre — the daemon would already have all the power it asks
  for.
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
./.venv/bin/python -m phoenix_dna.repl --provider fake  # no API key needed
./.venv/bin/python -m phoenix_dna.repl --dbus           # drive a running daemon via D-Bus
```

Commands: `/new` (fresh session), `/session`, `/state`, `/help`, `/quit`.
Plain text is submitted as a prompt; follow-ups continue the same session;
**Ctrl+C while a run is active cancels it**; a command that calls `sudo`
pops the real kdialog password prompt (cached for 15 min).
When the model asks, every question of the form is walked one by one —
answer with a number (or several, comma-separated), a label, or your own
words; `b` goes back to the previous question, a blank line skips it:

```
phoenix> back up my disk
  ❓ backup setup
  ❓ (1 of 2) Which disk should the backup use?
     1) /dev/sda — the SSD
     2) /dev/sdb — the spinning one
answer> 2
[2/2] Encrypt the backup?
   1) Yes
   2) No
answer> b
answer> 1
  ✓ The user answered your questions:
Backing up to /dev/sdb.
```

**Readline** (GNU readline; macOS uses libedit, Windows would need
`pyreadline3`): `↑`/`↓` recalls prompts and history persists between sessions
in `<data_dir>/repl_history` (`--history FILE` to override, `--no-history` to
disable; blank lines and consecutive duplicates are not recorded). **TAB**
completes slash commands (`/se<TAB>` → `/session`) and file paths — including
`~` expansion and a trailing `/` on directories (`read ~/Doc<TAB>`).

Live example (real MiniMax):

```
phoenix> How many days until the 5th of November next year? ...
── run run_f688b923bdb3 · session c3e3e33918ea
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

- A command that calls `sudo` runs **arbitrary commands as root** once the
  user types the password — same trust model as a terminal. The password is
  kept in memory only (never logged/written), validated immediately via
  `sudo -S -k -v`, and re-used for 15 minutes (configurable) so a session
  asks once.
- Destructive commands that escalate to root (recursive forced deletion,
  filesystem formats, raw device writes, package removal, unit masking…)
  are gated on a **harness-owned Allow/Deny** — the daemon asks, the options
  are fixed, and it fails closed on timeout (see *Asking the user*). The
  screen is a conservative pattern list, not a sandbox: a destructive shape
  it does not know runs without asking (`confirm_patterns` extends it).
- No filesystem sandbox exists. A prompt-injected `rm -rf` is as dangerous
  as it sounds — the confirmation gate stops the known-destructive shapes
  that escalate to root, but mitigations still to consider: a
  confirm-in-pane step for *every* root write, per-project trust, or
  running the daemon in a `bubbles`/podman sandbox.
- Commands run in their own process group so cancellation and timeouts kill
  the whole tree; bash output is truncated to `max_output_chars`. Commands
  that fail because they asked for confirmation on closed stdin get a hint
  appended, nudging the model to retry with a non-interactive flag.

## Design decisions carried over from the pi research

- Four filesystem/shell tools with pi/Claude-Code-shaped schemas (models are
  RL-trained on these shapes).
- Minimal system prompt (a context block + current date, well under 1000
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

- a session tree in the pane: the forks are recorded (a session knows the
  one it was forked from, and the query it was forked at), so the branches
  can be shown as the branches they are
- compaction for long conversations
- MiniMax speech (STT/TTS) — belongs to the plasmoid, not this daemon
- "claw-like" desktop automation duties (window/clipboard/notification tools)
