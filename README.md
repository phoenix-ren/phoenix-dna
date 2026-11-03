<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA

A voice-first desktop AI agent for KDE Plasma 6, packaged as a plasmoid
(panel widget) plus a Python D-Bus daemon.

The plasmoid captures speech, transcribes it, and forwards the text to the
agent daemon over D-Bus. The daemon runs an LLM tool loop in one of two
modes, streams progress back, and renders the result in a fully-fledged results
window. When the agent needs a decision only the user can make, it asks: the
question appears in the results window as a card — radio buttons (or
checkboxes) per option, or the user's own words — and the run continues once
it is answered. A series of questions switches the window into a wizard view:
one question at a time, with a progress bar, the answers collected so far,
and the conversation returning when the agent has what it needs and goes to
work.

The panel's search button (or Meta+Alt+`) opens the session search: the
user's previous sessions on the left, under the name the daemon gave them,
and the selected session's queries and responses on the right. Double-clicking
a session opens it in the results window, where the conversation can be
continued.

## Typing a query

Not every prompt has to be spoken. The keyboard button next to the record
button opens the same prompt editor as the microphone, but empty and with
the keyboard in it: nothing is recorded, nothing is transcribed, and the
query is sent to the agent like a dictated one. The global shortcut
`Meta+Alt+Return` does the same from any application.

The button is a toggle — pressing it again puts the editor away, and the
query stays as a draft, so a mis-click costs nothing. `Esc` is the way to
throw the draft away: the editor closes and the query is gone.

The conversation window owns the prompt while it is on screen: a typed
query continues that session, exactly like a dictated follow-on.

## Layout

| Path         | Contents                                                        |
|--------------|-----------------------------------------------------------------|
| `shell/`     | Panel widget: the main plasmoid, audio capture, transcription   |
| `overlay/`   | Dictation overlay shown while recording                         |
| `resultswindow/` | Fully-fledged results window for conversation output        |
| `search/`    | Session search: the previous sessions, listed by name           |
| `settingswindow/` | Settings window: appearance, API keys and global shortcuts |
| `theme/`     | Shared colour-scheme singleton and Markdown presentation helpers |
| `waveform/`  | Offline `.wav` → `.png` waveform renderer                       |
| `widgettest/`| Developer testbench that hosts the widget in a plain window      |
| `agent/`     | Python agent daemon (`phoenix-dna-agent`), a D-Bus service          |

## Prompts with files

A prompt is not only text: the user can attach files to it. The "+" button
on the left of the prompt (in the dictation overlay and in the results
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

It runs as the desktop user, not as root: it lives on the session bus that
the plasmoid connects to, and it elevates to root on demand through `sudo`
when a prompt asks for it — with a confirmation card (Allow/Deny) for
destructive commands, answered by you before they run. See `agent/README.md`
for the details.

## License

BSD-2-Clause — see [LICENSE](LICENSE).

Every source file carries an SPDX header, so the licence can be detected
automatically:

```bash
reuse lint
```
