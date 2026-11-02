# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session persistence: append-only JSONL, one file per session.

Entry shapes (one JSON object per line)::

    {"ts": "...", "kind": "message", "message": {...}}   # model-visible
    {"ts": "...", "kind": "run_started", "data": {...}}  # audit only
    {"ts": "...", "kind": "run_finished", "data": {...}}

Only ``message`` entries are replayed into the conversation when a session
is resumed; audit entries are skipped. System prompts are not persisted -
they are rebuilt from the mode on every run.
"""

from __future__ import annotations

import datetime as _dt
import json
import logging
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from uuid import uuid4

log = logging.getLogger("phoenix.sessions")


@dataclass
class Conversation:
    id: str
    mode: str
    messages: list[dict] = field(default_factory=list)
    created_at: str = field(default_factory=lambda: _utcnow())


def _utcnow() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")


class SessionStore:
    def __init__(self, root: Path):
        self.root = Path(root)

    def path(self, mode: str, session_id: str) -> Path:
        return self.root / mode / f"{session_id}.jsonl"

    def append(self, mode: str, session_id: str, entry: dict) -> None:
        path = self.path(mode, session_id)
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            entry = {"ts": _utcnow(), **entry}
            with open(path, "a", encoding="utf-8") as fh:
                fh.write(json.dumps(entry, ensure_ascii=False) + "\n")
        except OSError as exc:
            log.error("cannot persist session %s/%s: %s", mode, session_id, exc)

    def read(self, mode: str, session_id: str) -> list[dict]:
        path = self.path(mode, session_id)
        if not path.is_file():
            return []
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
            log.error("cannot read session %s: %s", path, exc)
        return entries


class SessionManager:
    def __init__(self, store: SessionStore, max_cached: int = 32):
        self.store = store
        self.max_cached = max_cached
        self._cache: OrderedDict[str, Conversation] = OrderedDict()

    def get(self, mode_name: str, session_id: str = "") -> Conversation:
        """Fetch or create a conversation.

        An empty or unknown ``session_id`` starts a fresh conversation.
        """
        if session_id:
            cached = self._cache.get(session_id)
            if cached is not None:
                self._cache.move_to_end(session_id)
                return cached
            messages = [
                entry["message"]
                for entry in self.store.read(mode_name, session_id)
                if entry.get("kind") == "message"
                and isinstance(entry.get("message"), dict)
            ]
            conversation = Conversation(
                id=session_id, mode=mode_name, messages=messages
            )
        else:
            conversation = Conversation(
                id=uuid4().hex[:12], mode=mode_name, messages=[]
            )
        self._cache[conversation.id] = conversation
        while len(self._cache) > self.max_cached:
            self._cache.popitem(last=False)
        return conversation

    def append(self, conversation: Conversation, message: dict) -> None:
        conversation.messages.append(message)
        self.store.append(
            conversation.mode,
            conversation.id,
            {"kind": "message", "message": message},
        )

    def record(self, conversation: Conversation, kind: str, data: dict) -> None:
        self.store.append(
            conversation.mode, conversation.id, {"kind": kind, "data": data}
        )
