<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA

![Phoenix DNA](icons/phoenix-dna.svg)

Do you suffer from advanced AI psychosis? Then this tool is for *you*!
Phoenix DNA is the world's first **desktop-native agent**; an AI agent 
specifically tailored for desktop use on Linux/KDE. 

In other agents, getting
past the `sudo` (I'm looking at you, [OpenCode](https://opencode.ai])) interactive 
prompt can an exercise in frustration -- and that's because virtually every 
AI agent on the market is basically lowest-common denominator, (sort of) runs-anywhere 
*web shite*. 

By contrast, Phoenix DNA is hyperfocused on desktop use:
- **easy access**: a widget on your window manager's task panel; simple.
- **text-to-speech output**: for when you're afk
- **authentication built in**: using a native KDE PAM authentication, so your DNA can `apt install` for you
- **streamlined screenshot grabbing tool**: your queries often relate to a visible window, so make it easy to grab a screenshot and write a prompt directed at it
- **hyperlinks**: to quickly open files, URLs, email addresses, phone numbers, etc. outputted during LLM responses
- **tool CPU usage monitoring**: to confirm the tool that's been running for five minutes isn't dead
- **wire rate monitoring**: the pace of the conversation with the model in bytes per second, in and out
- **context metering**: a ring in the corner of the conversation window says how much of the model's context window the session has used — the numbers behind that share one click away
- **forks**: a conversation can be branched into a session of its own at any of your own prompts, so another way through the problem can be tried without losing the first one
- **metered transfers**: the agent's transfer tool downloads (web, FTP, even torrent magnets) and uploads with live progress — the pace of the bytes and how much of the thing is here
- **session tending**: name a session yourself — the name stands — and throw away the ones in the way, with a confirmation that names them
- **back-to-the-future UX and performance**: because in a capable programmer's hands, native is faster, harder, stronger, better
- **other agents allowed!**: a dictated prompt is automatically copied to the clipboard for easy pasting into prompt boxes of lesser AI agents

Made with 🏳️‍⚧️ 💜 and attitude by Phoenix Ren aka Michelle Percossi, and her trusty GLM and MiniMax AIs.

## Layout

| Path         | Contents                                                        |
|--------------|-----------------------------------------------------------------|
| `shell/`     | Panel widget: the main plasmoid, audio capture, transcription   |
| `overlay/`   | Dictation overlay shown while recording                         |
| `resultswindow/` | Fully-fledged results window for conversation output        |
| `search/`    | Session search: the previous sessions, listed by name           |
| `settingswindow/` | Settings window: appearance, API keys and global shortcuts |
| `context/`   | The context view: the ring in the results window's corner and the overlay it opens |
| `theme/`     | Shared colour-scheme singleton, the app's icon set and Markdown presentation helpers |
| `waveform/`  | Offline `.wav` → `.png` waveform renderer                       |
| `widgettest/`| Developer testbench that hosts the widget in a plain window      |
| `agent/`     | Python agent daemon (`phoenix-dna-agent`), a D-Bus service          |
| `icons/`     | The artwork: the mark the widget picker shows, the avatar, and the app's own icon set (Lucide's drawings) |

## Prompts with files

