# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session persistence: one SQLite database per user.

Layout::

    <data_dir>/users/<user>/sessions.db      # one DB per user
    <data_dir>/sessions/<mode>/<id>.jsonl    # legacy store, imported once

The user dimension: everything the agent does belongs to the local login
name — an anonymous browse was still this human's history. Sessions
recorded by an older daemon (the removed ``root`` mode, the even older
``web`` mode) keep their own user, so their history stays where it was.

Schema (per DB)::

    sessions(id, user, mode, title, created_at, updated_at)
    messages(seq, session_id, ts, role, content, message_json)
    runs(seq, session_id, ts, kind, data_json)

``messages`` stores every query and response twice over, deliberately: the
``role``/``content`` pair is what the session search will query, while
``message_json`` keeps the full OpenAI-style message (tool calls included)
for faithful replay into the LLM conversation. ``runs`` is the audit trail
(run_started / run_finished); only ``messages`` are replayed on resume.
System prompts are not persisted - they are rebuilt on every run.

A session may be a fork of another one: ``parent_id`` names the session it
was branched from and ``fork_index`` the query it was branched at (see
``fork_session``). The fork is a session of its own - it has its own id,
its own name and its own history - and the session it came from keeps
everything it had.
"""

from __future__ import annotations

import datetime as _dt
import getpass
import json
import logging
import re
import sqlite3
import threading
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from uuid import uuid4

from .search import summary_name

log = logging.getLogger("phoenix.sessions")

#: How long the preview of a session's first query may get.
_PREVIEW_LENGTH = 120

#: The mode every session is filed under. The agent works as the desktop
#: user; sessions recorded by an older daemon carry their own mode ("root"
#: back then, when the panel had a root button), and that is what the
#: search still shows for them.
SESSION_MODE = "user"

#: Reasoning models wrap their thinking in these tags inside the content.
_THINK_BLOCK = re.compile(r"<think>.*?</think>", re.DOTALL)
#: An unclosed block: a run cut off mid-thought.
_THINK_OPEN = re.compile(r"<think>.*$", re.DOTALL)


def _strip_thinking(content: str) -> str:
    """Removes ``<think>…</think>`` blocks from an assistant message.

    The thinking stays in ``message_json``, where the LLM conversation
    replays it; the searchable, shown transcript text is the answer only.
    """
    stripped = _THINK_OPEN.sub("", _THINK_BLOCK.sub("", content))
    if stripped == content:
        return content
    return stripped.strip()


_SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    user TEXT NOT NULL,
    mode TEXT NOT NULL,
    title TEXT NOT NULL DEFAULT '',
    title_manual INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    parent_id TEXT NOT NULL DEFAULT '',
    fork_index INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS messages (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    ts TEXT NOT NULL,
    role TEXT NOT NULL,
    content TEXT NOT NULL,
    message_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq);
CREATE TABLE IF NOT EXISTS runs (
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    ts TEXT NOT NULL,
    kind TEXT NOT NULL,
    data_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_runs_session ON runs(session_id, seq);
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS compactions (
    session_id TEXT NOT NULL,
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    upto_seq INTEGER NOT NULL,
    summary TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_compactions_session ON compactions(session_id, seq);
"""

#: The columns the tables gained after they were first written, and how
#: a database that predates one of them gets it.
#:
#: The tables are created with "IF NOT EXISTS", so a database that
#: already has them keeps the shape it has - which means a column added
#: later has to be added to the old databases too.
_MIGRATIONS = (
    ("sessions", "title_manual", "INTEGER NOT NULL DEFAULT 0"),
    ("sessions", "parent_id", "TEXT NOT NULL DEFAULT ''"),
    ("sessions", "fork_index", "INTEGER NOT NULL DEFAULT 0"),
)

#: The query that reads a user's sessions with everything the search
#: lists them by: their id, mode, title, the summary name they are
#: listed under (computed from the title and the first query), the
#: timestamps and how many messages they hold.
#:
#: The summary name itself is computed in Python, not in SQL, so the
#: same rule names a session in the list and in the answer to a
#: rename.
_SESSION_SUMMARY_SQL = (
    "SELECT s.id AS id, s.mode AS mode, s.title AS title, "
    "       s.created_at AS created_at, s.updated_at AS updated_at, "
    "       s.parent_id AS parent_id, s.fork_index AS fork_index, "
    "       (SELECT m.content FROM messages m "
    "         WHERE m.session_id = s.id AND m.role = 'user' "
    "         ORDER BY m.seq LIMIT 1) AS first_query, "
    "       (SELECT COUNT(*) FROM messages m "
    "         WHERE m.session_id = s.id) AS message_count "
    "FROM sessions s WHERE s.user = ?"
)


