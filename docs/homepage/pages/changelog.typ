// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Changelog — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Changelog", subtitle: "What changed, release by release.", active: "changelog")

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
