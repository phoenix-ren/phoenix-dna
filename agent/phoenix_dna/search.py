# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session search: fuzzy matching of the sessions' summary names.

The widget's session search lists the user's previous sessions on the
left and the selected session's queries and responses on the right.
Typing narrows the list down: the text is matched fuzzily against the
sessions' summary names — the title the daemon generated for a session,
or its first query when the session was never named — so "dt" finds
"Disk tuning" and every further character drops the sessions that no
longer match.

The match itself is a subsequence match: every character of the query has
to occur in the name, in order and regardless of case. The score rewards
tight and word-boundary matches, so the best match can be sorted to the
top of the list.
"""

from __future__ import annotations

#: Characters that start a new word inside a session name. A query
#: character landing on one of them is a much better match than one
#: buried in the middle of a word.
_WORD_SEPARATORS = " \t-_/.,:;()[]{}"

#: How long the summary name of an unnamed session may get before it is
#: cut off (the first query is shown instead of the title).
_SUMMARY_LENGTH = 80


def summary_name(title: str, first_query: str) -> str:
    """The name a session is listed under.

    The daemon's title, when the session has one; otherwise its first
    query, so a session that was never named is still findable.
    """
    title = (title or "").strip()
    if title:
        return title
    query = " ".join((first_query or "").split())
    return query[:_SUMMARY_LENGTH]


def fuzzy_score(query: str, text: str) -> int | None:
    """Score a fuzzy match of ``query`` against ``text``.

    A match means every character of the query occurs in the text, in
    order and regardless of case ("dt" matches "Disk tuning"). The score
    rewards tight and word-boundary matches, so the best match can be
    sorted to the top; ``None`` means the query does not match at all.
    """
    query = (query or "").strip()
    if not query:
        return 0

    haystack = (text or "").lower()
    needle = query.lower()
    score = 0
    position = 0
    for char in needle:
        index = haystack.find(char, position)
        if index < 0:
            return None
        if index == 0 and position == 0:
            score += 12  # the query starts the name
        elif index == position:
            score += 6  # consecutive characters
        elif index == 0 or haystack[index - 1] in _WORD_SEPARATORS:
            score += 5  # the start of a word
        else:
            score -= 2  # a gap: the characters sit further apart
        score += 1
        position = index + 1

    # A short name is a tighter match than a long one.
    score += max(0, 20 - len(haystack)) // 5
    return score


def match_sessions(query: str, sessions: list[dict]) -> list[dict]:
    """The sessions whose summary name matches the query, best match first.

    An empty query matches everything, in the order it came in (the
    store lists the sessions newest first).
    """
    query = (query or "").strip()
    if not query:
        return list(sessions)

    scored: list[tuple[int, dict]] = []
    for session in sessions:
        score = fuzzy_score(query, str(session.get("summary", "")))
        if score is None:
            continue
        scored.append((score, session))

    # Stable sort: sessions with the same score keep their order, so the
    # newest one stays on top.
    scored.sort(key=lambda item: item[0], reverse=True)
    return [session for _score, session in scored]
