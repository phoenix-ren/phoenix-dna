# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Filesystem tool tests (read / write / edit / ls)."""

from __future__ import annotations

import pytest

from phoenix_ai.tools.files import EditTool, LsTool, ReadTool, WriteTool, resolve_path
from phoenix_ai.tools import ToolError


def test_resolve_path(home_dir):
    from pathlib import Path

    assert resolve_path("notes/todo.txt", home_dir) == home_dir / "notes" / "todo.txt"
    assert resolve_path("/etc/os-release", home_dir) == Path("/etc/os-release")
    with pytest.raises(ToolError):
        resolve_path("", home_dir)


def test_read_pagination(home_dir):
    path = home_dir / "poem.txt"
    path.write_text("one\ntwo\nthree\nfour\nfive\n")
    tool = ReadTool(home_dir, max_lines=100)

    result = tool.execute({"path": "poem.txt"})
    assert "one" in result.output and "five" in result.output
    assert result.details["total_lines"] == 5

    result = tool.execute({"path": "poem.txt", "offset": 2, "limit": 2})
    assert result.output.splitlines() == ["[poem.txt | lines 2-3 of 5]", "two", "three"]


def test_read_errors(home_dir):
    tool = ReadTool(home_dir)
    with pytest.raises(ToolError, match="not found"):
        tool.execute({"path": "missing.txt"})
    (home_dir / "adir").mkdir()
    with pytest.raises(ToolError, match="directory"):
        tool.execute({"path": "adir"})
    (home_dir / "blob.bin").write_bytes(b"ab\x00cd")
    with pytest.raises(ToolError, match="binary"):
        tool.execute({"path": "blob.bin"})
    with pytest.raises(ToolError, match="required"):
        tool.execute({})


def test_write_creates_parents(home_dir):
    tool = WriteTool(home_dir)
    result = tool.execute({"path": "deep/nested/file.txt", "content": "hello"})
    assert (home_dir / "deep" / "nested" / "file.txt").read_text() == "hello"
    assert "wrote 5 bytes" in result.output


def test_edit_exact_unique(home_dir):
    path = home_dir / "config.conf"
    path.write_text("alpha=1\nbeta=2\n")
    tool = EditTool(home_dir)

    result = tool.execute({"path": "config.conf", "oldText": "beta=2", "newText": "beta=42"})
    assert path.read_text() == "alpha=1\nbeta=42\n"
    assert "1 replacement" in result.output

    with pytest.raises(ToolError, match="not found"):
        tool.execute({"path": "config.conf", "oldText": "gamma=3", "newText": "x"})

    path.write_text("dup\ndup\n")
    with pytest.raises(ToolError, match="2 locations"):
        tool.execute({"path": "config.conf", "oldText": "dup", "newText": "other"})


def test_ls(home_dir):
    (home_dir / "solo").mkdir()
    (home_dir / "a.txt").write_text("x")
    tool = LsTool(home_dir)
    result = tool.execute({"path": "."})
    assert result.output.splitlines() == ["a.txt", "solo/"]
    (home_dir / "empty").mkdir()
    assert tool.execute({"path": "empty"}).output == "(empty directory)"
    with pytest.raises(ToolError):
        tool.execute({"path": "nope"})
