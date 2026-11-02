# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Readline support tests: completion, history tracking, persistence."""

from __future__ import annotations

from phoenix_ai.repl import (
    Completer,
    HistoryTracker,
    _complete_path,
    default_history_path,
    install_readline,
    load_history,
    save_history,
)


def test_slash_command_completion():
    completer = Completer()
    assert completer.matches("/mo", buf="/mo") == ["/mode", "/modes"]
    assert completer.complete("/mo", 0) == "/mode"
    assert completer.complete("/mo", 1) == "/modes"
    assert completer.complete("/mo", 2) is None  # readline end-of-matches
    assert completer.matches("/q", buf="/q") == ["/quit", "/q"]
    # mid-line text is not a command position -> falls through to path completion
    assert completer.matches("/mo", buf="hello /mo") != ["/mode", "/modes"]


def test_mode_argument_completion():
    completer = Completer()
    assert completer.matches("", buf="/mode ") == ["root", "user", "web"]
    assert completer.matches("us", buf="/mode us") == ["user"]
    assert completer.matches("ro", buf="/mode ro") == ["root"]
    assert completer.matches("zz", buf="/mode zz") == []


def test_path_completion(tmp_path):
    (tmp_path / "alpha.txt").write_text("x")
    (tmp_path / "beta").mkdir()
    (tmp_path / ".hidden").write_text("h")

    all_matches = _complete_path(str(tmp_path) + "/")
    assert str(tmp_path) + "/alpha.txt" in all_matches
    assert str(tmp_path) + "/beta/" in all_matches          # dirs get a trailing /
    assert not any(".hidden" in m for m in all_matches)     # hidden hidden

    assert _complete_path(str(tmp_path) + "/al") == [str(tmp_path) + "/alpha.txt"]
    assert any(".hidden" in m for m in _complete_path(str(tmp_path) + "/."))
    assert _complete_path(str(tmp_path) + "/nothing-matches") == []


def test_path_completion_keeps_typed_lead(tmp_path):
    sub = tmp_path / "sub dir"
    sub.mkdir()
    (sub / "inner.txt").write_text("x")
    # the lead (including spaces) is preserved verbatim
    assert _complete_path(f"{sub}/i") == [f"{sub}/inner.txt"]


def test_history_tracker_dedupes():
    added = []
    tracker = HistoryTracker(add_fn=added.append)
    tracker.add("first prompt")
    tracker.add("first prompt")   # consecutive duplicate dropped
    tracker.add("   ")            # blank dropped
    tracker.add("second prompt")
    assert tracker.entries == ["first prompt", "second prompt"]
    assert added == tracker.entries


def test_history_file_roundtrip(tmp_path):
    path = tmp_path / "repl_history"
    save_history(path, ["alpha", "beta"])
    assert load_history(path) == ["alpha", "beta"]
    save_history(path, [])
    assert load_history(path) == []


def test_install_readline_roundtrip(tmp_path):
    import pytest

    if not install_readline.__module__:
        pytest.skip("readline unavailable")
    hist = tmp_path / "repl_history"
    save_history(hist, ["old prompt"])

    tracker = install_readline(hist)
    if tracker is None:  # readline not importable on this platform
        import pytest

        pytest.skip("readline unavailable")
    assert "old prompt" in tracker.entries

    tracker.add("new prompt")
    save_history(hist, tracker.entries)  # what atexit does at exit
    assert load_history(hist) == ["old prompt", "new prompt"]


def test_default_history_path(monkeypatch):
    monkeypatch.delenv("PHOENIX_DATA_DIR", raising=False)
    assert default_history_path().name == "repl_history"
    monkeypatch.setenv("PHOENIX_DATA_DIR", "/tmp/opencode/state")
    assert str(default_history_path()) == "/tmp/opencode/state/repl_history"
