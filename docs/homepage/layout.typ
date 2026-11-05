// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// The shared frame of every homepage section: sidebar navigation, the
// content column, and the footer. Compiled with typst's HTML export.

#let site-url = "https://phoenix-ren.github.io/phoenix-dna"
#let repo-url = "https://github.com/phoenix-ren/phoenix-dna"
#let sponsors-url = "https://github.com/sponsors/phoenix-ren"

#let nav = (
  ("", "Overview"),
  ("quickstart", "Quickstart"),
  ("features", "Features"),
  ("faq", "FAQ"),
  ("support", "Support development ❤"),
  ("changelog", "Changelog"),
  ("press", "Press mentions"),
)

#let href(page) = if page == "" { "index.html" } else { page + ".html" }

// A code block with a caption: the command the user copies.
#let cmd(caption, code) = html.elem("figure", attrs: (class: "cmd"), [
  #html.elem("figcaption", caption)
  #html.elem("div", attrs: (class: "cmd-body"), code)
])

// A link button (call to action).
#let cta(url, label) = html.elem("a", attrs: (href: url, class: "cta"), label)

#let page(title: "", subtitle: "", active: "", body) = html.elem("div", attrs: (class: "shell"), [
  #html.elem("button", attrs: (id: "theme-toggle", title: "Switch light/dark theme", "aria-label": "Switch theme"), [◐])
  #html.elem("a", attrs: (class: "skip", href: "#main"), [Skip to content])
  #html.elem("nav", attrs: (class: "sidebar"), [
    #html.elem("a", attrs: (href: href(""), class: "brand"), [
      #html.elem("img", attrs: (src: "assets/phoenix-dna.svg", alt: "Phoenix DNA mark", class: "brand-icon"))
      #html.elem("span", [Phoenix DNA])
    ])
    #html.elem("ul", nav.map(((page, label)) => html.elem(
      "li",
      attrs: (class: if page == active { "active" } else { "" }),
      html.elem("a", attrs: (href: href(page)), label),
    )).join())
    #html.elem("div", attrs: (class: "sidebar-footer"), [
      #html.elem("a", attrs: (href: repo-url), [GitHub])
      #html.elem("span", attrs: (class: "dot"), [·])
      #html.elem("a", attrs: (href: sponsors-url), [Sponsor])
    ])
  ])
  #html.elem("main", attrs: (id: "main"), [
    #if title != "" [
      #html.elem("hgroup", [
        #html.elem("h1", title)
        #if subtitle != "" [#html.elem("p", attrs: (class: "subtitle"), subtitle)]
      ])
    ]
    #body
    #html.elem("footer", attrs: (class: "footer"), [
      #html.elem("p", [
        Made with 🏳️‍⚧️ 💜 and attitude by Phoenix Ren, and her trusty GLM and MiniMax AIs.
      ])
      #html.elem("p", [
        BSD-2-Clause — every file carries an SPDX header. \
        Icons: #html.elem("a", attrs: (href: "https://lucide.dev"), [Lucide]). \
        #html.elem("a", attrs: (href: repo-url + "/blob/master/PACKAGING.md"), [Packager docs]) · #html.elem("a", attrs: (href: repo-url + "/releases"), [Releases])
      ])
    ])
  ])
])
