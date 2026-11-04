<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA

![Phoenix DNA](icons/phoenix-dna.svg)

A voice-first desktop AI agent for KDE Plasma 6, packaged as a plasmoid
(panel widget) plus a Python D-Bus daemon.

The plasmoid captures speech, transcribes it, and forwards the text to the
agent daemon over D-Bus. The daemon runs an LLM tool loop as the desktop
user, streams progress back, and renders the result in a fully-fledged results
window. When the agent needs a decision only the user can make, it asks: the
question appears in the results window as a card — radio buttons (or
checkboxes) per option, or the user's own words — and the run continues once
it is answered. Several questions arrive as one form and open a wizard
overlay of its own: large, centred, themed; one question at a time with a
back button and exact progress, each answer required before the next, and
everything sent together at the end — the conversation returning the moment
you submit.

The panel's search button (or Meta+Alt+`) opens the session search: the
user's previous sessions on the left, under the name the daemon gave them,
and the selected session's queries and responses on the right. Double-clicking
a session opens it in the results window, where the conversation can be
continued.

## The trail of a run

The results window shows what the agent did while it worked, in the order it
did it: the notes it thinks in, the words it says, the tools it calls. Each
step is one line — "Used 1 Thought", "Used 2 Edit" — that opens into the
step's own text: the reasoning, the command with its output, the code it
changed. The steps of one stretch belong together, so a run of eight calls
reads as one entry.

The answer comes last and on its own, in the agent's font and at full size —
the point of the run. What the agent *says* on the way there — the working
notes that precede a tool call — is set the same way: it is the agent's own
words, and it is read as such. Only the titles of the steps around them are
dimmed, and only the reasoning and the tool output stay behind their titles:
they are the model's private notes and a log, not words for the user.

Because the trail is written as the run goes on, the window can be followed
while the agent works. A step that is still running says so, and says for
how long — the difference between an agent at work and an agent that hung.

None of it is read aloud. The read-aloud speaks the answer — the words after
the last tool call — and never a working note; the same is true of the
daemon's own speech (see *Reading the answers aloud* below).

The microphone button and the waveform next to it are one big button: the
waveform is part of the button's target, so hovering it lights the button up
and clicking it starts or stops the recording.

## Typing a query

Not every prompt has to be spoken. The keyboard button at the far left of the
panel opens the same prompt editor as the microphone, but empty and with
the keyboard in it: nothing is recorded, nothing is transcribed, and the
query is sent to the agent like a dictated one. The global shortcut
`Meta+Alt+Return` does the same from any application.

In every prompt editor — the dictation overlay as much as the results
window's follow-up field — `Return` sends, and `Shift+Return` is the line
break. A query of several sentences is written without the first `Return`
sending it away, and the send button is a plain up arrow inside the prompt
field, doing with a click what `Return` does with the keyboard.

While the agent is working on a query, that same button is a stop button:
one press interrupts the run (its tools are told too), and the conversation
shows how far it got. A finished query announces itself with a soft chime —
the answer can be read when it is heard, without the window being watched.

The button is a toggle — pressing it again puts the editor away, and the
query stays as a draft, so a mis-click costs nothing. `Esc` is the way to
throw the draft away: the editor closes and the query is gone.

The conversation window owns the prompt while it is on screen: a typed
query continues that session, exactly like a dictated follow-on.

## The dictated prompt on the clipboard

When the voice input has ended and the prompt editor opens on the
transcript, the prompt is put on the clipboard — so it can be pasted
into another application without being picked out of the editor by
hand. The editor remains the place where the prompt is checked and
corrected; corrections made there are not copied.

## Layout

| Path         | Contents                                                        |
|--------------|-----------------------------------------------------------------|
| `shell/`     | Panel widget: the main plasmoid, audio capture, transcription   |
| `overlay/`   | Dictation overlay shown while recording                         |
| `resultswindow/` | Fully-fledged results window for conversation output        |
| `search/`    | Session search: the previous sessions, listed by name           |
| `settingswindow/` | Settings window: appearance, API keys and global shortcuts |
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

* **API keys** — the LLM key (the agent daemon's MiniMax key) and the
  speech-to-text key, with the speech-to-text provider (MiniMax or OpenAI)
  chosen alongside it. Both are stored in
  `~/.config/phoenix-dna/keys.json` (mode 0600), the file the daemon and
  the transcriber already read. A changed speech-to-text key applies to
  the next dictation; the daemon reads its key at startup, so a changed
  LLM key applies after `systemctl --user restart phoenix.service`.
  Environment variables still take precedence over the file, and the
  window says so when it sees one set.

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
agent/systemd/install-user-service.sh      # once
systemctl --user status phoenix.service
journalctl --user -u phoenix.service -f
```

It runs as the desktop user: it lives on the session bus that
the plasmoid connects to, and it elevates to root on demand — a command
that calls `sudo` makes the daemon ask for the password (KDE password
dialog) and run it as root, with a confirmation card (Allow/Deny) for
destructive commands, answered by you before they run. See `agent/README.md`
for the details.

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
