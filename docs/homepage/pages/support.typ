// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Support Phoenix DNA development ❤ — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Support Phoenix DNA development ❤", subtitle: "Phoenix DNA is made by one person and her AIs. Here is how to keep it flying.", active: "support")

Phoenix DNA is free software, BSD-2-Clause, and it stays that way. It is
also a lot of surface for one person: a C++/QML Plasma applet, a Python
agent daemon, three packaging recipes and a homepage. If it earns its keep
on your desktop, there are three ways to help — pick what you have.

= Sponsor the maker 💜

#html.elem("p", [
  #cta(sponsors-url, [Sponsor Phoenix Ren on GitHub])
])

One-time or monthly, every tier — the money buys the machines the packaging
recipes are verified on (fresh Fedora, Arch and Kubuntu VMs are not free),
the models the agent is developed against, and the time a native app of
this scope demands.

= Contribute code and words

The repository is #html.elem("a", attrs: (href: repo-url), [phoenix-ren/phoenix-dna on GitHub]).

- *Bugs and ideas* — the
  #html.elem("a", attrs: (href: repo-url + "/issues"), [issue tracker])
  is the right place for both; a reproducer with a `journalctl --user -u
  phoenix.service` excerpt is gold.
- *Pull requests* — the style files (`.clang-format` for C++, the existing
  SPDX headers everywhere) decide the shape of a patch; every source file
  carries an SPDX header, and `reuse lint` must pass.
- *Tests* — the Python suite runs without CMake:
  `python3 -m pytest -q` inside `agent/`; the CTest suite builds with
  `-DBUILD_TESTING=ON`.
- *The homepage* — this site lives in `docs/homepage/` in the repository,
  written in typst. Typos and better words are welcome.

= Package it, spread it

- The recipes are real files, exercised end to end on fresh VMs — if you
  maintain a distro package, a COPR, a PPA or an AUR helper integration,
  #html.elem("a", attrs: (href: repo-url + "/blob/master/PACKAGING.md"), [PACKAGING.md])
  is written for you.
- Screenshots, videos and write-ups help more than you think — and the
  #html.elem("a", attrs: (href: "press.html"), [press page]) collects them.

= Thanks

Phoenix DNA stands on: KDE Plasma and Frameworks, Qt, PipeWire, systemd —
and, provocatively, on GLM and MiniMax, the AIs that helped write it. The
icon set is #html.elem("a", attrs: (href: "https://lucide.dev"), [Lucide]).
Made with 🏳️‍⚧️ 💜 and attitude.
