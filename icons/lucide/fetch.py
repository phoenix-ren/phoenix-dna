#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Fetch the Lucide icons the app draws with.

The icons live in Lucide's own repository, in their own licence (ISC, and
MIT for the ones Feather gave them); this script copies them into
icons/lucide/ and puts the SPDX header each one needs on top of it, so the
app carries the icons it uses and nothing else.

    python3 icons/lucide/fetch.py

The list is the one the app draws; every name is a file in Lucide's
icons/ directory. The licence of each icon is decided by Lucide's own
LICENSE file: the icons Feather gave Lucide are MIT, the rest is ISC.
"""

import os
import sys
import urllib.request

# The icons the app draws, and the licence each of them carries.
#
# The MIT ones are the icons Feather (MIT) gave Lucide, under the names
# Lucide has since renamed: the "x-circle" of the list is today's
# "circle-x", and so on.
ICONS = {
    # ISC -- Lucide's own.
    "camera": "ISC",
    "clipboard-list": "ISC",
    "circle-stop": "ISC",
    "eye": "ISC",
    "eye-off": "ISC",
    "git-branch": "ISC",
    "keyboard": "ISC",
    "megaphone": "ISC",
    "mic-audio-lines": "ISC",
    "paperclip": "ISC",
    "pencil": "ISC",
    "send": "ISC",
    "settings": "ISC",
    # MIT -- Feather's, carried over by Lucide.
    "arrow-left": "MIT",
    "arrow-right": "MIT",
    "arrow-up": "MIT",
    "chevron-down": "MIT",
    "chevron-right": "MIT",
    "circle-x": "MIT",
    "info": "MIT",
    "plus": "MIT",
    "search": "MIT",
    "trash": "MIT",
    "x": "MIT",
}

SOURCE = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/{}.svg"

HEADERS = {
    "ISC": (
        "SPDX-FileCopyrightText: 2026 Lucide Icons and Contributors\n"
        "SPDX-License-Identifier: ISC"
    ),
    "MIT": (
        "SPDX-FileCopyrightText: 2013-present Cole Bemis\n"
        "SPDX-License-Identifier: MIT"
    ),
}


def fetch(name):
    """The icon's own file, as Lucide has it."""
    url = SOURCE.format(name)
    with urllib.request.urlopen(url) as response:
        return response.read().decode("utf-8")


def with_header(svg, licence):
    """The icon's file, with the licence header on top of it."""
    header = "<!--\n{}\n-->\n".format(HEADERS[licence])
    # The comment goes in front of the XML declaration, which has to stay
    # the first thing in the file -- so the declaration moves below it.
    if svg.startswith("<?xml"):
        declaration, _, rest = svg.partition("?>\n")
        return header + declaration + "?>\n" + rest
    return header + svg


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, licence in sorted(ICONS.items()):
        target = os.path.join(here, name + ".svg")
        svg = with_header(fetch(name), licence)
        with open(target, "w") as handle:
            handle.write(svg)
        print("wrote %s (%s)" % (name + ".svg", licence))
    return 0


if __name__ == "__main__":
    sys.exit(main())
