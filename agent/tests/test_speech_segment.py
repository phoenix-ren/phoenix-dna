# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for sentence segmentation (speech/segment.py)."""

from __future__ import annotations

from phoenix_dna.speech.normalize import NO_SPLIT_MARK, strip_marks
from phoenix_dna.speech.segment import segment


def test_simple_sentences_split():
    text = (
        "The cache was completely and utterly full of old junk. "
        "I removed the stale kernels and caches. Space is finally back."
    )
    # The first piece stands alone (>= minimum); the two short ones merge.
    chunks = segment(text)
    assert len(chunks) == 2
    assert chunks[0] == "The cache was completely and utterly full of old junk."
    assert chunks[1] == "I removed the stale kernels and caches. Space is finally back."


def test_question_and_exclamation_split():
    text = "Is the disk full? No! It is only the caches growing again today."
    chunks = segment(text)
    assert any(c.startswith("Is the disk full?") for c in chunks)


def test_abbreviation_not_split():
    text = "Use e.g. the df tool. Then check the inode usage very carefully."
    chunks = segment(text)
    assert any("e.g. the df tool." in c for c in chunks)


def test_single_initial_not_split():
    text = "As J. Smith wrote, the answer is 42 and the question is deep."
    chunks = segment(text)
    assert any("J. Smith" in c for c in chunks)


def test_decimal_not_split():
    text = "Load average is 3.14 which is high, but memory sits at 40% used."
    chunks = segment(text)
    assert any("3.14" in c for c in chunks)


def test_numbered_list_item_not_split():
    text = "1. install the package. 2. reboot the machine. 3. enjoy the silence."
    chunks = segment(text)
    assert any(c.startswith("1. install") for c in chunks)


def test_ellipsis_not_split():
    text = "It almost worked… the rest of the sentence continues after it."
    chunks = segment(text)
    assert any("worked" in c and "continues" in c for c in chunks)


def test_no_split_inside_replaced_math_span():
    spoken = NO_SPLIT_MARK + "the integral from 0 to 1 of f of x, d x" + NO_SPLIT_MARK
    text = f"Compute {spoken} now. Then check the result against the table."
    chunks = segment(text)
    for chunk in chunks:
        assert NO_SPLIT_MARK not in chunk
    assert any("the integral from 0 to 1" in c for c in chunks)


def test_minimum_length_merges_short_pieces():
    text = "Yes. Fine. " + "This final sentence is comfortably long enough on its own."
    chunks = segment(text)
    assert len(chunks) == 1


def test_maximum_length_hard_splits():
    sentence = "word " * 120  # 600 chars, no punctuation
    chunks = segment(sentence.strip(), min_length=1)
    assert all(len(c) <= 300 for c in chunks)
    assert len(chunks) >= 2
    # nothing lost
    assert " ".join(chunks).split() == sentence.split()


def test_paragraph_break_is_a_boundary():
    text = "First paragraph ends here.\n\nSecond paragraph starts afresh now."
    chunks = segment(text)
    assert len(chunks) == 2
    assert chunks[0] == "First paragraph ends here."


def test_empty_and_whitespace_only():
    assert segment("") == []
    assert segment("   \n\n  ") == []


def test_marks_stripped_from_output():
    marked = NO_SPLIT_MARK + "x squared" + NO_SPLIT_MARK
    chunks = segment(f"Consider {marked} here and think about it for a while.")
    assert all(NO_SPLIT_MARK not in c for c in chunks)
    assert any("x squared" in c for c in chunks)


def test_strip_marks_leaves_content():
    assert strip_marks(NO_SPLIT_MARK + "a over b" + NO_SPLIT_MARK) == "a over b"
