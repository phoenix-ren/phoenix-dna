// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Overview — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Overview", subtitle: "What Phoenix DNA is, how it is built, and why it is native rather than web.", active: "")

Phoenix DNA is a *desktop-native AI agent* for Linux/KDE: it lives in the
Plasma panel, takes dictated or typed prompts, runs them through an
LLM-driven agent loop *as your desktop user*, and shows the answer in a
native window. It can attach files and screenshots to a prompt, escalate to
administrator rights through KDE's own password dialog when a command needs
them, and read its answers aloud.

= Two halves, one package

An install puts two cooperating halves on the machine:

- *The Plasma applet* — a C++ plugin, five QML modules (the dictation
  overlay, the results window, the session search, the settings window, the
  shared theme), the icon set and a desktop entry. This is what you click.
- *The agent daemon* — the Python package `phoenix_dna` behind
  `phoenix-dna-agent`: a systemd *user* service owning the D-Bus name
  `dna.phoenix.Agent`. This is what thinks. D-Bus activation starts it on
  the plasmoid's first call, so nothing has to be enabled by hand.

They talk over the session bus — progress streams back as signals, the
wizard's questions and your answers travel the same road.

= Design philosophy

- *Native over web.* Virtually every AI agent on the market is
  lowest-common-denominator (sort-of) runs-anywhere web. In a capable
  programmer's hands, native is faster, harder, stronger, better — and it
  can do things a browser tab cannot: sit on the panel, take a screenshot
  through KWin, ask PAM for the password, follow the desktop's colour
  scheme.
- *Your desktop user, not a sandbox and not root.* The daemon runs as you,
  on your session bus, writing `~/.config/phoenix-dna`. When a tool calls
  `sudo`, the KDE password dialog asks *you*, a confirmation card covers
  destructive commands, and the elevation lasts for that one command.
- *The keys stay yours.* API keys live in `~/.config/phoenix-dna/keys.json`
  (mode 0600) or the environment — never in a vendor cloud. Any
  OpenAI-compatible endpoint serves the model, the dictation and the voice,
  including your own server.
- *Packaged like the distro would.* One CMake project installs both halves;
  the RPM, DEB and PKGBUILD recipes are real files exercised on fresh VMs,
  not documentation snippets.

= The shape of a conversation

+ *You press record and talk* — the dictation overlay appears with the
  waveform; the mic never listens on its own. Press again (or use the
  typed-prompt window) and the clip goes to the speech-to-text endpoint.
+ *The prompt can carry more than words* — files from the picker, a
  screenshot of the window you pointed at, each shown as a chip.
+ *The agent loop runs as you* — commands, file reads, whatever the task
  needs, streamed back as it goes. The CPU meter shows the running tool,
  the wire meter the pace of the talk with the model.
+ *The answer lands in a native window* — Markdown with formulas rendered,
  addresses clickable, an optional voice reading it aloud, and the whole
  session kept for the session search.

= Where things live

| Path | Contents |
|---|---|
| `shell/` | Panel widget: the plasmoid, audio capture, transcription |
| `overlay/` | Dictation overlay shown while recording |
| `resultswindow/` | The conversation window |
| `search/` | Session search over previous sessions |
| `settingswindow/` | Settings: appearance, API keys, global shortcuts |
| `theme/` | Shared colour scheme, icon set, Markdown helpers |
| `waveform/` | Offline `.wav` → `.png` waveform renderer |
| `agent/` | The Python agent daemon |

= License

BSD-2-Clause. Every source file carries an SPDX header, so the licence can
be detected automatically with `reuse lint`. The icon set is Lucide's
drawings; licence texts for everything ship in
`/usr/share/licenses/phoenix-dna`.
