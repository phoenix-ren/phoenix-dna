// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Phoenix DNA — the voice-first AI agent for the KDE Plasma panel")
#import "/layout.typ": *
#show: page.with(active: "")

#html.elem("div", attrs: (class: "hero"), [
  #html.elem("img", attrs: (src: "assets/phoenix-dna.svg", alt: "Phoenix DNA mark"))
  #html.elem("h1", "Phoenix DNA")
])
#html.elem("p", attrs: (class: "tagline"), [
  The voice-first AI agent that lives in your KDE Plasma panel. \
  Dictate a prompt, get a native answer — with the desktop, not a browser tab, doing the work.
])
#html.elem("div", attrs: (class: "badges"), [
  #html.elem("span", attrs: (class: "badge accent"), [v0.2.0])
  #html.elem("span", attrs: (class: "badge"), [KDE Plasma 6.4+])
  #html.elem("span", attrs: (class: "badge"), [Qt 6.7+])
  #html.elem("span", attrs: (class: "badge"), [Fedora · Arch · Kubuntu])
  #html.elem("span", attrs: (class: "badge"), [BSD-2-Clause])
])

#html.elem("p", [
  Phoenix DNA is the world's first *desktop-native* agent: an AI agent tailored
  for desktop use on Linux/KDE, not a lowest-common-denominator web app that
  sort-of runs anywhere. A widget on the panel, a daemon at your user's
  service, and an answer window that belongs to your desktop.
])

= Why DNA is different

#html.elem("div", attrs: (class: "cards"), [
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Always within reach])
    #html.elem("p", [A widget on the panel. One hotkey, and you're talking to it — no window hunting, no alt-tabbing to a browser.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Voice in, voice out])
    #html.elem("p", [Dictate the prompt; have the answer read aloud while you're away from the desk. The mic barges in over the voice when you speak.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Authentication built in])
    #html.elem("p", [Native KDE password dialog for `sudo`, with an Allow/Deny card for destructive commands — your DNA can `dnf install` for you.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Screenshot-to-prompt])
    #html.elem("p", [`Meta+Alt` picks a window, photographs it, and attaches the picture to the prompt you're writing.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Wizards, like it's 1999])
    #html.elem("p", [When the agent needs a decision, it asks — and the question arrives as a wizard window of its own: one at a time, a progress bar, a Back button, and every answer sent together. Answer your AI like it's 1999.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [The archive is yours])
    #html.elem("p", [Every conversation is kept on your own disk, named by the agent, found by a few keystrokes. No account, no vendor cloud, no import button.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Answers with working links])
    #html.elem("p", [Files, folders, web pages, mailboxes and phone numbers in an answer are clickable, and open where the desktop handles them.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Meters that tell the truth])
    #html.elem("p", [A CPU meter on the running tool, a wire meter in bytes per second, a transfer meter on the download in progress — and a ring for the context the session has used.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Transfers you can watch])
    #html.elem("p", [An ISO is not a command. Downloads — web, FTP, torrent magnets — and uploads are a tool of their own, metered as they run, with a budget and a stall alarm.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Fork, and try again])
    #html.elem("p", [The answer went nowhere? Branch the conversation at any of your own prompts: the fork grows on its own, and the original keeps its history.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Provider-agnostic])
    #html.elem("p", [Any OpenAI-compatible endpoint serves the model, the dictation and the voice: MiniMax, OpenAI, Groq, OpenRouter — or your own server.])
  ])
  #html.elem("div", attrs: (class: "card"), [
    #html.elem("h4", [Packaged like a native])
    #html.elem("p", [Real RPM, DEB and PKGBUILD recipes, verified on fresh distro VMs — and a fully offline voice option with Kokoro or Piper.])
  ])
])

= Quickstart

#html.elem("div", attrs: (class: "distros"), [
  #html.elem("div", attrs: (class: "distro"), [
    #html.elem("h4", [Fedora 45+])
    ````bash
    dnf install \
      https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/phoenix-dna-0.2.0-1.fc45.x86_64.rpm
    ````
  ])
  #html.elem("div", attrs: (class: "distro"), [
    #html.elem("h4", [Arch / CachyOS])
    ````bash
    yay -S phoenix-dna    # AUR
    ````
  ])
  #html.elem("div", attrs: (class: "distro"), [
    #html.elem("h4", [Kubuntu 26.04+])
    ````bash
    curl -LO https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/phoenix-dna_0.2.0-1_amd64.deb
    sudo apt install ./phoenix-dna_0.2.0-1_amd64.deb
    ````
  ])
])

#html.elem("p", [
  Then add *Phoenix DNA* to your panel (right-click the panel → *Add widgets*),
  put an API key in the settings window, and you're talking. All the detail is
  on the #html.elem("a", attrs: (href: "quickstart.html"), [Quickstart page]) —
  including building from source for every supported distro — and the
  #html.elem("a", attrs: (href: "manual.html"), [manual]) walks every button
  and shortcut in turn.
])
