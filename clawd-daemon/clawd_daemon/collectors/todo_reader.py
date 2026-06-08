"""Read the active TodoWrite task for a Claude session.

Claude Code stores per-session todo lists under ~/.claude/todos/.
Format observed: JSON array, each item { content, status, activeForm }.
The first in_progress task is "what Claude is working on now".
"""

from __future__ import annotations

import json
from pathlib import Path


def read_active(claude_dir: Path, session_id: str | None) -> str:
    """Return the currently-active task title for *session_id*, or "" if none."""
    if not session_id:
        return ""
    todos_dir = claude_dir / "todos"
    if not todos_dir.exists():
        return ""

    # File naming varies by Claude Code version: try a few patterns.
    candidates = [
        todos_dir / f"{session_id}.json",
        todos_dir / f"{session_id}-agent.json",
    ]
    candidates.extend(todos_dir.glob(f"{session_id}*.json"))

    for f in candidates:
        if not f.exists():
            continue
        try:
            data = json.loads(f.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        if not isinstance(data, list):
            continue
        for item in data:
            if isinstance(item, dict) and item.get("status") == "in_progress":
                # Prefer activeForm (present continuous) but fall back to content
                return str(item.get("activeForm") or item.get("content") or "")[:60]
        # No in_progress task — fall back to the first pending one
        for item in data:
            if isinstance(item, dict) and item.get("status") == "pending":
                return str(item.get("content") or "")[:60]
    return ""
