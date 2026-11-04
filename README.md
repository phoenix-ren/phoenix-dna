<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA

![Phoenix DNA](icons/phoenix-dna.svg)

Do you suffer from advanced AI psychosis? Then this tool is for *you*!
Phoenix DNA is the world's first **desktop-native agent**; an AI agent 
specifically tailored for desktop use on Linux/KDE. In other agents, getting
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
