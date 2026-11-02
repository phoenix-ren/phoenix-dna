# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Session store and manager tests."""

from __future__ import annotations

from phoenix_ai.sessions import SessionManager, SessionStore


def test_store_roundtrip(tmp_path):
    store = SessionStore(tmp_path / "sessions")
    store.append("user", "abc", {"kind": "message", "message": {"role": "user", "content": "hi"}})
    store.append("user", "abc", {"kind": "run_finished", "data": {"status": "completed"}})
    store.append("user", "abc", {"kind": "message", "message": {"role": "assistant", "content": "yo"}})

    entries = store.read("user", "abc")
    assert len(entries) == 3
    assert all("ts" in e for e in entries)
    assert store.read("user", "missing") == []
    assert store.path("user", "abc").parent == tmp_path / "sessions" / "user"


def test_store_skips_corrupt_lines(tmp_path):
    store = SessionStore(tmp_path / "sessions")
    path = store.path("user", "x")
    path.parent.mkdir(parents=True)
    path.write_text('{"kind": "message", "message": {"role": "user", "content": "ok"}}\nnot json\n\n')
    assert len(store.read("user", "x")) == 1


def test_manager_new_session(tmp_path):
    manager = SessionManager(SessionStore(tmp_path))
    conv = manager.get("user")
    assert conv.id and len(conv.id) == 12
    assert conv.messages == []
    assert conv.mode == "user"


def test_manager_persistence_and_reload(tmp_path):
    store = SessionStore(tmp_path)
    manager = SessionManager(store)
    conv = manager.get("user", "sess42")
    assert conv.messages == []  # unknown id -> fresh conversation

    manager.append(conv, {"role": "user", "content": "hello"})
    manager.append(conv, {"role": "assistant", "content": "hi"})
    manager.record(conv, "run_finished", {"status": "completed"})

    # a brand new manager (fresh process simulation) reloads from disk
    reloaded = SessionManager(SessionStore(tmp_path)).get("user", "sess42")
    assert [m["content"] for m in reloaded.messages] == ["hello", "hi"]

    # audit entries are not replayed as messages
    assert all(m.get("role") for m in reloaded.messages)


def test_manager_cache_reuse(tmp_path):
    manager = SessionManager(SessionStore(tmp_path))
    first = manager.get("user", "cached1")
    first.messages.append({"role": "user", "content": "x"})
    again = manager.get("user", "cached1")
    assert again is first
    assert len(again.messages) == 1


def test_manager_cache_eviction(tmp_path):
    manager = SessionManager(SessionStore(tmp_path), max_cached=2)
    for i in range(5):
        manager.get("user", f"s{i}")
    assert len(manager._cache) == 2
