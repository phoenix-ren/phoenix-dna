// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Features — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Features", subtitle: "What the desktop-native design buys you, in detail.", active: "features")

= Voice in

The record button (or its global shortcut, rebindable) starts dictation. A
overlay comes up with the live waveform; press again and the clip is
transcribed *by whichever endpoint you named* — MiniMax, OpenAI, Groq,
OpenRouter or your own server, the same multipart request for all. The
transcriber re-resolves its configuration for every clip: a changed endpoint
or key applies to the very next word you say.

Prefer the keyboard? A typed-prompt window has its own global shortcut, and
a dictated prompt is always copied to the clipboard too — for easy pasting
into the prompt boxes of lesser agents.

= Prompts with files

A prompt is not only text. The `+` button opens a file picker; every picked
file becomes a chip beside the prompt and travels to the daemon, where the
agent can read it. Images are shown to the model directly; the text of a PDF
is extracted for it.

== Screenshot-to-prompt

The camera button (or `Meta+Alt`) starts KWin's window picker: the pointer
is followed, the window under it named in the panel's status line, and the
window you click is *photographed* — the click never reaches the window
itself. The picture attaches to the prompt you were writing, named after
the window it shows. Your queries often relate to what is on the screen;
this makes asking about it one gesture.

= Administrator rights, done natively

The agent runs as your desktop user. When a tool needs more, it calls
`sudo` — and out comes *KDE's own password dialog*, not a piped prompt in a
terminal nobody is watching. Destructive commands additionally raise a
confirmation card (Allow/Deny) that you answer before they run. This is the
`apt install` experience web agents keep fumbling, done the way the desktop
intended.

= Answers you can act on

The results window renders Markdown with the agent's own font — light mode
set like a news article, dark mode like a screen that never sleeps. LaTeX
formulas render to PDF pages when a TeX engine is installed, and stay
verbatim otherwise.

== Everything that names a thing is a link

Files, folders, web pages, mailboxes, phone numbers — in the answer, in the
tool log, in the session search — are detected on the rendered text and
become clickable:

- a folder opens in Dolphin; a file opens *selected in its folder*, because
  where a file is is the useful thing to show;
- a page opens in the browser, an address in the mail client, a number in
  the dialer;
- the detection is careful: a path must begin with a system root or `~`, a
  number must actually look like a phone number (a date, a version, an IP
  address are not), and a bare name with no place is left alone.

== The meters

Two meters tell you what the agent is doing:

- a *CPU meter* on the running tool — so the command that has been going
  for five minutes proves it is alive;
- a *wire meter* — bytes per second, in and out, the pace of the
  conversation with the model.

= Voice out: read-aloud answers

Opt-in, off by default. The answer is normalized for the ear — math is
spoken as mathematics (`$\pi r^2$` becomes "pi r squared"), code fences and
URLs are skipped — then synthesized sentence by sentence and played through
PipeWire. Interrupt it at any moment: the stop control, closing the window,
`Esc`, or pressing record (the microphone barges in over the voice, and the
daemon falls silent the moment a new run starts). A playback-speed slider
time-stretches without changing the pitch.

Two genuinely different read modes:

- *final* (default) — only the final answer is spoken; a multi-turn tool
  run never reads its preambles aloud. Correct by construction.
- *stream* — speaking starts while the answer is generated, holding back
  one sentence; if the turn turns out to call tools, the held text is
  discarded. What was already said cannot be unsaid — the known cost of
  the mode.

The voice comes from whichever endpoint you name: MiniMax's streaming
engine, OpenAI, Groq (Orpheus), OpenRouter (ElevenLabs and friends) — or
*Local*:

== Local synthesis, no cloud

An offline engine stands in: Kokoro-82M via `kokoro-onnx` in its own small
venv, or `piper-tts` as the fallback. The daemon itself stays stdlib-only
and starts unchanged without it — speech simply stays off.

#cmd([a one-time local voice setup], ```bash
python3.13 -m venv ~/.local/share/phoenix-dna/tts-venv
~/.local/share/phoenix-dna/tts-venv/bin/pip install kokoro-onnx onnxruntime soundfile
# download kokoro-v1.0.onnx and voices.json into the same directory
```,)

= Any provider, one window

The settings window names an endpoint and the host in the URL decides the
service — for the model, the dictation and the voice, each with its own
field:

| Endpoint | Default model | Key |
|---|---|---|
| `https://api.minimax.io/v1` | `MiniMax-Text-01` | `minimax_api_key` |
| `https://api.openai.com/v1` | `gpt-4o-mini` | `openai_api_key` |
| `https://api.groq.com/openai/v1` | `openai/gpt-oss-120b` | `groq_api_key` |
| `https://openrouter.ai/api/v1` | `anthropic/claude-sonnet-4.5` | `openrouter_api_key` |
| anything else | you name it | `llm_api_key` |

Models that reason out loud are handled whichever service serves them: the
reasoning is shown separately, never read aloud. Power users can pin
everything in `~/.config/phoenix-dna/agent.toml`, which overrides the
window.

= The rest of the toolbox

- *Session search* — previous sessions by name, full text, with the
  addresses in them clickable too.
- *Ask-user wizard* — when the agent needs a decision mid-run, the
  questions come back with your answers; Esc always closes, a refused
  submit hands the questions back.
- *Global shortcuts* — record, screenshot, session search, typed prompt —
  bound through KGlobalAccel, so System Settings can rebind them and they
  survive restarts.
- *Appearance* — follow the system (default) or force light/dark, applied
  the moment it is chosen; the panel widget itself keeps matching the panel
  it lives on.
- *Window identity* — every window carries the app's own mark, taskbar
  entries included.