@dataclass
class Conversation:
    id: str
    mode: str
    user: str = ""
    messages: list[dict] = field(default_factory=list)
    created_at: str = field(default_factory=lambda: _utcnow())


def _utcnow() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")


def _local_username() -> str:
    """The login name of the human running the daemon.

    Environment first (what the session set), passwd database as fallback,
    "anon" if neither works.
    """
    import os
    import pwd

    for name in ("USER", "LOGNAME"):
        value = os.environ.get(name)
        if value:
            return value
    try:
        return pwd.getpwuid(os.geteuid()).pw_name
    except KeyError:
        pass
    try:
        return getpass.getuser()
    except Exception:
        return "anon"


def valid_user(user: str) -> str:
    """The user name, checked: it must be a plain directory name.

    The user names the database file, so a name with a path separator in
    it would read (or write) outside the users directory.
    """
    name = (user or "").strip()
    if not name or name.startswith(".") or "/" in name or "\\" in name:
        raise ValueError(f"invalid user name {user!r}")
    return name


class SessionStore:
    """SQLite-backed sessions, one database file per user."""

    def __init__(self, users_dir: Path, legacy_dir: Path | None = None):
        self.users_dir = Path(users_dir)
        self.legacy_dir = Path(legacy_dir) if legacy_dir is not None else None
        self.local_user = _local_username()
        self._lock = threading.RLock()
        self._dbs: dict[str, sqlite3.Connection] = {}
        self._migrate_legacy()

    # -- users --------------------------------------------------------------

    def user_for_mode(self, mode: str) -> str:
        """Which user's history a session of the given mode belongs to.

        Everything the agent does now belongs to the local login name.
        The modes of the older daemon ("root", "web") only come from
        history rows of that daemon — they stay in their own database.
        """
        return "root" if mode == "root" else self.local_user

    def db_path(self, user: str) -> Path:
        return self.users_dir / valid_user(user) / "sessions.db"

    # -- messages -----------------------------------------------------------

    def append_message(
        self,
        user: str,
        mode: str,
        session_id: str,
        message: dict,
        ts: str | None = None,
    ) -> None:
        """Persist one conversation message (query, response, tool result)."""
        with self._lock:
            db = self._db(user)
            self._ensure_session(db, user, mode, session_id)
            self._insert_message(db, session_id, ts or _utcnow(), message)
            self._touch_session(db, session_id)

    def read_messages(self, user: str, session_id: str) -> list[dict]:
        """The session's messages, replayable into the LLM conversation.

        A session that has been compacted replays its summary and the
        turns that came after it: the older turns are summarised, and
        the summary is what the model reads. The history itself keeps
        everything - the session search and the transcript show it all.
        """
        with self._lock:
            db = self._db(user)
            upto = self._compacted_upto(db, session_id)
            if upto is None:
                rows = db.execute(
                    "SELECT message_json FROM messages WHERE session_id = ? ORDER BY seq",
                    (session_id,),
                ).fetchall()
                summary = ""
            else:
                rows = db.execute(
                    "SELECT message_json FROM messages WHERE session_id = ? AND seq > ? "
                    "ORDER BY seq",
                    (session_id, upto),
                ).fetchall()
                summary = self._compaction_summary(db, session_id)
        messages = []
        if summary:
            # The summary stands in for the turns it replaced: it comes
            # first, and it says what it is.
            messages.append(
                {
                    "role": "user",
                    "content": "[Summary of the earlier conversation]\n" + summary,
                }
            )
        for (blob,) in rows:
            try:
                messages.append(json.loads(blob))
            except ValueError:
                log.warning("skipping corrupt message row in %s/%s", user, session_id)
        return messages

    # -- compaction ----------------------------------------------------------

    def compact(
        self, user: str, session_id: str, upto_seq: int, summary: str
    ) -> None:
        """Record that the turns up to ``upto_seq`` are summarised.

        The history keeps every message; what changes is what the model
        is sent: the summary, and the turns that came after it. A later
        compaction supersedes an earlier one.
        """
        summary = (summary or "").strip()
        if not summary or upto_seq <= 0:
            return
        with self._lock:
            db = self._db(user)
            db.execute(
                "INSERT INTO compactions (session_id, upto_seq, summary, created_at) "
                "VALUES (?, ?, ?, ?)",
                (session_id, upto_seq, summary, _utcnow()),
            )
        log.info("session %s of %s was summarised up to message %d", session_id, user, upto_seq)

    @staticmethod
    def _compacted_upto(db: sqlite3.Connection, session_id: str) -> int | None:
        """The seq the last compaction ends at, or None when there is none.

        The latest compaction wins: it summarises everything the earlier
        ones did, plus what came after them.
        """
        row = db.execute(
            "SELECT upto_seq FROM compactions WHERE session_id = ? ORDER BY seq DESC LIMIT 1",
            (session_id,),
        ).fetchone()
        return int(row[0]) if row is not None else None

    @staticmethod
    def _compaction_summary(db: sqlite3.Connection, session_id: str) -> str:
        """The summary of the last compaction of a session."""
        row = db.execute(
            "SELECT summary FROM compactions WHERE session_id = ? ORDER BY seq DESC LIMIT 1",
            (session_id,),
        ).fetchone()
        return str(row[0]) if row is not None else ""

    def compaction_input(
        self, user: str, session_id: str, keep_tokens: int
    ) -> tuple[list[dict], int]:
        """What a compaction would summarise, and the seq it ends at.

        The messages that come after the cut are kept as they are: the
        recent turns are what the agent is working on. The cut lands on
        a user message, so that no tool call is left without its result.

        The summary of an earlier compaction is part of what is
        summarised: a second compaction carries the first one along, so
        nothing is lost between two of them.

        Returns an empty list when there is nothing to summarise - no
        user message to cut at, or nothing older than the budget.
        """
        with self._lock:
            db = self._db(user)
            rows = db.execute(
                "SELECT seq, role, message_json FROM messages WHERE session_id = ? ORDER BY seq",
                (session_id,),
            ).fetchall()
            previous = self._compaction_summary(db, session_id)

        entries = []
        for row in rows:
            try:
                message = json.loads(row["message_json"])
            except ValueError:
                continue
            if isinstance(message, dict):
                entries.append((int(row["seq"]), str(row["role"]), message))

        if not entries:
            return [], 0

        # The kept messages: the most recent ones that fit in the
        # budget. Everything older than them is what a summary would
        # stand for.
        budget = max(0, int(keep_tokens)) * 4
        used = 0
        keep_from = 0
        for index in range(len(entries) - 1, -1, -1):
            size = len(json.dumps(entries[index][2], ensure_ascii=False))
            if used + size > budget and index < len(entries) - 1:
                keep_from = index + 1
                break
            used += size
        else:
            # Everything fits in the budget: there is nothing to
            # summarise.
            return [], 0

        # The kept part may not begin with a tool result: its call went
        # with the summarised part, and a tool message without its call
        # is a conversation the model refuses. The cut moves past the
        # tool results that would start it.
        while keep_from < len(entries) and entries[keep_from][1] == "tool":
            keep_from += 1
        if keep_from == 0:
            return [], 0

        older = entries[:keep_from]
        messages: list[dict] = []
        if previous:
            messages.append(
                {
                    "role": "user",
                    "content": "[Summary of the earlier conversation]\n" + previous,
                }
            )
        messages.extend(message for _seq, _role, message in older)
        # The seq the summary ends at: everything from here on is kept.
        return messages, int(older[-1][0])

    def replay_estimate(self, user: str, session_id: str) -> int:
        """How many tokens the conversation would carry, roughly.

        The size of the messages the model would be sent, a token being
        about four characters of them. It is the same measure the
        context view shows, and the one the compaction threshold is
        measured against.
        """
        with self._lock:
            db = self._db(user)
            upto = self._compacted_upto(db, session_id)
            if upto is None:
                row = db.execute(
                    "SELECT SUM(LENGTH(message_json)) AS n FROM messages WHERE session_id = ?",
                    (session_id,),
                ).fetchone()
            else:
                row = db.execute(
                    "SELECT SUM(LENGTH(message_json)) AS n FROM messages "
                    "WHERE session_id = ? AND seq > ?",
                    (session_id, upto),
                ).fetchone()
            summary = self._compaction_summary(db, session_id)
        return (int(row["n"] or 0) if row is not None else 0) // 4 + len(summary) // 4

    # -- audit runs ----------------------------------------------------------

    def record_run(
        self, user: str, mode: str, session_id: str, kind: str, data: dict
    ) -> None:
        """Append an audit entry (run_started / run_finished)."""
        with self._lock:
            db = self._db(user)
            self._ensure_session(db, user, mode, session_id)
            db.execute(
                "INSERT INTO runs (session_id, ts, kind, data_json) VALUES (?, ?, ?, ?)",
                (
                    session_id,
                    _utcnow(),
                    kind,
                    json.dumps(data, ensure_ascii=False),
                ),
            )
            self._touch_session(db, session_id)

    # -- helpers -------------------------------------------------------------

    def _db(self, user: str) -> sqlite3.Connection:
        connection = self._dbs.get(user)
        if connection is not None:
            return connection
        path = self.db_path(user)
        path.parent.mkdir(parents=True, exist_ok=True)
        # check_same_thread=False: the daemon worker (and tests) may touch
        # the store from more than one thread; the store's lock serializes
        # every use. isolation_level=None is autocommit: every statement
        # hits disk immediately, so a crash loses nothing and a second
        # connection (fresh daemon, the search feature) sees everything.
        connection = sqlite3.connect(
            path, check_same_thread=False, isolation_level=None
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA journal_mode=WAL")
        connection.executescript(_SCHEMA)
        self._migrate(connection)
        connection.commit()
        self._dbs[user] = connection
        return connection

    @staticmethod
    def _migrate(db: sqlite3.Connection) -> None:
        """Gives a database written by an older daemon the columns it lacks.

        The tables are created with "IF NOT EXISTS", so a database that
        already has them keeps its own shape - which means a column
        added later has to be added to the old databases as well. The
        names in _MIGRATIONS are the daemon's own constants, never
        anything the user can supply.
        """
        for table, column, definition in _MIGRATIONS:
            columns = {row["name"] for row in db.execute(f"PRAGMA table_info({table})")}
            if column not in columns:
                db.execute(f"ALTER TABLE {table} ADD COLUMN {column} {definition}")
                log.info("added the column %s.%s to the session store", table, column)

    @staticmethod
    def _ensure_session(
        db: sqlite3.Connection, user: str, mode: str, session_id: str
    ) -> None:
        now = _utcnow()
        db.execute(
            "INSERT OR IGNORE INTO sessions (id, user, mode, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (session_id, user, mode, now, now),
        )

    @staticmethod
    def _touch_session(db: sqlite3.Connection, session_id: str) -> None:
        db.execute(
            "UPDATE sessions SET updated_at = ? WHERE id = ?", (_utcnow(), session_id)
        )

    @staticmethod
    def _insert_message(
        db: sqlite3.Connection, session_id: str, ts: str, message: dict
    ) -> None:
        role = str(message.get("role") or "")
        content = message.get("content")
        if isinstance(content, list):
            # A prompt with attachments: the content is a list of parts
            # (text and images). The searchable text is the text parts.
            content = "\n".join(
                str(part.get("text", ""))
                for part in content
                if isinstance(part, dict) and part.get("type") == "text"
            )
        elif not isinstance(content, str):
            # Assistant messages with tool calls may carry None; the query
            # text lives in message_json either way.
            content = "" if content is None else str(content)
        if role == "assistant":
            # Reasoning models wrap their thinking in <think> tags inside
            # the content. The thinking stays in message_json; the search
            # queries and the transcripts the frontends show want the
            # answer only.
            content = _strip_thinking(content)
        db.execute(
            "INSERT INTO messages (session_id, ts, role, content, message_json) "
            "VALUES (?, ?, ?, ?, ?)",
            (
                session_id,
                ts,
                role,
                content,
                json.dumps(message, ensure_ascii=False),
            ),
        )

    # -- titles ---------------------------------------------------------------

    def set_title(self, user: str, session_id: str, title: str) -> bool:
        """Persist the session's (re)generated title.

        Unlike the usual one-shot providers, the title is rewritten after
        every completed run: a follow-on can pull the conversation onto new
        ground, and the stored name must follow it.

        A name the user gave the session stands: the automatic one never
        overwrites it. Returns whether the name was stored - false when
        the session carries a name of the user's own, or is not there.
        """
        with self._lock:
            db = self._db(user)
            cursor = db.execute(
                "UPDATE sessions SET title = ? WHERE id = ? AND title_manual = 0",
                (title, session_id),
            )
        return cursor.rowcount > 0

    def title(self, user: str, session_id: str) -> str:
        """The stored title, or '' when none was generated (yet)."""
        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT title FROM sessions WHERE id = ?", (session_id,)
            ).fetchone()
        return str(row[0]) if row is not None else ""

    def is_manual_title(self, user: str, session_id: str) -> bool:
        """Whether the session's name is the user's own.

        A session the user named is never named again by the model: the
        name they gave is the one they meant.
        """
        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT title_manual FROM sessions WHERE id = ?", (session_id,)
            ).fetchone()
        return bool(row is not None and row[0])

    def rename_session(self, user: str, session_id: str, title: str) -> dict | None:
        """Give a session the user's own name, or take that name back.

        The name stands from now on: the model's own name - which is
        rewritten after every run - never overwrites it. An empty name
        takes the user's name away, and the session is listed under its
        first query again, to be named afresh by the next run.

        A session of another user is not touched: the session id names a
        row of this user's database, and no other.

        Returns the session's entry as the search lists it now, or None
        when there was no such session.
        """
        title = " ".join((title or "").split())
        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT id FROM sessions WHERE id = ? AND user = ?",
                (session_id, user),
            ).fetchone()
            if row is None:
                return None
            db.execute(
                "UPDATE sessions SET title = ?, title_manual = ? WHERE id = ?",
                (title, 1 if title else 0, session_id),
            )
            updated = db.execute(
                _SESSION_SUMMARY_SQL + " AND s.id = ?", (user, session_id)
            ).fetchone()
        return self._summary_of(updated)

    # -- forking a session ----------------------------------------------------

    def fork_session(
        self, user: str, session_id: str, exchange_index: int
    ) -> dict | None:
        """Branch a session into a new one, at one of the user's queries.

        The fork holds the conversation up to that query: the query
        itself and the answer that followed it - everything the agent
        said and did in that exchange. What came after stays in the
        session it was forked from: the fork is the other way round, not
        a copy of both.

        The fork is a session of its own - its own id, its own name, its
        own place in the history - and the follow-ups typed after it
        continue it. The session it was forked from is not touched at
        all: it keeps its own history, and the two of them grow apart.

        ``exchange_index`` counts the user's queries: 1 is the first
        thing the user asked in that session, 2 the next one, and so on.
        The queries are counted as the session holds them, so the fork
        lands on the exchange the user pointed at even when the agent
        did a dozen things in between.

        Returns the fork's entry as the search lists it, or None when
        there was no such session, no such query, or the session belongs
        to somebody else.
        """
        session_id = (session_id or "").strip()
        try:
            exchange_index = int(exchange_index)
        except (TypeError, ValueError):
            return None
        if not session_id or exchange_index < 1:
            return None

        with self._lock:
            db = self._db(user)
            session = db.execute(
                "SELECT id, mode, title FROM sessions WHERE id = ? AND user = ?",
                (session_id, user),
            ).fetchone()
            if session is None:
                # Nothing there: the session is gone, or it belongs to
                # somebody else.
                return None
            messages = db.execute(
                "SELECT ts, role, content, message_json FROM messages "
                "WHERE session_id = ? ORDER BY seq",
                (session_id,),
            ).fetchall()

        # The exchange to fork at: the user's n-th query and everything
        # that came with it, up to their next query.
        start = None
        queries = 0
        for index, message in enumerate(messages):
            if message["role"] == "user":
                queries += 1
                if queries == exchange_index:
                    start = index
                    break
        if start is None:
            # The session holds fewer queries than that: there is no
            # such exchange to fork at.
            return None
        end = len(messages)
        for index in range(start + 1, len(messages)):
            if messages[index]["role"] == "user":
                end = index
                break

        title = self._fork_title(str(session["title"] or ""), messages)
        fork_id = uuid4().hex[:12]
        now = _utcnow()
        with self._lock:
            db = self._db(user)
            db.execute(
                "INSERT INTO sessions (id, user, mode, title, title_manual, "
                "created_at, updated_at, parent_id, fork_index) "
                "VALUES (?, ?, ?, ?, 0, ?, ?, ?, ?)",
                (
                    fork_id,
                    user,
                    str(session["mode"]),
                    title,
                    now,
                    now,
                    session_id,
                    exchange_index,
                ),
            )
            for message in messages[:end]:
                db.execute(
                    "INSERT INTO messages (session_id, ts, role, content, message_json) "
                    "VALUES (?, ?, ?, ?, ?)",
                    (
                        fork_id,
                        message["ts"],
                        message["role"],
                        message["content"],
                        message["message_json"],
                    ),
                )
            updated = db.execute(
                _SESSION_SUMMARY_SQL + " AND s.id = ?", (user, fork_id)
            ).fetchone()
        log.info(
            "forked session %s at query %d into %s", session_id, exchange_index, fork_id
        )
        return self._summary_of(updated)

    @staticmethod
    def _fork_title(title: str, messages) -> str:
        """The name a fork carries until the model names it itself.

        The name of the session it was forked from, marked as a fork -
        the two would otherwise be indistinguishable in the history.
        A session that was never named is named after its first query,
        the way the search lists it.
        """
        base = (title or "").strip()
        if not base:
            for message in messages:
                if message["role"] == "user":
                    base = " ".join(str(message["content"] or "").split())
                    break
        base = base[:_PREVIEW_LENGTH].strip()
        return f"{base} (fork)" if base else "Fork"

    # -- session search --------------------------------------------------------

    def list_sessions(self, user: str) -> list[dict]:
        """Every session of a user, newest first.

        One entry per session: its id, mode, title, the summary name it
        is listed under (the title, or its first query when the session
        was never named), a preview of that first query, its timestamps
        and how many messages it holds. This is what the widget's session
        search shows on the left.
        """
        with self._lock:
            db = self._db(user)
            rows = db.execute(
                _SESSION_SUMMARY_SQL + " ORDER BY s.updated_at DESC", (user,)
            ).fetchall()
        return [self._summary_of(row) for row in rows]

    @staticmethod
    def _summary_of(row) -> dict:
        """One session's entry, as the search lists it.

        The summary name is the title when the session has one - the
        user's own or the model's - and its first query when it has
        none, so a session that was never named is still findable.

        A fork says where it came from: ``parentId`` names the session
        it was branched from, ``forkIndex`` the query it was branched
        at. Both are empty for a session that was not forked.
        """
        first_query = str(row["first_query"] or "")
        title = str(row["title"] or "")
        return {
            "id": str(row["id"]),
            "mode": str(row["mode"]),
            "title": title,
            "summary": summary_name(title, first_query),
            "preview": " ".join(first_query.split())[:_PREVIEW_LENGTH],
            "createdAt": str(row["created_at"]),
            "updatedAt": str(row["updated_at"]),
            "messageCount": int(row["message_count"]),
            "parentId": str(row["parent_id"] or ""),
            "forkIndex": int(row["fork_index"] or 0),
        }

    def session_messages(self, user: str, session_id: str) -> list[dict]:
        """The session's queries and responses, oldest first.

        Only the user and assistant messages: the tool results in between
        are the agent's business, not the conversation the user had with
        it.
        """
        with self._lock:
            db = self._db(user)
            rows = db.execute(
                "SELECT ts, role, content FROM messages "
                "WHERE session_id = ? AND role IN ('user', 'assistant') "
                "ORDER BY seq",
                (session_id,),
            ).fetchall()
        return [
            {
                "ts": str(row["ts"]),
                "role": str(row["role"]),
                "content": str(row["content"]),
            }
            for row in rows
        ]

    # -- the context view ----------------------------------------------------

    def session_context(self, user: str, session_id: str) -> dict:
        """What one session's context window looks like.

        The session's own shape - its title, when it began, how many
        queries and answers it holds - and what its runs spent: the
        tokens of every run the session holds, summed.

        The tokens come from the audit trail: every run records what it
        spent when it ends. So a session opened from the history
        reports the same numbers as the one that is running, and the
        sum covers the whole conversation rather than only its last
        turn.

        The prompt tokens are split the way the context view shows
        them: the fresh input (the prompt tokens the server did not
        answer from its cache) and the cached part apart, so the total
        is the sum of the three and not of the four.
        """
        session_id = (session_id or "").strip()
        if not session_id:
            return {}

        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT title, created_at, updated_at FROM sessions "
                "WHERE id = ? AND user = ?",
                (session_id, user),
            ).fetchone()
            if row is None:
                # No such session (of this user): there is nothing to
                # report, and a session of somebody else's is none of
                # this user's business.
                return {}
            counts = db.execute(
                "SELECT role, COUNT(*) AS n FROM messages WHERE session_id = ? GROUP BY role",
                (session_id,),
            ).fetchall()
            runs = db.execute(
                "SELECT data_json FROM runs WHERE session_id = ? AND kind = 'run_finished' "
                "ORDER BY seq",
                (session_id,),
            ).fetchall()

        user_messages = 0
        assistant_messages = 0
        message_count = 0
        for count in counts:
            number = int(count["n"])
            message_count += number
            role = str(count["role"])
            if role == "user":
                user_messages = number
            elif role == "assistant":
                assistant_messages = number

        prompt_tokens = 0
        output_tokens = 0
        cache_tokens = 0
        reasoning_tokens = 0
        context_tokens = 0
        completed = 0
        for (blob,) in runs:
            try:
                data = json.loads(blob)
            except ValueError:
                log.warning("skipping corrupt run row in %s/%s", user, session_id)
                continue
            if not isinstance(data, dict):
                continue
            prompt_tokens += int(data.get("tokens_in", 0) or 0)
            output_tokens += int(data.get("tokens_out", 0) or 0)
            cache_tokens += int(data.get("cache_tokens", 0) or 0)
            reasoning_tokens += int(data.get("reasoning_tokens", 0) or 0)
            # The last run's prompt is the window as it stands now: the
            # runs come one after the other, and a compaction between
            # them makes the window smaller again.
            context_tokens = int(data.get("context_tokens", 0) or 0)
            completed += 1

        # The fresh input: the prompt tokens that were not served from
        # the cache. A server that counts the cached tokens apart from
        # the prompt ones reports them that way already; the floor
        # keeps the split honest for one that does not.
        fresh_input = max(0, prompt_tokens - cache_tokens)

        # How full the window is: the size of the last prompt the
        # session sent (the provider counts those, so the number is
        # exact). A session whose runs predate that record - or one that
        # has not run yet - is measured by the conversation itself
        # instead: the messages the next prompt would carry, a token
        # being about four characters of them.
        context_used = context_tokens or self.replay_estimate(user, session_id)

        return {
            "id": session_id,
            "title": str(row["title"] or ""),
            "createdAt": str(row["created_at"] or ""),
            "updatedAt": str(row["updated_at"] or ""),
            "userMessages": user_messages,
            "assistantMessages": assistant_messages,
            "messageCount": message_count,
            "runs": completed,
            "tokensIn": fresh_input,
            "tokensOut": output_tokens,
            "cacheTokens": cache_tokens,
            "reasoningTokens": reasoning_tokens,
            "totalTokens": fresh_input + output_tokens + cache_tokens,
            # How much of the model's context window the session takes
            # up, and whether that number is measured or estimated.
            "contextUsed": context_used,
            "contextUsedEstimated": context_tokens == 0,
        }

    # -- deleting a session ---------------------------------------------------

    def delete_session(self, user: str, session_id: str) -> bool:
        """Delete a session and everything that belongs to it.

        The session's messages and its run audit trail go with it: what
        is left behind would be an orphan nobody ever sees again. A
        session of another user is not touched - the session id names a
        row of this user's database, and no other.

        Returns whether there was a session to delete.
        """
        session_id = (session_id or "").strip()
        if not session_id:
            return False
        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT id FROM sessions WHERE id = ? AND user = ?",
                (session_id, user),
            ).fetchone()
            if row is None:
                # Nothing there: the session was already deleted, or it
                # belongs to somebody else.
                return False
            db.execute("DELETE FROM messages WHERE session_id = ?", (session_id,))
            db.execute("DELETE FROM runs WHERE session_id = ?", (session_id,))
            db.execute("DELETE FROM sessions WHERE id = ?", (session_id,))
        log.info("deleted session %s of user %s", session_id, user)
        return True

    # -- legacy JSONL import --------------------------------------------------

    def _migrate_legacy(self) -> None:
        """One-time import of the old JSONL store, marked per DB.

        The legacy layout was <root>/sessions/<mode>/<id>.jsonl with the
        same entry shapes; everything lands in the user DB the mode maps
        to. A corrupt entry is skipped; a failed import is logged and
        dropped (the daemon must not refuse to start over old files).
        """
        if self.legacy_dir is None or not self.legacy_dir.is_dir():
            return
        try:
            for mode_dir in sorted(p for p in self.legacy_dir.iterdir() if p.is_dir()):
                mode = mode_dir.name
                user = self.user_for_mode(mode)
                files = sorted(mode_dir.glob("*.jsonl"))
                if not files:
                    continue
                with self._lock:
                    db = self._db(user)
                    if self._meta_get(db, "jsonl_imported") == "1":
                        continue
                    for path in files:
                        self._import_legacy_file(db, user, mode, path)
                    db.execute(
                        "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
                        ("jsonl_imported", "1"),
                    )
                    db.commit()
                    log.info(
                        "imported %d legacy %s session(s) into %s",
                        len(files),
                        mode,
                        self.db_path(user),
                    )
        except (OSError, sqlite3.Error) as exc:
            log.error("legacy session import failed: %s", exc)

    def _import_legacy_file(
        self, db: sqlite3.Connection, user: str, mode: str, path: Path
    ) -> None:
        session_id = path.stem
        entries = self._read_jsonl(path)
        if not entries:
            return
        self._ensure_session(db, user, mode, session_id)
        for entry in entries:
            kind = entry.get("kind", "")
            ts = entry.get("ts") or _utcnow()
            if kind == "message" and isinstance(entry.get("message"), dict):
                self._insert_message(db, session_id, ts, entry["message"])
            elif kind in ("run_started", "run_finished") and isinstance(
                entry.get("data"), dict
            ):
                db.execute(
                    "INSERT INTO runs (session_id, ts, kind, data_json) VALUES (?, ?, ?, ?)",
                    (
                        session_id,
                        ts,
                        kind,
                        json.dumps(entry["data"], ensure_ascii=False),
                    ),
                )
        self._touch_session(db, session_id)

    @staticmethod
    def _read_jsonl(path: Path) -> list[dict]:
        entries = []
        try:
            for line in path.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    entries.append(json.loads(line))
                except ValueError:
                    log.warning("skipping corrupt line in %s", path)
        except OSError as exc:
            log.error("cannot read legacy session %s: %s", path, exc)
        return entries

    @staticmethod
    def _meta_get(db: sqlite3.Connection, key: str) -> str:
        row = db.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
        return str(row[0]) if row is not None else ""


