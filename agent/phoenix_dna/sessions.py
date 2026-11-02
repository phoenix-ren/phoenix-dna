# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session persistence: one SQLite database per user.

Layout::

    <data_dir>/users/<user>/sessions.db      # one DB per user
    <data_dir>/sessions/<mode>/<id>.jsonl    # legacy store, imported once

The user dimension follows the widget's modes: root-mode sessions belong to
``root``, everything else (user mode, plus any legacy ``web`` sessions from
an older daemon) belongs to the local login name — an anonymous browse was
still this human's history.

Schema (per DB)::

    sessions(id, user, mode, title, created_at, updated_at)
    messages(seq, session_id, ts, role, content, message_json)
    runs(seq, session_id, ts, kind, data_json)

``messages`` stores every query and response twice over, deliberately: the
``role``/``content`` pair is what the session search will query, while
``message_json`` keeps the full OpenAI-style message (tool calls included)
for faithful replay into the LLM conversation. ``runs`` is the audit trail
(run_started / run_finished); only ``messages`` are replayed on resume.
System prompts are not persisted - they are rebuilt from the mode on every
run.
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
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
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
"""


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
        """Which user's history a mode's sessions belong to.

        Root-mode sessions are filed under "root"; user-mode sessions
        belong to the local login name. Any other mode string can only
        come from history rows of an older daemon (the removed web
        mode) — they stay in the local login name's database.
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
        """The session's messages, replayable into the LLM conversation."""
        with self._lock:
            db = self._db(user)
            rows = db.execute(
                "SELECT message_json FROM messages WHERE session_id = ? ORDER BY seq",
                (session_id,),
            ).fetchall()
        messages = []
        for (blob,) in rows:
            try:
                messages.append(json.loads(blob))
            except ValueError:
                log.warning("skipping corrupt message row in %s/%s", user, session_id)
        return messages

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
        connection.commit()
        self._dbs[user] = connection
        return connection

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

    def set_title(self, user: str, session_id: str, title: str) -> None:
        """Persist the session's (re)generated title.

        Unlike the usual one-shot providers, the title is rewritten after
        every completed run: a follow-on can pull the conversation onto new
        ground, and the stored name must follow it.
        """
        with self._lock:
            db = self._db(user)
            db.execute(
                "UPDATE sessions SET title = ? WHERE id = ?", (title, session_id)
            )

    def title(self, user: str, session_id: str) -> str:
        """The stored title, or '' when none was generated (yet)."""
        with self._lock:
            db = self._db(user)
            row = db.execute(
                "SELECT title FROM sessions WHERE id = ?", (session_id,)
            ).fetchone()
        return str(row[0]) if row is not None else ""

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
                "SELECT s.id AS id, s.mode AS mode, s.title AS title, "
                "       s.created_at AS created_at, s.updated_at AS updated_at, "
                "       (SELECT m.content FROM messages m "
                "         WHERE m.session_id = s.id AND m.role = 'user' "
                "         ORDER BY m.seq LIMIT 1) AS first_query, "
                "       (SELECT COUNT(*) FROM messages m "
                "         WHERE m.session_id = s.id) AS message_count "
                "FROM sessions s WHERE s.user = ? "
                "ORDER BY s.updated_at DESC",
                (user,),
            ).fetchall()

        sessions = []
        for row in rows:
            first_query = str(row["first_query"] or "")
            sessions.append(
                {
                    "id": str(row["id"]),
                    "mode": str(row["mode"]),
                    "title": str(row["title"] or ""),
                    "summary": summary_name(str(row["title"] or ""), first_query),
                    "preview": " ".join(first_query.split())[:_PREVIEW_LENGTH],
                    "createdAt": str(row["created_at"]),
                    "updatedAt": str(row["updated_at"]),
                    "messageCount": int(row["message_count"]),
                }
            )
        return sessions

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

    def get(self, mode_name: str, session_id: str = "") -> Conversation:
        """Fetch or create a conversation.

        An empty or unknown ``session_id`` starts a fresh conversation in
        the user (and mode) the store files it under.
        """
        user = self.store.user_for_mode(mode_name)
        if session_id:
            key = (user, session_id)
            cached = self._cache.get(key)
            if cached is not None:
                self._cache.move_to_end(key)
                return cached
            conversation = Conversation(
                id=session_id,
                mode=mode_name,
                user=user,
                messages=self.store.read_messages(user, session_id),
            )
        else:
            conversation = Conversation(
                id=uuid4().hex[:12],
                mode=mode_name,
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

    def set_title(self, conversation: Conversation, title: str) -> None:
        self.store.set_title(conversation.user, conversation.id, title)

    def title(self, conversation: Conversation) -> str:
        return self.store.title(conversation.user, conversation.id)
