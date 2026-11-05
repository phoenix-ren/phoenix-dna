#!/bin/sh
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Build the Phoenix DNA homepage into site/ (HTML via typst's HTML export).
#
# Usage: docs/homepage/build.sh
#
# The typst version is pinned: HTML export is still marked experimental
# upstream, so the output must not drift between builds. Set TYPST_BIN to
# use an existing binary; otherwise the pinned one is downloaded once into
# .typst-tool/ (gitignored).

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$root"

TYPST_VERSION=0.15.1

if [ -n "${TYPST_BIN:-}" ] && [ -x "${TYPST_BIN:-}" ]; then
    typst="$TYPST_BIN"
else
    typst="$root/.typst-tool/typst"
    if [ ! -x "$typst" ]; then
        mkdir -p "$root/.typst-tool"
        url="https://github.com/typst/typst/releases/download/v$TYPST_VERSION/typst-x86_64-unknown-linux-musl.tar.xz"
        echo "downloading typst $TYPST_VERSION from $url" >&2
        curl -fsSL "$url" | tar -xJ -C "$root/.typst-tool" --strip-components=1
    fi
fi

site="$root/site"
rm -rf "$site"
mkdir -p "$site"

for src in pages/*.typ; do
    name=$(basename "$src" .typ)
    [ "$name" = index ] && out=index.html || out="$name.html"
    "$typst" compile --features html --format html --root "$root" "$src" "$site/$out"
done

# typst emits <meta charset> and <title> but no stylesheet links; add ours
# (plus a no-flash theme bootstrap) to every page.
for html in "$site"/*.html; do
    sed -i 's#</head>#<link rel="stylesheet" href="theme.css"><link rel="icon" type="image/svg+xml" href="assets/phoenix-dna.svg"><script>(function(){var t=localStorage.getItem("pd-theme");if(t)document.documentElement.dataset.theme=t})();</script><script src="site.js" defer></script></head>#' "$html"
done

cp theme.css site.js "$site"
cp -r assets "$site"

echo "$site"
