# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session search: fuzzy matching of the sessions' summary names."""

from __future__ import annotations

import pytest
from conftest import wait_for

from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.provider import FakeProvider
from phoenix_dna.search import fuzzy_score, match_sessions, summary_name
from phoenix_dna.sessions import SessionStore, valid_user


def _session(id, title="", first_query=""):
    return {
        "id": id,
        "mode": "user",
        "title": title,
        "summary": summary_name(title, first_query),
        "preview": first_query,
        "createdAt": "2026-01-01T00:00:00+00:00",
        "updatedAt": "2026-01-01T00:00:00+00:00",
        "messageCount": 2,
    }


# -- the fuzzy match ---------------------------------------------------------


def test_fuzzy_score_matches_a_subsequence():
    # Every character of the query occurs in the name, in order.
    assert fuzzy_score("dt", "Disk tuning") is not None
    assert fuzzy_score("disk", "Disk tuning") is not None
    assert fuzzy_score("TUN", "Disk tuning") is not None


def test_fuzzy_score_ignores_case_and_surrounding_space():
    assert fuzzy_score("  DISK ", "Disk tuning") == fuzzy_score("disk", "Disk tuning")


def test_fuzzy_score_rejects_a_missing_character():
    assert fuzzy_score("dz", "Disk tuning") is None
    # The characters have to be in order, too.
    assert fuzzy_score("td", "Disk tuning") is None


def test_fuzzy_score_of_an_empty_query_matches_everything():
    assert fuzzy_score("", "Disk tuning") == 0
    assert fuzzy_score("   ", "Disk tuning") == 0


def test_fuzzy_score_prefers_tight_matches():
    # The query starts the name: the best possible match.
    prefix = fuzzy_score("disk", "Disk tuning")
    # The same characters, scattered through the name.
    scattered = fuzzy_score("disk", "Data, is, kind")
    assert scattered is not None
    assert prefix > scattered


def test_match_sessions_filters_and_ranks():
    sessions = [
        _session("a", "Disk tuning"),
        _session("b", "Web research"),
        _session("c", "Disk cleanup"),
    ]
    matches = match_sessions("disk", sessions)
    assert [s["id"] for s in matches] == ["a", "c"]

    # A query that matches nothing empties the list.
    assert match_sessions("zzz", sessions) == []

    # An empty query lists everything, in the order it came in.
    assert [s["id"] for s in match_sessions("", sessions)] == ["a", "b", "c"]


def test_match_sessions_keeps_the_order_of_equal_matches():
    sessions = [
        _session("a", "Disk tuning"),
        _session("b", "Disk cleanup"),
        _session("c", "Disk cleanup"),
    ]
    assert [s["id"] for s in match_sessions("disk", sessions)] == ["a", "b", "c"]


def test_summary_name_falls_back_to_the_first_query():
    assert summary_name("Disk tuning", "how do I free space?") == "Disk tuning"
    assert summary_name("", "how do I free space?") == "how do I free space?"
    # Whitespace is collapsed, so the name stays on one line.
    assert summary_name("", "  how \n do I free space? ") == "how do I free space?"
    assert summary_name("", "") == ""


# -- the store ---------------------------------------------------------------


def test_store_lists_sessions_newest_first(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(user, "user", "old", {"role": "user", "content": "older"})
    store.append_message(user, "user", "new", {"role": "user", "content": "newer"})
    store.set_title(user, "new", "Disk cleanup")

    sessions = store.list_sessions(user)
    assert len(sessions) == 2
    by_id = {s["id"]: s for s in sessions}
    assert by_id["new"]["summary"] == "Disk cleanup"
    assert by_id["new"]["title"] == "Disk cleanup"
    assert by_id["new"]["messageCount"] == 1
    # A session that was never named is listed under its first query.
    assert by_id["old"]["summary"] == "older"
    assert by_id["old"]["preview"] == "older"


def test_store_lists_only_the_given_user(tmp_path):
    store = SessionStore(tmp_path / "users")
    store.append_message(
        store.local_user, "user", "mine", {"role": "user", "content": "x"}
    )
    store.append_message("root", "root", "theirs", {"role": "user", "content": "y"})

    mine = store.list_sessions(store.local_user)
    assert [s["id"] for s in mine] == ["mine"]
    assert [s["id"] for s in store.list_sessions("root")] == ["theirs"]


def test_store_session_messages_are_the_queries_and_answers(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(
        user, "user", "s", {"role": "user", "content": "how much disk?"}
    )
    store.append_message(user, "user", "s", {"role": "tool", "content": "tool noise"})
    store.append_message(
        user, "user", "s", {"role": "assistant", "content": "42 GB free"}
    )

    messages = store.session_messages(user, "s")
    assert [(m["role"], m["content"]) for m in messages] == [
        ("user", "how much disk?"),
        ("assistant", "42 GB free"),
    ]
    # An unknown session has no transcript.
    assert store.session_messages(user, "missing") == []


def test_valid_user_rejects_path_traversal():
    assert valid_user("michelle") == "michelle"
    for bad in ("", "  ", "..", ".hidden", "a/b", "a\\b"):
        with pytest.raises(ValueError):
            valid_user(bad)


def test_store_rejects_a_user_name_that_escapes_the_directory(tmp_path):
    store = SessionStore(tmp_path / "users")
    with pytest.raises(ValueError):
        store.list_sessions("../etc")


# -- the daemon --------------------------------------------------------------


def test_daemon_searches_and_reads_sessions(config):
    events = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["Disk answer."]),
        emitter=events.append,
    )
    try:
        daemon.submit("how much disk is left?", "user")
        assert wait_for(
            lambda: [e for e in events if e["kind"] == "run_finished"], timeout=5
        )

        user = daemon._sessions.store.local_user
        sessions = daemon.search_sessions(user)
        assert len(sessions) == 1
        session = sessions[0]
        assert session["summary"] == "how much disk is left?"
        assert session["messageCount"] >= 2

        # The query is matched fuzzily against the summary name.
        matches = daemon.search_sessions(user, "disk")
        assert [s["id"] for s in matches] == [session["id"]]
        assert daemon.search_sessions(user, "zzz") == []

        # The transcript carries the query and the answer.
        messages = daemon.session_transcript(user, session["id"])
        assert [m["role"] for m in messages] == ["user", "assistant"]
        assert messages[1]["content"] == "Disk answer."
    finally:
        daemon.shutdown()
