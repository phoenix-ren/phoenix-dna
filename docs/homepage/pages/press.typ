// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Press mentions — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Press mentions", subtitle: "Coverage of Phoenix DNA around the web — and how to get listed here.", active: "press")

Phoenix DNA went public on 5 November 2026. If you write, record or stream
about it, thank you — send a link
(#html.elem("a", attrs: (href: repo-url + "/issues"), [open an issue]) or a
pull request against this page) and it will be listed here with the date.

== Articles and posts

None yet — the ink is still drying on the announcement. Yours can be the
first.

== Video reviews

None yet. A note for anyone making one: the interesting ninety seconds are
the *Meta+Alt* screenshot picker dropping a window into the prompt, the
sudo confirmation card in front of a destructive command, and the answer
being read aloud while the meters move.

== For reviewers and journalists

- What it is, in one line: *a voice-first AI agent that lives in the KDE
  Plasma panel and runs as your desktop user, not in a browser tab.*
- The mark and the screenshots:
  #html.elem("a", attrs: (href: repo-url + "/tree/master/icons"), [icons]) in the repository (SVG + PNG);
  the logo is `icons/phoenix-dna.svg`.
- Licence: BSD-2-Clause. Maker: Phoenix Ren (aka Michelle Percossi).
  Sponsors: #html.elem("a", attrs: (href: sponsors-url), [github.com/sponsors/phoenix-ren]).
- Binaries for testing: the
  #html.elem("a", attrs: (href: repo-url + "/releases"), [releases page])
  carries CI-built RPM/DEB/PKGBUILD packages for Fedora 45+, Kubuntu 26.04+
  and Arch/CachyOS (also on the AUR as `phoenix-dna`).