A prompt is not only text: the user can attach files to it. The "+" button
inside the prompt field (in the dictation overlay and in the results
pane's follow-up field) opens a file picker, and every picked file shows
up as a chip next to the prompt. The files travel to the agent daemon with
the prompt and are copied into its own attachment directory, where the
agent can read them; images are also shown to the model directly, and the
text of a PDF is extracted for it.

The camera button does the same with a picture of the desktop: it starts
the window picker, which is also bound to the global shortcut `Meta+Alt`
(rebindable in *System Settings → Shortcuts → Phoenix DNA*). KWin takes
the pointer over and shows its own hint; the window under the pointer is
named in the status line of the panel, and the window that is clicked is
photographed — the click never reaches the window itself. The picture is
attached to the prompt that is being composed, like any other file, and
named after the window it shows.

Both need KWin, which is where the pointer is followed, the title of the
target window is looked up and the picture is taken.

## The addresses in the text

Everything the agent says names things: files and folders on the
machine, pages on the web, mailboxes, phone numbers. In the results
window and in the session search every such address is a link, and
what a click does is the desktop's business:

* a folder opens in the file manager — Dolphin — and a file opens in
  it too, selected in the folder it stands in: where a file *is* is
  the useful thing to show, the point of the link is the place as
  much as the bytes;
* a page opens in the browser, an address in the mail client, a
  number in the dialer;
* the Markdown's own links do the same.

The tool log counts: the commands and their output name files more
than anything else does, and they open like the answer's own links.

The detection is the window's own — it runs on the rendered text,
after the Markdown import, so the punctuation around an address is
known to be decoration and not part of it, and the text itself is
never rewritten, only marked. The daemon is untouched by it.

The detector is careful about what an address is: a path must begin
with one of the system's roots (or with `~`), a number must look like
one — a date, a version number and an IP address are not phone
numbers — and a bare name without a place (`src/main.cpp`) is left
alone, since there is no telling where it stands. Only what the
desktop has a handler for is ever handed to it: a link the Markdown
authored with some other scheme is refused.

## Forking a conversation

Sometimes the agent's answer is a dead end, and the way out is the same
question asked differently — or asked earlier, before the conversation
went off the rails. The results window can be **forked**: the little
branch button beside one of your own prompts branches the conversation
into a new session at that point.

The fork holds the conversation up to that prompt — the prompt itself
and the answer that followed it — and what came after stays where it
was. The session it was forked from is not touched: the two grow apart,
and the original keeps its own history.

The fork is a session of its own, so the follow-ups typed after it
continue the fork, not the conversation it came from. The window says
so, and the fork is listed in the session search under its own name —
the name of the session it was forked from, marked as a fork, until the
model names it after what was said in it.

## Settings

The panel's gear button opens the settings window, which configures:

* **Appearance** — the colour scheme of the app's windows: follow the
  system (the default), or force light or dark. The choice is stored as
  `ui_theme` in `~/.config/phoenix-dna/keys.json` and applies to the
  conversation window, the session search, the settings window and the
  dictation overlay the moment it is made; the panel widget itself keeps
  matching the panel it lives on.

  The same section holds the **font of the agent's answers**, one family
  and one size per mode — the light mode is meant to be read like a news
  article, the dark one like a screen that never sleeps. The two are
  chosen independently of each other and of the colour scheme; the
  scheme decides which of the two applies. They are stored as
  `ui_font_light` and `ui_font_dark` (and `ui_font_size_light`,
  `ui_font_size_dark`) in the same file, and an empty choice means the
  system's own font and size. Every font the system has is on offer,
  and a line of sample text below each list shows the face as the
  answers would be set in it. Only the agent's own words — the answers
  and the notes it thinks in — are set in them; the chrome around them
  (labels, buttons, tooltips) always keeps the system's font.

* **API keys** — the LLM key and the speech-to-text key, each with its
  provider chosen alongside it (MiniMax, OpenAI, Groq, or Custom for any
  other OpenAI-compatible server). Both are stored in
  `~/.config/phoenix-dna/keys.json` (mode 0600), the file the daemon and
  the transcriber already read. A changed speech-to-text key applies to
  the next dictation; the daemon reads its key at startup, so a changed
  LLM key applies at once — the shell restarts phoenix.service for you.
  Environment variables still take precedence over the file, and the
  window says so when it sees one set.

  The **Read aloud** section has the same choice for the voice: MiniMax,
  OpenAI, Groq, or the on-device engine (Local), which needs no key at
  all. Switching it applies at once — the shell asks the daemon to
  rebuild its speech pipeline.

* **Global shortcuts** — the recording, screenshot, session-search and
  typed-prompt hotkeys, captured with the keyboard and bound through
  KGlobalAccel. The bindings are the very ones System Settings → Shortcuts
  edits, so they survive restarts and can still be re-bound there; the
  window's rows always show the live bindings.

## Building

```bash
sudo cmake --install build
systemctl --user restart plasma-plasmashell   # otherwise the panel keeps the old plugin
journalctl --user -b --no-pager | grep -i phoenix   # check for QML errors
```

A CMake install installs both halves: the plasmoid and the Python agent
daemon, its `phoenix-dna-agent` launcher, the `phoenix.service` systemd user
unit and the D-Bus activation file. `-DPHOENIX_INSTALL_AGENT=OFF` leaves the
Python half to the distribution's own packaging; see [PACKAGING.md](PACKAGING.md)
for the options and the per-distribution recipes.

The screenshot path can be checked on its own, without the panel:

```bash
./build/bin/screenshottest /tmp/capture.png   # the picture of the active window
```

It asks KWin for a picture over D-Bus, reads it out of the pipe the way
the picker does and writes it to a PNG. It also says whether the picture
was already complete when KWin answered the call - it never is, and a
picture that is looked at before it is complete sends the decoder past
the end of the buffer.

## Running the agent

The Python agent daemon runs as a systemd **user** service, so it comes up
with the session — nothing has to be started by hand:

```bash
systemctl --user status phoenix.service
journalctl --user -u phoenix.service -f
```

A system install (`sudo cmake --install build`) puts `phoenix.service` in the
user unit directory and adds a D-Bus activation file, so the plasmoid's first
call starts the daemon even if the unit has not been enabled. In a development
checkout, `agent/systemd/install-user-service.sh` writes the same unit to
`~/.config/systemd/user/phoenix.service` with `ExecStart` and
`WorkingDirectory` pointing at the checkout; run it once per checkout.

It runs as the desktop user: it lives on the session bus that
the plasmoid connects to, and it elevates to root on demand — a command
that calls `sudo` makes the daemon ask for the password (KDE password
dialog) and run it as root, with a confirmation card (Allow/Deny) for
destructive commands, answered by you before they run. See `agent/README.md`
for the details.

## Choosing the model

The agent talks to whichever OpenAI-compatible endpoint you name, and
**the settings window is where you name it** (the panel's gear button →
**API keys**). An endpoint is all it takes:

```
Endpoint:  https://openrouter.ai/api/v1
Model:     anthropic/claude-sonnet-4.5
```

The host in the URL says which service it is, so the endpoint brings
the rest with it — the default model, and the key field the window
shows. A URL that is none of the known services is your own server: it
gets no model from anywhere (name one, or the daemon says so), and its
key is the dedicated `llm_api_key` entry.

| Endpoint | Default model | Key |
| --- | --- | --- |
| `https://api.minimax.io/v1` | `MiniMax-Text-01` | `minimax_api_key` |
| `https://api.openai.com/v1` | `gpt-4o-mini` | `openai_api_key` |
| `https://api.groq.com/openai/v1` | `openai/gpt-oss-120b` | `groq_api_key` |
| `https://openrouter.ai/api/v1` | `anthropic/claude-sonnet-4.5` | `openrouter_api_key` |
| anything else | — you name it | `llm_api_key` |

The same holds for the dictation and the voice: each has an endpoint
field in the window, and the host in the URL decides the service.

Groq's models are named by their own family — `openai/gpt-oss-120b`,
`qwen/qwen3.8-27b`, `meta-llama/…` — and which of them an account may
use is the account's own business; the ones yours has are listed at
<https://console.groq.com/keys>. OpenRouter carries hundreds of models,
each named `vendor/name`, listed at <https://openrouter.ai/models>; the
free ones are marked `:free`.

Switching endpoint does not leave the old one's model behind: a model
that belongs to another service follows the new one (OpenAI would
refuse "MiniMax-M3"). A model you named yourself — for your own server
— is left alone.

`vision` is a separate question and is *not* switched with the
endpoint: `vision = true` in `agent.toml` states it outright, so an
endpoint that lands on a text-only model (Llama, for instance) will be
sent images it cannot read. Use `vision = "auto"` to let the model name
decide.

`agent.toml` overrides the window, so it is the right place for what
the window does not offer:

```toml
[agent]
llm_base_url = "https://my.gateway/v1"
model = "qwen3-32b"
```

The keys live in `~/.config/phoenix-dna/keys.json`, or in the
environment (`OPENAI_API_KEY`, `GROQ_API_KEY`, `OPENROUTER_API_KEY`,
`MINIMAX_API_KEY`, `PHOENIX_LLM_API_KEY`); `PHOENIX_BASE_URL` and
`PHOENIX_MODEL` name the endpoint and the model from the environment.
The daemon reads all of this when it starts. Saving the LLM key in the
settings window restarts the daemon for you; a changed endpoint or model
applies after `systemctl --user restart phoenix.service`.

Models that reason out loud (MiniMax's `<think>` blocks) are handled the
same way whichever service serves them — the reasoning is shown
separately and never read aloud.

## The context window

The corner of the results window carries a ring: how much of the model's
context window the session on screen takes up. A click on it opens a
window with the numbers behind it — which model is in force, how much it
holds at once, how full it is, the tokens the session spent (the fresh
input, the output, the part of the prompt the server answered from its
cache, and the reasoning), how many queries and answers the session
holds, what it cost, and when it began. `Esc` — or the **Close** button —
puts it away.

The ring belongs to the session, so it lives with the conversation rather
than in the panel widget: the widget is the app's presence, not any one
conversation's.

Two numbers are worth telling apart, and the view tells them apart:

* **How full the window is** — the size of the biggest prompt the
  session sent. This is what the ring shows, and it is what decides
  whether the next prompt still fits.
* **What the session spent** — the sum of every request's prompt and
  answer. The conversation is sent again with every turn, so the same
  tokens are counted again and again: a session of a few dozen requests
  spends millions of tokens while the window is barely a tenth full.
  That is normal, and it is why most of it is *cache*: the server
  answers from its prompt cache, which is cheap and quick.

How much the model holds at once is the provider's own number (a million
tokens for MiniMax's models, 128 000 for OpenAI's and Groq's), and the
settings window has a **Context window** field for the cases the provider
does not know: your own server, or another model of a known service.
`context_window` in `agent.toml` does the same:

```toml
[agent]
context_window = 200000
```

What the tokens cost is the user's own number as well — the daemon does
not keep a price list. Name the two prices (in US dollars per million
tokens) and the window quotes the cost of the session:

```toml
[agent]
price_input = 0.30
price_output = 1.20
```

Without them the window says that the model is not priced, rather than
quoting a price of nothing.

The conversation grows with every turn and is never shortened, so a
session that goes on long enough will fill the window whatever the
model's limit is. Three things keep that in hand:

* **A tool's output is cut** to what the conversation may carry (the
  head and the tail of it, the middle dropped): the conversation is
  sent again with every turn, so what a tool brings back is paid for
  over and over.
* **A run that comes near the window answers.** When the turns are
  spent, or the prompt fills the window, the agent is asked to stop
  working and say what it has — with no tool within reach. The work
  done so far is an answer, not a run that ends in nothing.
* **A conversation that outgrows the window is summarised.** When it
  passes `compact_at` (half the window by default), the older turns
  go into a summary and the agent goes on with that; the pane says so
  in the run's trail. The history keeps everything — the session
  search and the transcript still show the whole conversation — and
  only what the *model* is sent changes. MiniMax's window is a million
  tokens, so the summary comes late there; `compact_at = 0.1` keeps the
  context to a tenth of it, and the ring in the corner shows the
  difference.

## Dictation (speech to text)

The dictation clip is transcribed by whichever endpoint the window
names. They all take the same multipart request — a `model` field and
the audio as `file` — because MiniMax deliberately mirrors the OpenAI
schema, so one code path serves them all.

| Endpoint | Default model |
| --- | --- |
| `https://api.minimax.io/v1/speech_to_text` | `asr-1.0` |
| `https://api.openai.com/v1/audio/transcriptions` | `gpt-4o-mini-transcribe` |
| `https://api.groq.com/openai/v1/audio/transcriptions` | `whisper-large-v3-turbo` |
| `https://openrouter.ai/api/v1/audio/transcriptions` | `openai/whisper-large-v3-turbo` |

An endpoint of one's own is your own server: put its key in the
`stt_api_key` entry of `keys.json` (or `PHOENIX_STT_API_KEY`).

Unlike the model calls, a changed endpoint or key applies to the *very
next* dictation — the transcriber re-resolves its configuration for
every clip, so nothing has to be restarted.

## Reading the answers aloud

The daemon can speak its own answers (opt-in, off by default): the
answer is normalized for the ear — math is spoken as mathematics
(`$\pi r^2$` becomes "pi r squared", tied to the renderer's own span
detector by a shared test fixture), code fences and URLs are not read
out — then synthesized sentence by sentence and played through a
PipeWire sink. Playback can be interrupted at any moment: the stop
control next to the record button, closing the results window (or
pressing `Esc` in it), or pressing record itself. The results window
also has a per-answer "Read aloud" button.

Enable it in the settings window (the panel's gear button → **Read
aloud**): flip the toggle, and it applies at once — the window asks the
daemon to rebuild its speech pipeline over D-Bus, so no service
restart, and turning it off mid-utterance falls silent. The same
section offers the read mode, the voice and a **playback speed** slider
(half as slow to twice as fast), plus a Test button. The speed is a
time-stretch: the voice keeps its pitch, so 1.5× is the same voice in
half less time — not a higher voice. The settings are stored in
`~/.config/phoenix-dna/keys.json` as `tts_*` entries; power users can
still set the same things in `~/.config/phoenix-dna/agent.toml`, which
wins over the file the window writes:

```toml
[agent]
tts_enabled = true
# tts_read_mode = "stream"    # speak while the answer is generated
# tts_voice = "English_expressive_narrator"
# tts_speed = 1.0
# tts_playback_speed = 1.0    # how fast it is played back (pitch kept)
```

Synthesis uses MiniMax's streaming `t2a_v2` endpoint (`speech-2.8-turbo`)
with the agent's own MiniMax key; a dedicated `tts_api_key` entry in
`keys.json` (or `PHOENIX_TTS_API_KEY`) overrides it. Every setting has a
`PHOENIX_TTS_*` environment spelling (`PHOENIX_TTS_ENABLED`, …) that
beats the file.

The **voice** has an endpoint field in the window too, and the host in
the URL decides the service — the same rule as the model's endpoint.

| Endpoint | Model | Voices |
| --- | --- | --- |
| `https://api.minimax.io/v1/t2a_v2` | `speech-2.8-turbo` | `English_expressive_narrator` and the rest of MiniMax's catalogue |
| `https://api.openai.com/v1/audio/speech` | `gpt-4o-mini-tts` | `alloy`, `ash`, `nova`, … |
| `https://api.groq.com/openai/v1/audio/speech` | `canopylabs/orpheus-v1-english` | `troy`, `austin`, `hannah`, … |
| `https://openrouter.ai/api/v1/audio/speech` | `elevenlabs/eleven-turbo-v2` | `george`, `sarah`, `adam`, … |

Groq's own voices come with a catch: Orpheus asks the organisation's
admin to accept its terms once
(<https://console.groq.com/playground?model=canopylabs%2Forpheus-v1-english>),
and the service refuses the request until they have.

OpenRouter's speech endpoint answers in MP3 or PCM only, and carries
several voice providers (ElevenLabs, Deepgram, Google, Fish Audio);
the list is at
<https://openrouter.ai/models?output_modalities=speech>.

MiniMax speaks a different protocol from the OpenAI-shaped ones — it
answers with a hex-encoded SSE stream, they with a plain audio body — so
it is the one engine that is chosen from a list rather than named by
its URL. The on-device engine ("Local") is chosen there too, and needs
no key.

Two read modes exist, and they are genuinely different:

* **final** (default) — only the final answer is spoken. A multi-turn
  tool run never reads its preambles ("Let me check the disk"), because
  they are not part of `final_text`. Correct by construction; the
  answer still comes out sentence by sentence as it synthesizes.
* **stream** — speaking starts while the answer is generated, holding
  back one sentence (configurable); if the turn turns out to call
  tools, the held text is discarded. What was already said cannot be
  unsaid — the known cost of the mode, and the reason it is not the
  default. Guaranteeing "final answer only" and speaking before
  generation finishes are mutually exclusive: a turn is
  indistinguishable from a final turn until it ends.

**The microphone never transcribes the agent's own voice**: pressing
record barges in and stops playback first, and the daemon stops
speaking the moment a new run starts.

### Local synthesis (no cloud)

An offline engine can stand in for MiniMax: Kokoro-82M via
`kokoro-onnx`, run in a separate Python 3.13 venv (the daemon itself is
on 3.14, and `kokoro-onnx` caps below that). The daemon stays
stdlib-only — the model lives in the venv's interpreter, spoken to over
a small stdio protocol:

```bash
python3.13 -m venv ~/.local/share/phoenix-dna/tts-venv
~/.local/share/phoenix-dna/tts-venv/bin/pip install kokoro-onnx onnxruntime soundfile
# download kokoro-v1.0.onnx and voices.json into the same directory
```

```toml
[agent]
tts_provider = "local"
tts_local_venv = "~/.local/share/phoenix-dna/tts-venv"
tts_voice_local = "~/.local/share/phoenix-dna/kokoro-v1.0.onnx"  # voices.json beside it
```

`piper-tts` is accepted as the fallback: point `tts_voice_local` at a
Piper `.onnx` voice (with no `voices.json` beside it) and install
`piper-tts` in the venv instead. Without the venv, the daemon starts
unchanged and speech simply stays off.

Desktop-wide speech with a Piper voice through speech-dispatcher
(`sd_generic`) and Orca is a separate concern — a system configuration
this project deliberately does not touch. So is `QtTextToSpeech` in the
results window (available as `import QtTextToSpeech` on this box): its
`speechd` backend lacks the `Synthesize` capability, which makes it
fit only for short ad-hoc utterances, never for answer streaming — not
wired up to keep the change lean.

## License

BSD-2-Clause — see [LICENSE](LICENSE).

Every source file carries an SPDX header, so the licence can be detected
automatically:

```bash
reuse lint
```
