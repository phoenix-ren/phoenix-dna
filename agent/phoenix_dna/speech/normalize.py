# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Turn raw answer Markdown into text worth hearing.

An answer the window renders is not an answer the ear wants: code blocks
are shown (not read), URLs are linked (not spelled), and the math is
compiled into images. This module rewrites the *same* text for speech:

* fenced code blocks and inline code spans are dropped;
* math spans are replaced by their spoken form *before* anything else,
  so the segmenter never has to reason about LaTeX;
* URLs are stripped to their domain name;
* list markers expand (a lone bullet becomes its item; a run of bullets
  is numbered, because a listener cannot see the structure);
* bold/italic markers, headings and blockquote bars dissolve;
* whitespace is normalized.

The guiding rule is conservatism: never mangle prose. When a construct
is not confidently recognized, the text is left exactly as it was — a
listener survives a stray asterisk better than a rewritten sentence.
"""

from __future__ import annotations

import logging
import re
from urllib.parse import urlparse

from .latex_speech import spoken_span, unicode_spoken
from .mathspan import find_math_spans

log = logging.getLogger("phoenix.speech.normalize")

#: Marks a replaced math span so the segmenter never splits inside it
#: (a spoken formula is one breath, not a sentence boundary hunt). The
#: private-use character cannot occur in an answer.
NO_SPLIT_MARK = "\ue000"

#: Fenced code blocks: split on the fence; even segments are prose,
#: odd ones code. An unterminated opener swallows the rest, which is
#: the streaming-safe reading (the tail is code-in-progress).
_FENCE = "```"

#: Inline code spans are unwrapped rather than elided: the span is part
#: of the sentence around it ("run `df -h` now"), and cutting the middle
#: out of a spoken sentence mangles it worse than reading a short
#: snippet would. Fenced blocks are dropped wholesale — they are code,
#: not speech.
_INLINE_CODE_RE = re.compile(r"`+([^`\n]*)`+")

_URL_RE = re.compile(r"\b(?:https?|ftp)://[^\s<>\"()\[\]]+")

_IMAGE_RE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_LINK_RE = re.compile(r"\[([^\]]+)\]\([^)]*\)")

_HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+")
_BLOCKQUOTE_RE = re.compile(r"^\s{0,3}>\s?")
_HORIZONTAL_RULE_RE = re.compile(r"^\s{0,3}(?:[-*_]\s*){3,}$")

_BOLD_RE = re.compile(r"\*\*(?=\S)(.+?)(?<=\S)\*\*")
_BOLD_UNDERSCORE_RE = re.compile(r"(?<![\w_])__(?=\S)(.+?)(?<=\S)__(?!\w)")
# Intraword underscores are identifiers (some_var_name), not emphasis —
# Markdown agrees, and mangling code names in prose is worse than an
# unemphasized word.
_ITALIC_RE = re.compile(r"(?<!\*)\*(?=\S)([^*\n]+?)(?<=\S)\*(?!\*)")
_ITALIC_UNDERSCORE_RE = re.compile(r"(?<![\w_])_(?=\S)([^_\n]+?)(?<=\S)_(?!\w)")
_STRIKE_RE = re.compile(r"~~(?=\S)(.+?)(?<=\S)~~")

_LIST_MARKER_RE = re.compile(r"^(\s*)([-*+]|\d{1,3}[.)])\s+")

# Left over after span replacement: delimiters of spans that did not
# parse, or that arrived malformed. They must not be read aloud.
_LEFTOVER_DELIMS = ("$$", "\\[", "\\]", "\\(", "\\)")
_LEFTOVER_COMMAND_RE = re.compile(r"\\(?:begin|end|item)\b\s*(?:\{[^}]*\})?")


def normalize(text: str) -> str:
    """Answer Markdown as clean speakable text (no segmentation marks)."""
    return strip_marks(normalize_marked(text))


def normalize_marked(text: str) -> str:
    """Answer Markdown as speakable text, math spans wrapped in marks.

    The :data:`NO_SPLIT_MARK` pairs tell the segmenter where a replaced
    math span begins and ends, so a spoken formula is never cut in half
    by a period inside it. Feed the result to :func:`segment`, which
    strips the marks again.
    """
    text = _drop_fenced_code(text)
    text = _drop_inline_code(text)
    text = _replace_math_spans(text)
    text = _map_unicode_math(text)
    text = _drop_leftover_latex(text)
    text = _strip_urls(text)
    text = _strip_markdown_marks(text)
    text = _expand_list_markers(text)
    return _clean_whitespace(text)


def strip_marks(text: str) -> str:
    """Remove the no-split marks; the words inside stay untouched."""
    return text.replace(NO_SPLIT_MARK, "")


# --------------------------------------------------------------------------


def _drop_fenced_code(text: str) -> str:
    parts = text.split(_FENCE)
    if len(parts) == 1:
        return text
    return "".join(parts[0::2])


def _drop_inline_code(text: str) -> str:
    return _INLINE_CODE_RE.sub(r"\1", text)


def _replace_math_spans(text: str) -> str:
    """Swap every math span for its spoken form, marks around it.

    Spans that do not parse are dropped silently ("" replacement): the
    surrounding prose still carries the meaning, and the window's
    rendered image carries the detail.
    """
    spans = find_math_spans(text)
    if not spans:
        return text
    out: list[str] = []
    cursor = 0
    for span in spans:
        spoken = spoken_span(span.body, display=span.display)
        out.append(text[cursor : span.start])
        if spoken:
            out.append(NO_SPLIT_MARK + spoken + NO_SPLIT_MARK)
        else:
            out.append(" ")
        cursor = span.end
    out.append(text[cursor:])
    return "".join(out)


def _map_unicode_math(text: str) -> str:
    """Bare Unicode math outside spans (∫, α, ≤ …) becomes words.

    The renderer typesets these symbols; speech says them. Symbols not
    in the shared table are left alone.
    """
    table = unicode_spoken()
    for symbol, word in table.items():
        if symbol in text:
            text = text.replace(symbol, f" {word} ")
    return text


def _drop_leftover_latex(text: str) -> str:
    for delimiter in _LEFTOVER_DELIMS:
        text = text.replace(delimiter, " ")
    return _LEFTOVER_COMMAND_RE.sub(" ", text)


def _strip_urls(text: str) -> str:
    """https://example.com/a/b?q=1 → example.com (trailing punctuation kept)."""

    def replace(match: re.Match) -> str:
        url = match.group(0)
        trailing = ""
        while url and url[-1] in ".,;:!?)":
            trailing = url[-1] + trailing
            url = url[:-1]
        domain = urlparse(url).netloc or url
        return domain + trailing

    return _URL_RE.sub(replace, text)


