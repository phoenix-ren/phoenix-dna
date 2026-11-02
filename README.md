<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Phoenix DNA

A voice-first desktop AI agent for KDE Plasma 6, packaged as a plasmoid
(panel widget) plus a Python D-Bus daemon.

The plasmoid captures speech, transcribes it, and forwards the text to the
agent daemon over D-Bus. The daemon runs an LLM tool loop in one of three
modes, streams progress back, and renders the result in the panel or in the
overlay/pane.

## Layout

| Path         | Contents                                                        |
|--------------|-----------------------------------------------------------------|
| `shell/`     | Panel widget: the main plasmoid, audio capture, transcription   |
| `overlay/`   | Dictation overlay shown while recording                         |
| `pane/`      | Results pane for conversation output                            |
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
(rebindable in *System Settings → Shortcuts → Phoenix DNA*). The picker
highlights the window under the mouse and photographs the one that is
clicked — the click never reaches the window itself. The picture is
attached to the prompt that is being composed, like any other file.

Both need KWin, which is where the pointer is followed, the target window
is highlighted and the picture is taken.

## Building

```bash
sudo cmake --install build
systemctl --user restart plasma-plasmashell   # otherwise the panel keeps the old plugin
journalctl --user -b --no-pager | grep -i phoenix   # check for QML errors
```

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
when a prompt asks for it. See `agent/README.md` for the details.

## License

BSD-2-Clause — see [LICENSE](LICENSE).

Every source file carries an SPDX header, so the licence can be detected
automatically:

```bash
reuse lint
```