class SessionManager:
    def __init__(self, store: SessionStore, max_cached: int = 32):
        self.store = store
        self.max_cached = max_cached
        self._cache: OrderedDict[tuple[str, str], Conversation] = OrderedDict()

    def get(self, session_id: str = "") -> Conversation:
        """Fetch or create a conversation.

        An empty or unknown ``session_id`` starts a fresh conversation in
        the user's own history.
        """
        user = self.store.user_for_mode(SESSION_MODE)
        if session_id:
            key = (user, session_id)
            cached = self._cache.get(key)
            if cached is not None:
                self._cache.move_to_end(key)
                return cached
            conversation = Conversation(
                id=session_id,
                mode=SESSION_MODE,
                user=user,
                messages=self.store.read_messages(user, session_id),
            )
        else:
            conversation = Conversation(
                id=uuid4().hex[:12],
                mode=SESSION_MODE,
                user=user,
                messages=[],
            )
        self._cache[(user, conversation.id)] = conversation
        while len(self._cache) > self.max_cached:
            self._cache.popitem(last=False)
        return conversation

    def append(self, conversation: Conversation, message: dict) -> None:
        conversation.messages.append(message)
        self.store.append_message(
            conversation.user, conversation.mode, conversation.id, message
        )

    def record(self, conversation: Conversation, kind: str, data: dict) -> None:
        self.store.record_run(
            conversation.user, conversation.mode, conversation.id, kind, data
        )

    def set_title(self, conversation: Conversation, title: str) -> bool:
        """Store the session's (re)generated name.

        A name the user gave the session stands: the automatic one
        never overwrites it. Returns whether the name was stored.
        """
        return self.store.set_title(conversation.user, conversation.id, title)

    def title(self, conversation: Conversation) -> str:
        return self.store.title(conversation.user, conversation.id)

    def compact(self, conversation: Conversation, upto_seq: int, summary: str) -> None:
        """Replace the older turns of a conversation with a summary.

        The history keeps every message; what changes is what the model
        is sent: the summary, and the turns that came after it. The
        conversation in memory is replaced as well, so the run that
        goes on reads the summary and not the turns it stands for.
        """
        self.store.compact(conversation.user, conversation.id, upto_seq, summary)
        conversation.messages = self.store.read_messages(
            conversation.user, conversation.id
        )

    def rename(self, user: str, session_id: str, title: str) -> dict | None:
        """Give a session the user's own name, or take that name back.

        Returns the session's entry as the search lists it now, or None
        when there was no such session.
        """
        return self.store.rename_session(user, session_id, title)

    def delete(self, user: str, session_id: str) -> bool:
        """Delete a session from the store and drop it from the cache.

        The cache would otherwise hand the deleted conversation back to
        whoever names its id again - a session that is no longer there
        would go on living in the daemon's memory. Returns whether
        there was a session to delete.
        """
        deleted = self.store.delete_session(user, session_id)
        # The cache is keyed by (user, session id), but a session is
        # cached under the user that opened it, which is not always the
        # one that owns it: every entry naming this session goes.
        for key in [key for key in self._cache if key[1] == session_id]:
            del self._cache[key]
        return deleted

    def fork(self, user: str, session_id: str, exchange_index: int) -> dict | None:
        """Branch a session into a new one, at one of the user's queries.

        The fork is a session of its own: it gets its own id, so the
        cache never holds it under the name of the session it was
        forked from. Returns the fork's entry as the search lists it,
        or None when there was no such session or no such query.
        """
        return self.store.fork_session(user, session_id, exchange_index)
