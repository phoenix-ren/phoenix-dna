# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for LaTeX → spoken math (speech/latex_speech.py, mathspan.py).

The LaTeX table below mirrors the constructs the models actually emit
(§2b of the speech design); the Unicode half runs against the shared
fixture (agent/tests/data/mathspans.json) that the C++ renderer's test
(theme/mathspantest.cpp) also executes, so the spoken table and the
typeset table cannot drift apart.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from phoenix_dna.speech.latex_speech import (
    DISPLAY_SPOKEN,
    UNICODE_LATEX,
    spoken_math,
    spoken_span,
    unicode_spoken,
)
from phoenix_dna.speech.mathspan import find_math_spans

FIXTURE = Path(__file__).resolve().parents[1] / "tests" / "data" / "mathspans.json"


@pytest.mark.parametrize(
    ("latex", "spoken"),
    [
        (r"\frac{a}{b}", "a over b"),
        ("x^{2}", "x squared"),
        ("x^2", "x squared"),
        ("x^{10}", "x to the tenth"),
        ("x^{n}", "x to the n"),
        ("x_{i}", "x sub i"),
        ("x_i^2", "x sub i squared"),
        (r"\sqrt{x}", "the square root of x"),
        (r"\sqrt[3]{x}", "the cube root of x"),
        (r"\sqrt[4]{x}", "the fourth root of x"),
        (r"\sum_{i=1}^{n}", "the sum from i equals 1 to n of"),
        (r"\sum_{i=1}^{n} i", "the sum from i equals 1 to n of i"),
        (r"\int_a^b f(x)\,dx", "the integral from a to b of f of x, d x"),
        (r"\lim_{x \to 0} \frac{1}{x}", "the limit as x approaches 0 of 1 over x"),
        (r"\alpha + \beta", "alpha plus beta"),
        (r"2 \times 3", "2 times 3"),
        (r"a \cdot b", "a times b"),
        (r"a \le b", "a is less than or equal to b"),
        (r"a \ge b", "a is greater than or equal to b"),
        (r"a \neq b", "a is not equal to b"),
        (r"a \approx b", "a is approximately equal to b"),
        (r"x \to \infty", "x approaches infinity"),
        (r"\pm 1", "plus or minus 1"),
        (r"x \in \mathbb{R}", "x is in the reals"),
        (r"a \pm 2", "a plus or minus 2"),
        (r"f(x) = x^2 + 1", "f of x equals x squared plus 1"),
        (r"\text{for all } x", "for all x"),
        (r"\mathrm{d}x", "d x"),
        (r"\operatorname{Tr} M", "Tr M"),
        ("e^{i\\pi}", "e to the i pi"),
        (r"\sin(x)", "sine of x"),
        (r"\sin^{-1}", "arc sine"),
        (r"\log_2 n", "log sub 2 n"),
        (r"a < b", "a is less than b"),
        (r"n! = n \times (n-1)", "n factorial equals n times the quantity n minus 1"),
    ],
)
def test_latex_spoken_table(latex, spoken):
    assert spoken_math(latex) == spoken


@pytest.mark.parametrize(
    "latex",
    [
        r"\begin{aligned}x &= 1\end{aligned}",
        r"\begin{cases}1 & 2\end{cases}",
        "x \\\\ y",  # row break: derivation territory
        r"\unknowncommand{x}",
        "{",
        "}",
    ],
)
def test_unparseable_spans_drop_silently(latex):
    assert spoken_math(latex) == ""


def test_short_display_span_spoken_in_full():
    assert spoken_span("a + b", display=True) == "a plus b"


def test_long_display_span_replaced_by_marker():
    derivation = " + ".join(["x_{%d}" % i for i in range(12)])
    assert len(derivation) > 80
    assert spoken_span(derivation, display=True) == DISPLAY_SPOKEN


def test_inline_span_always_spoken_in_full():
    long_inline = " + ".join(["x_{%d}" % i for i in range(12)])
    assert spoken_span(long_inline, display=False) == spoken_math(long_inline)


def test_unicode_math_in_spans():
    assert spoken_math("α + β") == "alpha plus beta"
    assert spoken_math("x ≤ 5") == "x is less than or equal to 5"
    assert spoken_math("∫ f") == "the integral of f"


def test_mathspan_fixture_spans():
    """The shared fixture drives the Python detector (and the C++ one)."""
    cases = json.loads(FIXTURE.read_text())["spans"]
    assert cases, "fixture is missing"
    for case in cases:
        spans = find_math_spans(case["input"])
        got = [(s.start, s.end, s.kind, s.body) for s in spans]
        wanted = [
            (e["start"], e["end"], e["kind"], e["body"]) for e in case["expected"]
        ]
        assert got == wanted, case["input"]


def test_mathspan_fixture_unicode():
    """Every fixture symbol maps to the fixture's command and word."""
    table = json.loads(FIXTURE.read_text())["unicode"]
    assert len(table) == len(UNICODE_LATEX)
    spoken = unicode_spoken()
    for entry in table:
        symbol = entry["symbol"]
        assert UNICODE_LATEX[symbol] == "\\" + entry["latex"], symbol
        assert spoken[symbol] == entry["spoken"], symbol
