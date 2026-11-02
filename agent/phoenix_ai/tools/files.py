# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Filesystem tools: read / write / edit / ls.

Relative paths resolve against the mode's working directory; absolute paths
are allowed anywhere the process can reach (this is a desktop agent, not a
sandboxed one - see the security notes in agent/README.md).
"""

from __future__ import annotations

import threading
from pathlib import Path

from . import ToolError, ToolResult, ToolSpec, validate_required

_BINARY_SNIFF = 8192


def resolve_path(path: str, root: Path) -> Path:
    if not isinstance(path, str) or not path:
        raise ToolError("path must be a non-empty string")
    p = Path(path).expanduser()
    if not p.is_absolute():
        p = root / p
    return p


def _read_text(path: Path) -> str:
    try:
        with open(path, "rb") as fh:
            head = fh.read(_BINARY_SNIFF)
            if b"\x00" in head:
                raise ToolError(f"refusing to read binary file: {path}")
            return head.decode("utf-8", errors="replace") + fh.read().decode(
                "utf-8", errors="replace"
            )
    except OSError as exc:
        raise ToolError(f"cannot read {path}: {exc.strerror or exc}") from exc


class ReadTool:
    spec = ToolSpec(
        name="read",
        description=(
            "Read the contents of a text file. Defaults to the first 2000 lines; "
            "use offset/limit for large files."
        ),
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path (relative or absolute)"},
                "offset": {"type": "integer", "description": "Line number to start from (1-indexed)"},
                "limit": {"type": "integer", "description": "Maximum lines to return"},
            },
            "required": ["path"],
        },
    )

    def __init__(self, root: Path, max_lines: int = 2000):
        self.root = root
        self.max_lines = max_lines

    def execute(self, args: dict, *, cancel: threading.Event | None = None) -> ToolResult:
        validate_required(self.spec, args)
        path = resolve_path(args["path"], self.root)
        if not path.exists():
            raise ToolError(f"file not found: {path}")
        if path.is_dir():
            raise ToolError(f"{path} is a directory (use ls)")
        offset = max(1, int(args.get("offset", 1)))
        limit = int(args.get("limit") or self.max_lines)
        text = _read_text(path)
        lines = text.splitlines()
        total = len(lines)
        selected = lines[offset - 1 : offset - 1 + limit]
        end = offset - 1 + len(selected)
        header = f"[{args['path']} | lines {offset}-{end} of {total}]"
        body = "\n".join(selected)
        return ToolResult(
            output=f"{header}\n{body}",
            details={"path": str(path), "total_lines": total, "offset": offset, "limit": limit},
        )


class WriteTool:
    spec = ToolSpec(
        name="write",
        description="Create or overwrite a file with the given content. Parent directories are created automatically.",
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path (relative or absolute)"},
                "content": {"type": "string", "description": "Full file content"},
            },
            "required": ["path", "content"],
        },
    )

    def __init__(self, root: Path):
        self.root = root

    def execute(self, args: dict, *, cancel: threading.Event | None = None) -> ToolResult:
        validate_required(self.spec, args)
        path = resolve_path(args["path"], self.root)
        content = args["content"]
        if not isinstance(content, str):
            raise ToolError("content must be a string")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        except OSError as exc:
            raise ToolError(f"cannot write {path}: {exc.strerror or exc}") from exc
        return ToolResult(
            output=f"wrote {len(content)} bytes to {path}",
            details={"path": str(path), "bytes": len(content)},
        )


class EditTool:
    spec = ToolSpec(
        name="edit",
        description=(
            "Edit a file by replacing exact text. oldText must match the file "
            "content exactly (including whitespace) and must be unique in the file."
        ),
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "File path (relative or absolute)"},
                "oldText": {"type": "string", "description": "Exact text to replace (must be unique)"},
                "newText": {"type": "string", "description": "Replacement text"},
            },
            "required": ["path", "oldText", "newText"],
        },
    )

    def __init__(self, root: Path):
        self.root = root

    def execute(self, args: dict, *, cancel: threading.Event | None = None) -> ToolResult:
        validate_required(self.spec, args)
        path = resolve_path(args["path"], self.root)
        old, new = args["oldText"], args["newText"]
        if not isinstance(old, str) or not isinstance(new, str):
            raise ToolError("oldText and newText must be strings")
        if not old:
            raise ToolError("oldText must not be empty")
        if not path.is_file():
            raise ToolError(f"file not found: {path}")
        text = _read_text(path)
        count = text.count(old)
        if count == 0:
            raise ToolError(f"oldText not found in {path}")
        if count > 1:
            raise ToolError(
                f"oldText matches {count} locations in {path}; provide a longer, unique oldText"
            )
        try:
            path.write_text(text.replace(old, new, 1), encoding="utf-8")
        except OSError as exc:
            raise ToolError(f"cannot write {path}: {exc.strerror or exc}") from exc
        return ToolResult(
            output=f"edited {path} (1 replacement)",
            details={"path": str(path), "replacements": 1},
        )


class LsTool:
    spec = ToolSpec(
        name="ls",
        description="List the entries of a directory.",
        parameters={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Directory path (default: working directory)"},
            },
            "required": [],
        },
    )

    def __init__(self, root: Path):
        self.root = root

    def execute(self, args: dict, *, cancel: threading.Event | None = None) -> ToolResult:
        target = resolve_path(args.get("path", "."), self.root)
        if not target.exists():
            raise ToolError(f"directory not found: {target}")
        if not target.is_dir():
            raise ToolError(f"{target} is not a directory")
        try:
            entries = sorted(target.iterdir(), key=lambda p: p.name)
        except OSError as exc:
            raise ToolError(f"cannot list {target}: {exc.strerror or exc}") from exc
        names = [p.name + ("/" if p.is_dir() else "") for p in entries]
        output = "\n".join(names) if names else "(empty directory)"
        return ToolResult(output=output, details={"path": str(target), "count": len(names)})
