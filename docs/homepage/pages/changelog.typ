// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Changelog — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Changelog", subtitle: "What changed, release by release.", active: "changelog")

== 0.2.0 [2026-11-05]

The conversation gets instruments of its own — and an archive it can be
tended in. Everything below is new since 0.1.0:

- *Metered transfers* — a download (a web URL, an FTP server, a torrent
  magnet) or an upload is a tool call of its own, metered while it runs:
  the pace of the bytes and how much of the thing is here. A download
  writes `.part` and is renamed into place only when it is complete, the
  budget defaults to an hour and caps at a day, and a socket silent for
  ninety seconds is a stalled transfer, not a slow one. The prompts warn
  the model off `curl` and `wget` for moving files — by name, because
  the decision it has to interrupt is made in commands.
- *The context view* — a ring in the corner of the conversation window
  says how much of the model's context window the session has used. A
  click opens the numbers behind the share: the model in force, how much
  it holds at once, the tokens the session spent (the fresh input, the
  output, the part the server answered from its cache, the reasoning),
  the turns it holds, what it cost, and when it began.
- *Forks* — a conversation can be branched into a session of its own at
  any of your own prompts. The fork holds everything up to the prompt —
  the prompt itself and the answer that followed it — and the original
  is not touched: the two grow apart.
- *Session tending* — name a session yourself and the name stands: the
  daemon is never asked for another. Throw a session away with a
  confirmation that names it — the list is full of similar names, and
  the wrong one must not go.
- *The settings window restarts the daemon for you* when the LLM key is
  saved. No more pasting a key and then doing `systemctl --user restart
  phoenix.service` by hand; the endpoint and the model still ask for it,
  and the window says so when they do.
- *This website* — new since 0.1.0: a manual that walks every window,
  button and shortcut in the order a conversation goes, the features in
  detail, per-distro quickstarts, and this changelog, compiled from the
  repository and deployed on every push.

== 0.1.0 [2026-11-05]

First public release. Everything below is new — the highlights, as the
release sees them:

- *The agent itself* — a Plasma panel widget and a Python agent daemon
  (`phoenix-dna-agent`, systemd user service, D-Bus activation) that runs
  an LLM-driven tool loop as the desktop user, streaming progress back
  over the session bus.
- *Voice-first input* — dictation with a live waveform overlay,
  transcribed by any OpenAI-compatible speech endpoint (MiniMax, OpenAI,
  Groq, OpenRouter, or your own); a typed-prompt window for the keyboard
  days; dictated prompts are copied to the clipboard for other agents.
- *Prompts with files and screenshots* — a file picker with chips, and a
  KWin window-picker (`Meta+Alt`) that photographs the window you point
  at without the click ever reaching it.
- *Native administrator rights* — KDE's password dialog for `sudo`, with
  an Allow/Deny confirmation card for destructive commands.
- *A native answer window* — Markdown with independently chosen light/dark
  answer fonts, LaTeX formulas rendered to PDF when a TeX engine exists,
  reasoning shown separately from the answer, and everything that names a
  thing (files, folders, URLs, mailboxes, phone numbers) a clickable link
  the desktop knows how to open.
- *Meters* — CPU usage of the running tool, and the wire rate of the
  conversation in bytes per second, in and out.
- *Read-aloud answers* — ear-normalized synthesis through PipeWire with
  two read modes (final-only by default, streaming opt-in), a
  pitch-preserving speed slider, and a fully on-device option (Kokoro-82M
  or Piper) that needs no key at all.
- *Sessions* — every conversation kept and searchable by name and full
  text; the ask-user wizard hands questions back with your answers when a
  submit is refused.
- *Provider-agnostic everything* — the model, the dictation and the voice
  each take any endpoint, with the settings window as the single place to
  name them; keys live locally in `~/.config/phoenix-dna/keys.json`
  (0600) or the environment.
- *Settings window* — appearance (follow system / force light / dark),
  API keys, read-aloud, and global shortcuts bound through KGlobalAccel.
- *Packaging* — Fedora RPM, Arch PKGBUILD (also on the AUR as
  `phoenix-dna`) and Debian/Kubuntu packaging, verified end to end on
  fresh VMs, with CI-built binaries on the releases page; BSD-2-Clause
  with SPDX headers throughout, `reuse`-clean.

#html.elem("p", attrs: (class: "subtitle"), [
  Earlier history lives in the git log — one branch per feature, merges
  told as stories. The #html.elem("a", attrs: (href: repo-url + "/releases"), [releases page]) carries the artifacts.
])
