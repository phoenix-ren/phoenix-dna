# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session store and manager tests (per-user SQLite)."""

from __future__ import annotations

import json
import sqlite3

from phoenix_dna.sessions import SessionManager, SessionStore, _local_username


def _sessions_rows(db_path):
    db = sqlite3.connect(db_path)
    db.row_factory = sqlite3.Row
    return [dict(r) for r in db.execute("SELECT * FROM sessions")]


def _message_rows(db_path):
    db = sqlite3.connect(db_path)
    db.row_factory = sqlite3.Row
    return [dict(r) for r in db.execute("SELECT * FROM messages ORDER BY seq")]


def test_store_roundtrip(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(user, "user", "abc", {"role": "user", "content": "hi"})
    store.record_run(user, "user", "abc", "run_finished", {"status": "completed"})
    store.append_message(user, "user", "abc", {"role": "assistant", "content": "yo"})

    messages = store.read_messages(user, "abc")
    assert [m["content"] for m in messages] == ["hi", "yo"]

    rows = _message_rows(store.db_path(user))
    assert [r["role"] for r in rows] == ["user", "assistant"]
    assert rows[0]["content"] == "hi"
    # The full message is kept for faithful replay.
    assert json.loads(rows[0]["message_json"]) == {"role": "user", "content": "hi"}

    sessions = _sessions_rows(store.db_path(user))
    assert len(sessions) == 1
    assert sessions[0]["id"] == "abc"
    assert sessions[0]["mode"] == "user"
    assert sessions[0]["user"] == user
    assert sessions[0]["created_at"] <= sessions[0]["updated_at"]

    # Unknown sessions read back empty.
    assert store.read_messages(user, "missing") == []


def test_one_database_per_user(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user

    assert store.user_for_mode("root") == "root"
    assert store.user_for_mode("user") == user

    store.append_message(user, "user", "s1", {"role": "user", "content": "mine"})
    store.append_message("root", "root", "s2", {"role": "user", "content": "root's"})

    assert store.db_path(user).parent == tmp_path / "users" / user
    assert store.db_path("root").parent == tmp_path / "users" / "root"
    assert store.read_messages(user, "s2") == []  # not in michelle's DB
    assert store.read_messages("root", "s1") == []  # not in root's DB

    root_rows = _message_rows(store.db_path("root"))
    assert [r["content"] for r in root_rows] == ["root's"]


def test_manager_new_session(tmp_path):
    manager = SessionManager(SessionStore(tmp_path / "users"))
    conv = manager.get()
    assert conv.id and len(conv.id) == 12
    assert conv.messages == []
    assert conv.mode == "user"
    assert conv.user == manager.store.local_user


def test_manager_belongs_to_the_local_user(tmp_path):
    manager = SessionManager(SessionStore(tmp_path / "users"))
    conv = manager.get("sess42")
    assert conv.user == manager.store.local_user


def test_manager_persistence_and_reload(tmp_path):
    store = SessionStore(tmp_path / "users")
    manager = SessionManager(store)
    conv = manager.get("sess42")
    assert conv.messages == []  # unknown id -> fresh conversation

    manager.append(conv, {"role": "user", "content": "hello"})
    manager.append(conv, {"role": "assistant", "content": "hi"})
    manager.record(conv, "run_finished", {"status": "completed"})

    # a brand new manager (fresh process simulation) reloads from disk
    fresh = SessionManager(SessionStore(tmp_path / "users"))
    conv2 = fresh.get("sess42")
    assert [m["content"] for m in conv2.messages] == ["hello", "hi"]
    assert conv2.messages[0]["role"] == "user"

    # run audit entries exist exactly once (not duplicated by the reload)
    db = sqlite3.connect(store.db_path(store.local_user))
    runs = db.execute("SELECT kind FROM runs WHERE session_id = 'sess42'").fetchall()
    assert [r[0] for r in runs] == ["run_finished"]


def test_manager_caches_the_conversation(tmp_path):
    manager = SessionManager(SessionStore(tmp_path / "users"))
    conv = manager.get("shared-id")

    conv.messages.append({"role": "user", "content": "my q"})
    cached = manager.get("shared-id")
    assert cached is conv


def test_tool_call_message_roundtrip(tmp_path):
    """Assistant messages with tool calls survive replay faithfully."""
    store = SessionStore(tmp_path / "users")
    manager = SessionManager(store)
    conv = manager.get("tools")
    message = {
        "role": "assistant",
        "content": "",
        "tool_calls": [
            {
                "id": "call_1",
                "type": "function",
                "function": {"name": "bash", "arguments": '{"command": "ls"}'},
            }
        ],
    }
    manager.append(conv, message)

    fresh = SessionManager(SessionStore(tmp_path / "users"))
    replay = fresh.get("tools").messages
    assert replay == [message]


def test_stored_transcript_text_carries_no_thinking(tmp_path):
    """The searchable content is the answer; message_json keeps the raw."""
    store = SessionStore(tmp_path / "users")
    manager = SessionManager(store)
    conv = manager.get("thinkest")
    manager.append(
        conv,
        {
            "role": "assistant",
            "content": "<think>Pondering.</think>The answer.",
        },
    )

    # the transcript the frontends show: the answer only
    messages = store.session_messages(_local_username(), conv.id)
    assert messages[-1]["content"] == "The answer."
    # the replayable message keeps the reasoning
    fresh = SessionManager(SessionStore(tmp_path / "users"))
    replay = fresh.get("thinkest").messages
    assert replay[-1]["content"] == "<think>Pondering.</think>The answer."


def test_stored_transcript_text_drops_unclosed_thinking(tmp_path):
    store = SessionStore(tmp_path / "users")
    manager = SessionManager(store)
    conv = manager.get("thinkest")
    manager.append(
        conv,
        {"role": "assistant", "content": "Answer first. <think>cut off mid"},
    )
    messages = store.session_messages(_local_username(), conv.id)
    assert messages[-1]["content"] == "Answer first."


def test_delete_session_takes_everything_with_it(tmp_path):
    """A deleted session is gone: its messages and its runs too."""
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(user, "user", "keep", {"role": "user", "content": "mine"})
    store.append_message(user, "user", "drop", {"role": "user", "content": "theirs"})
    store.record_run(user, "user", "drop", "run_finished", {"status": "completed"})

    assert store.delete_session(user, "drop") is True

    assert store.read_messages(user, "drop") == []
    assert store.session_messages(user, "drop") == []
    assert [s["id"] for s in store.list_sessions(user)] == ["keep"]

    db = sqlite3.connect(store.db_path(user))
    runs = db.execute(
        "SELECT COUNT(*) FROM runs WHERE session_id = 'drop'"
    ).fetchone()[0]
    assert runs == 0

    # The other session is untouched.
    assert [m["content"] for m in store.read_messages(user, "keep")] == ["mine"]


def test_delete_session_touches_no_other_user(tmp_path):
    """A session id names one user's session: the others are not touched."""
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(user, "user", "mine", {"role": "user", "content": "mine"})
    store.append_message(
        "root", "root", "theirs", {"role": "user", "content": "theirs"}
    )

    # The session exists, but it belongs to another user.
    assert store.delete_session(user, "theirs") is False
    assert store.read_messages("root", "theirs") != []

    assert store.delete_session("root", "theirs") is True
    assert store.read_messages("root", "theirs") == []


def test_delete_session_that_is_not_there(tmp_path):
    """Deleting a session that is gone is not an error: it says so."""
    store = SessionStore(tmp_path / "users")
    assert store.delete_session(store.local_user, "missing") is False
    assert store.delete_session(store.local_user, "") is False


def test_the_manager_forgets_a_deleted_session(tmp_path):
    """The cache must not hand a deleted conversation back."""
    store = SessionStore(tmp_path / "users")
    manager = SessionManager(store)
    conv = manager.get("sess42")
    manager.append(conv, {"role": "user", "content": "hello"})

    assert manager.delete(store.local_user, "sess42") is True

    # The conversation is gone from the cache, not merely from the disk.
    assert manager.get("sess42").messages == []


def test_a_manual_name_stands_against_the_model(tmp_path):
    """The name the user gave the session is never overwritten."""
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(
        user, "user", "s1", {"role": "user", "content": "how much disk is left?"}
    )

    entry = store.rename_session(user, "s1", "Disk tuning")
    assert entry["summary"] == "Disk tuning"
    assert entry["title"] == "Disk tuning"
    assert store.is_manual_title(user, "s1") is True

    # The model's own name never overwrites it.
    assert store.set_title(user, "s1", "Disk usage") is False
    assert store.title(user, "s1") == "Disk tuning"

    # The name the session is listed under is the user's own.
    listed = store.list_sessions(user)
    assert [s["summary"] for s in listed] == ["Disk tuning"]


def test_taking_the_manual_name_back(tmp_path):
    """An empty name gives the session back to the model."""
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(
        user, "user", "s1", {"role": "user", "content": "how much disk is left?"}
    )
    store.rename_session(user, "s1", "Disk tuning")

    entry = store.rename_session(user, "s1", "")
    assert entry["title"] == ""
    # Without a name of its own, the session is listed under its first
    # query again.
    assert entry["summary"] == "how much disk is left?"
    assert store.is_manual_title(user, "s1") is False

    # And the model may name it again.
    assert store.set_title(user, "s1", "Disk usage") is True
    assert store.title(user, "s1") == "Disk usage"


def test_rename_touches_no_other_user(tmp_path):
    """A session id names one user's session: the others are not touched."""
    store = SessionStore(tmp_path / "users")
    user = store.local_user
    store.append_message(user, "user", "mine", {"role": "user", "content": "mine"})
    store.append_message(
        "root", "root", "theirs", {"role": "user", "content": "theirs"}
    )

    assert store.rename_session(user, "theirs", "Not yours") is None
    assert store.title("root", "theirs") == ""

    assert store.rename_session("root", "theirs", "Yours")["summary"] == "Yours"
    assert store.title(user, "mine") == ""


def test_rename_of_a_session_that_is_not_there(tmp_path):
    """Renaming a session that is gone is not an error: it says so."""
    store = SessionStore(tmp_path / "users")
    assert store.rename_session(store.local_user, "missing", "A name") is None


def test_the_store_gains_the_manual_name_column(tmp_path):
    """A database written by the older daemon gets the column it lacks.

    The tables are created with "IF NOT EXISTS", so a database that
    already has them keeps its own shape - the column added later has
    to be added to it as well.
    """
    users = tmp_path / "users"
    user = _local_username()
    (users / user).mkdir(parents=True)
    connection = sqlite3.connect(users / user / "sessions.db")
    connection.executescript(
        "CREATE TABLE sessions ("
        "  id TEXT PRIMARY KEY, user TEXT NOT NULL, mode TEXT NOT NULL,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL, updated_at TEXT NOT NULL);"
    )
    connection.execute(
        "INSERT INTO sessions VALUES ('s1', ?, 'user', 'Old name', 'x', 'x')",
        (user,),
    )
    connection.commit()
    connection.close()

    store = SessionStore(users)
    entry = store.rename_session(user, "s1", "New name")
    assert entry["summary"] == "New name"
    assert store.title(user, "s1") == "New name"


def test_legacy_jsonl_import(tmp_path):
    legacy = tmp_path / "sessions"
    for mode, sid, contents in (
        (
            "user",
            "old1",
            [{"role": "user", "content": "hi"}, {"role": "assistant", "content": "yo"}],
        ),
        ("root", "oldroot", [{"role": "user", "content": "fix it"}]),
    ):
        path = legacy / mode / f"{sid}.jsonl"
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = [
            json.dumps(
                {
                    "ts": "2026-01-01T00:00:00+00:00",
                    "kind": "run_started",
                    "data": {"run_id": "r1", "mode": mode},
                }
            )
        ]
        lines += [
            json.dumps(
                {"ts": "2026-01-01T00:00:01+00:00", "kind": "message", "message": m}
            )
            for m in contents
        ]
        lines += [
            json.dumps(
                {
                    "ts": "2026-01-01T00:00:02+00:00",
                    "kind": "run_finished",
                    "data": {"status": "completed"},
                }
            )
        ]
        path.write_text("\n".join(lines) + "\n")

    store = SessionStore(tmp_path / "users", legacy)
    user = store.local_user

    mine = store.read_messages(user, "old1")
    assert [m["content"] for m in mine] == ["hi", "yo"]
    root_messages = store.read_messages("root", "oldroot")
    assert [m["content"] for m in root_messages] == ["fix it"]

    db = sqlite3.connect(store.db_path(user))
    runs = db.execute("SELECT kind FROM runs WHERE session_id = 'old1'").fetchall()
    assert [r[0] for r in runs] == ["run_started", "run_finished"]

    # A second store instance must not duplicate anything.
    again = SessionStore(tmp_path / "users", legacy)
    assert [m["content"] for m in again.read_messages(user, "old1")] == ["hi", "yo"]
    db2 = sqlite3.connect(again.db_path(user))
    count = db2.execute(
        "SELECT COUNT(*) FROM messages WHERE session_id = 'old1'"
    ).fetchone()[0]
    assert count == 2


def test_legacy_import_skips_corrupt_lines(tmp_path):
    legacy = tmp_path / "sessions" / "user"
    legacy.mkdir(parents=True)
    (legacy / "broken.jsonl").write_text(
        '{"kind": "message", "message": {"role": "user", "content": "ok"}}\nnot json\n\n'
    )
    store = SessionStore(tmp_path / "users", tmp_path / "sessions")
    assert [m["content"] for m in store.read_messages(store.local_user, "broken")] == [
        "ok"
    ]


def test_local_username_resolution():
    """The local user comes from the environment or passwd, never blank."""
    assert _local_username()
