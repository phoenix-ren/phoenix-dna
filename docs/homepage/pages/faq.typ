// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "FAQ — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "FAQ", subtitle: "The questions that come up when an agent moves into the panel.", active: "faq")

#html.elem("h3", attrs: (class: "faq-q"), [Which distributions can run it?])
KDE Plasma *6.4 or newer* and Qt *6.7 or newer*: Fedora 45+, current Arch and
CachyOS, Ubuntu 25.10/26.04+, Debian sid. Debian 13 (trixie) ships Plasma 6.3
and Ubuntu 24.04 carries Qt 6.4/Plasma 5 — both are too old to build this
tree.

#html.elem("h3", attrs: (class: "faq-q"), [Is my microphone always listening?])
No. Recording starts when you press the record button or its global shortcut,
and stops when you press again. Nothing else opens the mic — and when the
agent is reading an answer aloud, pressing record barges in and stops the
voice first, so the two never talk over each other.

#html.elem("h3", attrs: (class: "faq-q"), [Where do my API keys live?])
In `~/.config/phoenix-dna/keys.json`, created with mode 0600, or in the
environment (`OPENAI_API_KEY`, `GROQ_API_KEY`, `OPENROUTER_API_KEY`,
`MINIMAX_API_KEY`, …). The settings window shows when an environment
variable is overriding the file. Nothing is sent anywhere except the
endpoint you named.

#html.elem("h3", attrs: (class: "faq-q"), [Can I use my own server, or avoid a big cloud entirely?])
Yes — name any OpenAI-compatible endpoint in the settings window; the host
in the URL decides the service, and an unknown host is treated as your own
server (you name the model, and the key goes in the `llm_api_key` entry).
The read-aloud voice can run entirely on-device with Kokoro or Piper —
see #html.elem("a", attrs: (href: "features.html"), [Features → Local synthesis]).

#html.elem("h3", attrs: (class: "faq-q"), [How does `sudo` work without a terminal?])
The agent runs as your desktop user. When a tool needs administrator rights
it calls `sudo`, and the daemon asks for the password with *KDE's own
password dialog* (`kdialog`). Destructive commands first raise a
confirmation card — Allow/Deny — that you answer before anything runs.
Elevation is per-command; there is no standing root shell.

#html.elem("h3", attrs: (class: "faq-q"), [Why does a new key want a service restart?])
The daemon reads its LLM configuration at startup. After changing the key or
endpoint in the settings window, run `systemctl --user restart
phoenix.service` — the window says so at the right moment. Dictation and the
voice are different: a changed speech-to-text key applies to the *next*
dictation, and the voice pipeline rebuilds the moment you change it, no
restart.

#html.elem("h3", attrs: (class: "faq-q"), [What gets sent to the model?])
Your prompt, the files you deliberately attached, and the screenshots you
took. Nothing else leaves the machine: no ambient capture, no desktop
streaming. The daemon talks to exactly the endpoint you configured.

#html.elem("h3", attrs: (class: "faq-q"), [Do I have to talk? Can I use it keyboard-only?])
Yes. The typed-prompt window has its own global shortcut, follow-up
questions can be typed in the results window, and every shortcut is
rebindable in *System Settings → Shortcuts → Phoenix DNA*.

#html.elem("h3", attrs: (class: "faq-q"), [The formulas in answers look like source code. Why?])
LaTeX formulas are rendered to PDF by a TeX engine — install `texlive-xetex`
(or lualatex/pdflatex). Without one, the formulas stay verbatim rather than
being mangled by a broken renderer; that is deliberate.

#html.elem("h3", attrs: (class: "faq-q"), [Where is the daemon's log?])
`journalctl --user -u phoenix.service -f`. The status of the unit:
`systemctl --user status phoenix.service`. For QML errors after an install,
`journalctl --user -b | grep -i phoenix` after restarting plasmashell.

#html.elem("h3", attrs: (class: "faq-q"), [How do I uninstall?])
With your package manager: `dnf remove phoenix-dna`,
`pacman -R phoenix-dna` or `sudo apt remove phoenix-dna`. User data lives in
`~/.config/phoenix-dna` and `~/.local/share/phoenix-dna` — delete those two
and nothing is left.

#html.elem("h3", attrs: (class: "faq-q"), [Who makes this, and under what licence?])
Phoenix Ren (aka Michelle Percossi) and her trusty GLM and MiniMax AIs —
made with 🏳️‍⚧️ 💜 and attitude. BSD-2-Clause; every file carries an SPDX
header and `reuse lint` passes. If Phoenix DNA earns its keep, consider
#html.elem("a", attrs: (href: sponsors-url), [sponsoring its development]).
