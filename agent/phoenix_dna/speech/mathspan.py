# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Math-span detection: the Python mirror of the renderer's matcher.

``theme/latexrenderer.cpp`` decides which parts of an answer are formulas
with ``mathSpanMatcher()``; the speech pipeline must find *exactly* the
same spans, or the daemon would speak different mathematics than the
window renders. The alternation below is a line-by-line port of that
matcher, and ``agent/tests/data/mathspans.json`` pins both sides to one
fixture so the two detectors cannot drift apart.

Ported rules (see the C++ comment):

* all span kinds in one alternation, display delimiters first — a
  ``$$`` can never be misread as two ``$`` spans;
* the inline ``$…$`` must hug its content, so ``$5 and $10`` stays
  currency.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

#: ``\[ … \]``
DISPLAY_BRACKET = "display_bracket"
#: ``$$ … $$``
DISPLAY_DOLLAR = "display_dollar"
#: ``\( … \)``
INLINE_PAREN = "inline_paren"
#: ``$ … $``
INLINE_DOLLAR = "inline_dollar"

#: Span kinds by capturing group of ``_SPAN_RE`` (1-based order).
_KINDS_BY_GROUP = (DISPLAY_BRACKET, DISPLAY_DOLLAR, INLINE_PAREN, INLINE_DOLLAR)

# Ordered alternation, ported from theme/latexrenderer.cpp:mathSpanMatcher.
# Python's ``re`` and QRegularExpression (PCRE) both take the leftmost
# alternative at each position, so the order carries the same meaning.
_SPAN_RE = re.compile(
    r"\\\[([\s\S]*?)\\\]"
    r"|\$\$([\s\S]*?)\$\$"
    r"|\\\((.+?)\\\)"
    r"|\$(\S(?:[^$\n]*\S)?)\$"
)


@dataclass(frozen=True)
class MathSpan:
    """One detected math span; ``start``/``end`` include the delimiters."""

    start: int
    end: int
    kind: str
    #: The span's content, between its delimiters.
    body: str

    @property
    def display(self) -> bool:
        """Whether the span is display math (its own paragraph)."""
        return self.kind in (DISPLAY_BRACKET, DISPLAY_DOLLAR)


def find_math_spans(text: str) -> list[MathSpan]:
    """All math spans in ``text``, left to right, never overlapping."""
    spans: list[MathSpan] = []
    for match in _SPAN_RE.finditer(text):
        for index, kind in enumerate(_KINDS_BY_GROUP):
            body = match.group(index + 1)
            if body is not None:
                spans.append(MathSpan(match.start(), match.end(), kind, body))
                break
    return spans
