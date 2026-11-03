# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for answer Markdown → speakable text (speech/normalize.py)."""

from __future__ import annotations

from phoenix_dna.speech.normalize import (
    NO_SPLIT_MARK,
    normalize,
    normalize_marked,
    strip_marks,
)


def test_fenced_code_block_dropped():
    text = "Here is how.\n```python\ndef f():\n    return 1\n```\nThat is how."
    assert "def f" not in normalize(text)
    assert "return 1" not in normalize(text)
    assert "Here is how." in normalize(text)
    assert "That is how." in normalize(text)


def test_unterminated_fence_swallows_the_rest():
    # Streaming-safe: an opener without a closer is code in progress.
    text = "answer\n```bash\ndf -h"
    assert "df -h" not in normalize(text)
    assert "answer" in normalize(text)


def test_inline_code_dropped():
    assert normalize("Set `ENABLE_FOO = 1` first.") == "Set ENABLE_FOO = 1 first."


def test_urls_stripped_to_domain():
    text = "See https://example.com/docs/page?q=1 for details."
    assert normalize(text) == "See example.com for details."


def test_url_keeps_trailing_punctuation():
    text = "Read the manual at https://docs.example.com/setup."
    assert normalize(text) == "Read the manual at docs.example.com."


def test_single_bullet_becomes_its_item():
    assert normalize("- just this one") == "just this one"


def test_bullet_run_is_numbered():
    text = "- first item\n- second item\n- third item"
    assert normalize(text) == "1. first item\n2. second item\n3. third item"


def test_ordered_markers_keep_their_numbers():
    text = "3. third\n4. fourth"
    assert "3. third" in normalize(text)
    assert "4. fourth" in normalize(text)


def test_bold_and_italic_markers_expand():
    assert normalize("**important** point") == "important point"
    assert normalize("*emphasis* holds") == "emphasis holds"
    assert normalize("_underline_ holds") == "underline holds"


def test_intraword_underscores_survive():
    # some_var_name is an identifier, not three emphasized fragments.
    assert normalize("rename some_var_name here") == "rename some_var_name here"


def test_headings_and_quotes_strip():
    assert normalize("## A heading") == "A heading"
    assert normalize("> quoted text") == "quoted text"


def test_math_span_becomes_spoken_form():
    text = "The area is $\\pi r^2$ exactly."
    assert normalize(text) == "The area is pi r squared exactly."


def test_math_spans_are_marked_for_the_segmenter():
    marked = normalize_marked("Value $x.y$ here.")
    assert NO_SPLIT_MARK in marked
    assert strip_marks(marked) == "Value x dot y here."


def test_unparseable_span_dropped_silently():
    text = "It holds $$\\begin{aligned}a &= 1\\\\b &= 2\\end{aligned}$$ mostly."
    cleaned = normalize(text)
    assert "begin" not in cleaned
    assert "aligned" not in cleaned
    assert "$$" not in cleaned
    assert "It holds" in cleaned


def test_bare_unicode_math_becomes_words():
    assert normalize("x ≤ 5 and x ≥ 1") == (
        "x is less than or equal to 5 and x is greater than or equal to 1"
    )


def test_currency_is_not_math():
    assert normalize("It costs $5 and $10.") == "It costs $5 and $10."


def test_markdown_links_keep_label_not_url():
    assert normalize("See [the docs](https://example.com/x).") == "See the docs."


def test_whitespace_collapsed():
    assert normalize("a\n\n\n\nb") == "a\n\nb"
    assert normalize("a   b\t\tc") == "a b c"


def test_prose_left_alone():
    plain = "The disk was 80% full, so I cleaned the caches in /var."
    assert normalize(plain) == plain