def _strip_markdown_marks(text: str) -> str:
    text = _IMAGE_RE.sub(" ", text)
    text = _LINK_RE.sub(r"\1", text)
    lines: list[str] = []
    for line in text.splitlines():
        if _HORIZONTAL_RULE_RE.match(line):
            line = ""
        line = _HEADING_RE.sub("", line)
        line = _BLOCKQUOTE_RE.sub("", line)
        lines.append(line)
    text = "\n".join(lines)
    text = _BOLD_RE.sub(r"\1", text)
    text = _BOLD_UNDERSCORE_RE.sub(r"\1", text)
    text = _ITALIC_RE.sub(r"\1", text)
    text = _ITALIC_UNDERSCORE_RE.sub(r"\1", text)
    text = _STRIKE_RE.sub(r"\1", text)
    return text


def _expand_list_markers(text: str) -> str:
    """Expand list markers; number runs of bullets.

    A single bullet is just an introduction; a run of them is structure
    the eye sees and the ear needs spelled out, so the items are
    numbered ("1. … 2. …"). Ordered markers keep the number the author
    wrote — renumbering an intentionally sparse list would change it.
    """
    lines = text.splitlines()
    out: list[str] = []
    run: list[int] = []  # indices into `out` of the current bullet run
    for line in lines:
        marker = _LIST_MARKER_RE.match(line)
        if marker is None:
            run = []
            out.append(line)
            continue
        indent, symbol = marker.group(1), marker.group(2)
        content = line[marker.end() :]
        if symbol in "-*+":
            out.append(f"{indent}{content}")
            run.append(len(out) - 1)
        else:
            run = []
            out.append(f"{indent}{symbol} {content}")
    if len(run) > 1:
        for number, index in enumerate(run, start=1):
            out[index] = f"{number}. {out[index].lstrip()}"
    return "\n".join(out)


def _clean_whitespace(text: str) -> str:
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r" ?\n ?", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()
