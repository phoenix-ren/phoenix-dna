#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Draw the Phoenix DNA mark.

The mark is a DNA double helix that rises into the flame of a phoenix:
two ribbons weave over and under each other, carrying the base pairs
between them, and meet in a single point at the top -- the flame the
phoenix is made of. The helix below it is the life the flame is reborn
from.

Nothing here is hand-plotted: the strands are the two curves of a helix
and everything else follows from them, so the mark can be re-tuned --
the weave, the thickness, the colours -- by editing the constants below
and running this script again.

    python3 icons/make-icon.py

writes

    icons/phoenix-dna.svg        the mark, on transparent ground,
    icons/phoenix-dna-avatar.svg the mark on a rounded tile, and
    icons/phoenix-dna-avatar.png the tile as a picture, for the places
                                 that want one (the PNG needs rsvg-convert,
                                 and is left alone if it is not installed).
"""

import math
import os
import shutil
import subprocess
import sys

# ---------------------------------------------------------------------------
# The geometry of the helix
# ---------------------------------------------------------------------------

SIZE = 128.0  # the canvas, in the SVG's user units
CENTRE_X = 63.6  # the axis the helix turns around
TOP_Y = 12.0  # where the two strands meet: the tip of the flame
BOTTOM_Y = 116.0  # where the strands end: the two tails

AMPLITUDE = 35.0  # how far the strands swing out from the axis
TURNS = 1.25  # half-turns of the helix from the bottom to the top
LEAN = 6.0  # how far the tip leans out of the axis: a flame licks sideways

# The amplitude and the thickness both swell in the middle of the helix
# and taper towards the ends -- towards a point at the top, which is what
# makes the two strands meet in a flame rather than a fork.
AMPLITUDE_BASE = 0.10  # the spread of the two tails at the bottom
SHAPE = 0.7  # how fast the strands close in towards the ends
WIDTH = 12.0  # the thickest a strand gets
WIDTH_BASE = 0.05  # the thickness of the tails at the bottom

RUNG_WIDTH = 5.0  # the base pairs
RUNG_PARAMETERS = (0.04, 0.40, 0.78)  # where along the helix they sit

WEAVE_GAP = 1.4  # the daylight between a strand and the one it crosses
WEAVE_SPAN = 0.055  # how much of the strand the gap is cut out of

SAMPLES = 160  # the points the curves are sampled at

# ---------------------------------------------------------------------------
# The colours
# ---------------------------------------------------------------------------

# Both strands of a helix are the same molecule, so they share a palette;
# what tells them apart is the weave -- the gap where one passes over the
# other. The fire runs from a deep ember at the bottom to a pale gold at
# the tip, and the base pairs are the pale part of it.
STRAND_COLOURS = ("#E8380D", "#FF7A18", "#FFC24B")
RUNG_COLOUR = "#FFF3D6"
RUNG_ALPHA = "0.92"

# The ground the mark stands on in the avatar: a deep, warm night.
TILE_COLOURS = ("#2A0E1E", "#12060E")
TILE_RADIUS = 0.22  # the tile's corner radius, as a fraction of its side
TILE_SCALE = 0.8  # how much of the tile the mark takes up
TILE_LIFT = 2.5  # how much higher than the middle the mark sits: the tip
# of the flame is lighter than the base, so the mark's own centre of
# gravity is above the middle of the shape
TILE_GLOW = ("#FF8A3D", 0.22)  # the light the flame throws on the tile


# ---------------------------------------------------------------------------
# The curves
# ---------------------------------------------------------------------------


def amplitude(t):
    """How far the strands stand out from the axis at parameter t."""
    return AMPLITUDE * math.sin(math.pi * (AMPLITUDE_BASE + (1.0 - AMPLITUDE_BASE) * t)) ** SHAPE


def phase(t):
    """The angle the helix has turned to at parameter t."""
    return 2.0 * math.pi * TURNS * t - 0.5 * math.pi


def lean(t):
    """How far the strand is out of the axis: the flame leans as it rises."""
    return LEAN * t ** 3


def centre_point(t, strand):
    """The middle of a strand at parameter t (0 = bottom, 1 = top)."""
    swing = math.sin(phase(t))
    if strand == 1:
        swing = -swing
    x = CENTRE_X + amplitude(t) * swing + lean(t)
    y = BOTTOM_Y - (BOTTOM_Y - TOP_Y) * t
    return x, y


def strand_width(t):
    """The thickness of a strand at parameter t."""
    return WIDTH * math.sin(math.pi * (WIDTH_BASE + (1.0 - WIDTH_BASE) * t)) ** 0.7


def crossings():
    """Where the two strands pass each other, and which one is on top.

    The strand that is moving to the right as it crosses is the one drawn
    over the other, which is what makes the helix look woven rather than
    two curves printed on top of each other.
    """
    found = []
    for k in range(-4, 5):
        t = (k + 0.5) / (2.0 * TURNS)
        if not 0.0 < t < 1.0:
            continue
        # cos(phase) is +1 on even crossings and -1 on odd ones, and it
        # says which way the strand is travelling through the crossing.
        over = 0 if math.cos(k * math.pi) > 0 else 1
        found.append((t, over))
    return found


# ---------------------------------------------------------------------------
# Turning points into paths
# ---------------------------------------------------------------------------


def outline(points, widths):
    """The outline of a strand: its middle line pushed out to both sides."""
    count = len(points)
    left = []
    right = []
    for i, (x, y) in enumerate(points):
        if i == 0:
            dx = points[1][0] - x
            dy = points[1][1] - y
        elif i == count - 1:
            dx = x - points[i - 1][0]
            dy = y - points[i - 1][1]
        else:
            dx = points[i + 1][0] - points[i - 1][0]
            dy = points[i + 1][1] - points[i - 1][1]
        length = math.hypot(dx, dy) or 1.0
        nx = -dy / length
        ny = dx / length
        half = widths[i] / 2.0
        left.append((x + nx * half, y + ny * half))
        right.append((x - nx * half, y - ny * half))
    return left + right[::-1]


def closed_path(points, corners=()):
    """A closed, smooth path through the points.

    The points marked as corners keep their angle: the tip of the flame
    has to stay sharp, and a spline would round it off.
    """
    count = len(points)

    def at(i):
        return points[i % count]

    out = ["M %.3f %.3f" % points[0]]
    for i in range(count):
        p0 = at(i - 1)
        p1 = at(i)
        p2 = at(i + 1)
        p3 = at(i + 2)
        c1 = (p1[0] + (p2[0] - p0[0]) / 6.0, p1[1] + (p2[1] - p0[1]) / 6.0)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6.0, p2[1] - (p3[1] - p1[1]) / 6.0)
        if i in corners:
            c1 = p1
        if (i + 1) in corners:
            c2 = p2
        out.append(
            "C %.3f %.3f %.3f %.3f %.3f %.3f"
            % (c1[0], c1[1], c2[0], c2[1], p2[0], p2[1])
        )
    out.append("Z")
    return " ".join(out)


def open_path(points):
    """A smooth open path through the points."""
    count = len(points)
    out = ["M %.3f %.3f" % points[0]]
    for i in range(count - 1):
        p0 = points[max(i - 1, 0)]
        p1 = points[i]
        p2 = points[i + 1]
        p3 = points[min(i + 2, count - 1)]
        c1 = (p1[0] + (p2[0] - p0[0]) / 6.0, p1[1] + (p2[1] - p0[1]) / 6.0)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6.0, p2[1] - (p3[1] - p1[1]) / 6.0)
        out.append(
            "C %.3f %.3f %.3f %.3f %.3f %.3f"
            % (c1[0], c1[1], c2[0], c2[1], p2[0], p2[1])
        )
    return " ".join(out)


# ---------------------------------------------------------------------------
# The mark
# ---------------------------------------------------------------------------


def strand_outline(strand):
    """The outline of a strand, as points."""
    samples = [centre_point(i / SAMPLES, strand) for i in range(SAMPLES + 1)]
    widths = [strand_width(i / SAMPLES) for i in range(SAMPLES + 1)]
    return outline(samples, widths)


def gap_band(strand, crossing):
    """The middle line of the strand around a crossing, as an open path."""
    steps = 24
    points = []
    for i in range(steps + 1):
        t = crossing - WEAVE_SPAN + (2.0 * WEAVE_SPAN) * i / steps
        points.append(centre_point(t, strand))
    return open_path(points)


def build_mark():
    """The mark itself, as a piece of SVG."""
    outlines = {strand: strand_outline(strand) for strand in (0, 1)}

    # The tip of the flame is where the two outlines meet: the point where
    # the strand reaches the top. It has to stay a corner.
    corners = {}
    for strand in (0, 1):
        points = outlines[strand]
        tip = min(range(len(points)), key=lambda i: points[i][1])
        corners[strand] = {tip}

    # Which strand passes under which: the one that is on top at a
    # crossing is not interrupted, the other one is cut open around it.
    masks = {0: [], 1: []}
    for t, top_strand in crossings():
        masks[1 - top_strand].append((top_strand, t))

    defs = []
    for strand in (0, 1):
        if not masks[strand]:
            continue
        parts = []
        for other, t in masks[strand]:
            band = gap_band(other, t)
            width = strand_width(t) + 2.0 * WEAVE_GAP
            parts.append(
                '<path d="%s" fill="none" stroke="#000" stroke-width="%.3f" '
                'stroke-linecap="round"/>' % (band, width)
            )
        defs.append(
            '<mask id="weave-%d" maskUnits="userSpaceOnUse" x="0" y="0" '
            'width="%g" height="%g">'
            '<rect x="0" y="0" width="%g" height="%g" fill="#fff"/>%s</mask>'
            % (strand, SIZE, SIZE, SIZE, SIZE, "".join(parts))
        )

    # The base pairs, drawn first: the strands lie over them.
    rungs = []
    for t in RUNG_PARAMETERS:
        x0, y0 = centre_point(t, 0)
        x1, y1 = centre_point(t, 1)
        rungs.append(
            '<path d="M %.3f %.3f L %.3f %.3f" stroke="%s" stroke-opacity="%s" '
            'stroke-width="%g" stroke-linecap="round" fill="none"/>'
            % (x0, y0, x1, y1, RUNG_COLOUR, RUNG_ALPHA, RUNG_WIDTH)
        )

    strands = []
    for strand in (0, 1):
        mask = ' mask="url(#weave-%d)"' % strand if masks[strand] else ""
        strands.append(
            '<path d="%s" fill="url(#flame-%d)"%s/>'
            % (closed_path(outlines[strand], corners[strand]), strand, mask)
        )

    # The gradient is laid out in the canvas' own coordinates, not in the
    # bounding box of the strand that carries it: the two strands have to
    # carry the very same fire, or the place where they cross would show
    # a seam.
    stops = "".join(
        '<stop offset="%g" stop-color="%s"/>' % (offset, colour)
        for offset, colour in zip((0.0, 0.55, 1.0), STRAND_COLOURS)
    )
    gradients = [
        '<linearGradient id="flame-%d" gradientUnits="userSpaceOnUse" '
        'x1="0" y1="%g" x2="0" y2="%g">%s</linearGradient>'
        % (strand, BOTTOM_Y, TOP_Y, stops)
        for strand in (0, 1)
    ]

    return "".join(gradients) + "".join(defs) + "".join(rungs) + "".join(strands)


def render_mark(tile=False):
    """The whole SVG, on the tile if asked for."""
    body = build_mark()

    head = ""
    if tile:
        middle = SIZE / 2.0
        body = (
            '<g transform="translate(%.3f %.3f) scale(%.4f) translate(%.3f %.3f)">'
            "%s</g>"
            % (middle, middle - TILE_LIFT, TILE_SCALE, -middle, -middle, body)
        )
        head = (
            '<linearGradient id="ground" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s"/></linearGradient>'
            '<radialGradient id="glow" cx="50%%" cy="46%%" r="50%%">'
            '<stop offset="0" stop-color="%s" stop-opacity="%g"/>'
            '<stop offset="0.5" stop-color="%s" stop-opacity="%g"/>'
            '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
            '<rect x="0" y="0" width="%g" height="%g" rx="%.3f" '
            'fill="url(#ground)"/>'
            '<circle cx="%g" cy="%g" r="%g" fill="url(#glow)"/>'
            % (
                TILE_COLOURS[0],
                TILE_COLOURS[1],
                TILE_GLOW[0],
                TILE_GLOW[1],
                TILE_GLOW[0],
                TILE_GLOW[1] * 0.35,
                TILE_GLOW[0],
                SIZE,
                SIZE,
                SIZE * TILE_RADIUS,
                middle,
                middle - TILE_LIFT,
                SIZE * 0.62,
            )
        )

    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "%s"
        '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" '
        'viewBox="0 0 128 128">\n%s\n%s\n</svg>\n' % (svg_header(), head, body)
    )


def svg_header():
    """The header of this file, as the SVG's own.

    The picture is drawn by this script, so it carries the same licence
    header as the script: the comment block at the top of this file, read
    back out of it.
    """
    with open(os.path.abspath(__file__)) as handle:
        lines = handle.read().splitlines()

    block = []
    for line in lines:
        if not line.startswith("#"):
            break
        text = line.lstrip("#").strip()
        if not text.startswith("SPDX-"):
            continue
        block.append(text)

    return "<!--\n%s\n-->\n" % "\n".join(block)


def render_png(source, target, size=512):
    """The picture as a PNG, if there is a renderer to do it with."""
    if not shutil.which("rsvg-convert"):
        print("rsvg-convert is not installed: %s is left alone" % target)
        return False
    subprocess.run(
        ["rsvg-convert", "-w", str(size), "-h", str(size), source, "-o", target],
        check=True,
    )
    return True


def main():
    here = os.path.dirname(os.path.abspath(__file__))

    mark = os.path.join(here, "phoenix-dna.svg")
    avatar = os.path.join(here, "phoenix-dna-avatar.svg")
    with open(mark, "w") as handle:
        handle.write(render_mark())
    with open(avatar, "w") as handle:
        handle.write(render_mark(tile=True))

    render_png(avatar, os.path.join(here, "phoenix-dna-avatar.png"))

    print("wrote %s and %s" % (os.path.basename(mark), os.path.basename(avatar)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
