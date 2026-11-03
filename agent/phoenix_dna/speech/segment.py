# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Sentence segmentation for speech: chunks neither choppy nor huge.

The synthesis API takes a piece of text and returns its audio, so the
answer is cut at sentence boundaries — but not blindly:

* a boundary is terminal punctuation (``.!?``) followed by whitespace;
* never inside an abbreviation ("e.g."), a decimal ("3.14"), an
  ellipsis ("wait…"), a numbered list item ("2. and then"), or a
  replaced math span (marked by :data:`normalize.NO_SPLIT_MARK` — a
  spoken formula is one breath);
* chunks are merged up to a minimum length and hard-split at a maximum,
  so the daemon neither pays one request per three words nor waits for
  a whole essay before the first sound.
"""

from __future__ import annotations

import re

from .normalize import NO_SPLIT_MARK, strip_marks

DEFAULT_MIN_LENGTH = 40
DEFAULT_MAX_LENGTH = 300

#: Words whose period is not a sentence end. Kept deliberately small:
#: each entry is a word the models write in answers.
_ABBREVIATIONS = frozenset(
    {
        "e.g",
        "i.e",
        "etc",
        "vs",
        "cf",
        "ca",
        "approx",
        "min",
        "max",
        "avg",
        "Dr",
        "Mr",
        "Mrs",
        "Ms",
        "Mx",
        "Prof",
        "St",
        "Sr",
        "Jr",
        "Fig",
        "Figs",
        "Eq",
        "Eqs",
        "No",
        "Nos",
        "Ref",
        "Refs",
        "Vol",
        "Ch",
        "Sec",
        "Sect",
        "dept",
        "univ",
        "inc",
        "ltd",
        "co",
        "corp",
    }
)

_TERMINALS = ".!?"
_WORD_BEFORE_RE = re.compile(r"[A-Za-z.]*$")
_ELLIPSIS = "…"


def segment(
    text: str,
    *,
    min_length: int = DEFAULT_MIN_LENGTH,
    max_length: int = DEFAULT_MAX_LENGTH,
) -> list[str]:
    """Speakable chunks of ``text`` (which may carry no-split marks)."""
    pieces = _candidate_pieces(text)
    pieces = _merge_short(pieces, min_length)
    chunks: list[str] = []
    for piece in pieces:
        chunks.extend(_split_long(piece, max_length))
    out = []
    for chunk in chunks:
        cleaned = strip_marks(_squeeze(chunk))
        if cleaned:
            out.append(cleaned)
    return out


# --------------------------------------------------------------------------
# Candidate pieces are (text, hard): "hard" marks a paragraph break, a
# boundary the merger must respect — short paragraphs read as
# paragraphs, not as scraps glued onto their neighbours.


def _candidate_pieces(text: str) -> list[tuple[str, bool]]:
    """Split at safe sentence boundaries and paragraph breaks."""
    pieces: list[list] = []  # [text, hard] — mutable for back-marking

    def emit(piece: str, hard: bool) -> None:
        if piece.strip():
            pieces.append([piece, hard])

    start = 0
    in_mark = False
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char == NO_SPLIT_MARK:
            in_mark = not in_mark
            index += 1
            continue
        if in_mark:
            index += 1
            continue
        if char == "\n":
            # A paragraph break (after normalization: two or more
            # newlines) ends the piece even without punctuation (lists,
            # headings); a single newline is just a soft wrap.
            run_end = index
            while run_end < length and text[run_end] == "\n":
                run_end += 1
            if run_end - index >= 2:
                pending = text[start:index]
                if pending.strip():
                    emit(pending, True)
                elif pieces:
                    # The sentence boundary already emitted this piece;
                    # the blank line hardens the boundary behind it.
                    pieces[-1][1] = True
                while run_end < length and text[run_end] in " \t":
                    run_end += 1
                start = index = run_end
                continue
            index += 1
            continue
        if char in _TERMINALS and _is_boundary(text, index):
            end = index + 1
            # Consume a run of closing quotes/brackets (he said".).
            while end < length and text[end] in "\"')]”’":
                end += 1
            if end == length or text[end] in " \t\n":
                emit(text[start:end], False)
                index = end
                while index < length and text[index] in " \t":
                    index += 1
                start = index
                continue
        index += 1
    emit(text[start:], False)
    return [(piece, hard) for piece, hard in pieces]


def _is_boundary(text: str, index: int) -> bool:
    """Whether the terminal at ``index`` really ends a sentence."""
    # Inside a run of terminals ("...", "!?", "wait…"): not a boundary —
    # the sentence is still going (or has not convincingly ended).
    if index > 0 and text[index - 1] in _TERMINALS + _ELLIPSIS:
        return False
    if index + 1 < len(text) and text[index + 1] in _TERMINALS + _ELLIPSIS:
        return False
    if text[index] == "." and _ELLIPSIS in text[max(0, index - 2) : index + 2]:
        return False
    # Decimals and numbered items: "3.14" has no space, but "2. Next…"
    # (a list) and "version 3. 5" must not split either.
    if index > 0 and text[index - 1].isdigit():
        return False
    previous = text[:index]
    word_match = _WORD_BEFORE_RE.search(previous)
    word = word_match.group(0) if word_match else ""
    if word.lower() in _ABBREVIATIONS:
        return False
    # An initial ("J. R. R. Tolkien") is not a sentence end.
    if len(word) == 1 and word.isupper():
        return False
    # A period that flows into a lowercase word reads as a continuation.
    if text[index] == ".":
        rest = text[index + 1 :].lstrip(" \t\n")
        if rest[:1].islower():
            return False
    return True


def _merge_short(pieces: list[tuple[str, bool]], min_length: int) -> list[str]:
    """Greedily join pieces until each reaches the minimum length.

    Joining never crosses a paragraph break: a short paragraph is still
    a paragraph, and gluing it onto the next one would erase the pause
    that carries the structure. A short *tail* of a paragraph may still
    ride along with the piece before it (soft boundary only).
    """
    if min_length <= 0:
        return [piece for piece, _hard in pieces]
    merged: list[str] = []
    absorbable: list[bool] = []  # per emitted chunk: soft boundary after?
    buffer = ""
    for piece, hard in pieces:
        buffer = f"{buffer} {piece}" if buffer else piece
        if hard or len(strip_marks(buffer)) >= min_length:
            merged.append(buffer)
            absorbable.append(not hard)
            buffer = ""
    if buffer:
        if merged and absorbable[-1]:
            merged[-1] = f"{merged[-1]} {buffer}"
        else:
            merged.append(buffer)
    return merged


def _split_long(piece: str, max_length: int) -> list[str]:
    """Split an oversized piece at its last comma/colon/space below max."""
    if len(strip_marks(piece)) <= max_length:
        return [piece]
    cut = _secondary_boundary(piece, max_length)
    if cut <= 0:
        return [piece]
    head = piece[:cut].rstrip()
    rest = piece[cut:].lstrip()
    # Neither half may strand an open no-split mark.
    if head.count(NO_SPLIT_MARK) % 2 or rest.count(NO_SPLIT_MARK) % 2:
        return [piece]
    return _split_long(head, max_length) + _split_long(rest, max_length)


def _secondary_boundary(piece: str, max_length: int) -> int:
    """The latest comma/semicolon/colon/space at or below the limit."""
    window = piece[:max_length]
    best = -1
    in_mark = False
    previous = ""
    for index, char in enumerate(window):
        if char == NO_SPLIT_MARK:
            in_mark = not in_mark
            previous = char
            continue
        if in_mark:
            previous = char
            continue
        if char in ",;:":
            # "1,000" is a number, not a boundary.
            if previous.isdigit() and piece[index + 1 : index + 2].isdigit():
                previous = char
                continue
            best = index + 1
        elif char == " ":
            best = index
        previous = char
    return best


def _squeeze(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()
