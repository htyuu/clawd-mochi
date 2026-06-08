"""Estimate token usage from a Claude session transcript.

The transcript file at ~/.claude/sessions/<session_id>.jsonl is a JSONL
file where each line is a message event. We scan for lines containing
'usage.input_tokens' / 'usage.output_tokens' / 'usage.cache_*' and
accumulate the totals.

Because the transcript can be several MB after a long session, we
use a streaming reader that stops after the last few KB (newest events
are at the end of the file).
"""

from __future__ import annotations

import json
import logging
from pathlib import Path

log = logging.getLogger(__name__)


def count_tokens(
    claude_dir: Path,
    session_id: str | None,
    model: str = "sonnet",
) -> tuple[int, int]:
    """Return (tokens_used, tokens_max) for the session, or (0, 200k) on miss."""
    if not session_id:
        return 0, 200_000

    transcript_dir = claude_dir / "sessions"
    if not transcript_dir.exists():
        return 0, 200_000

    # Look up the model's context limit (we'll override from the file if we
    # encounter a model stanza).  The caller passes the *default* model;
    # the file may contain a "model" key.
    from clawd_daemon.config import DEFAULT_MODEL_LIMITS  # noqa: PLC0415

    tokens_max = DEFAULT_MODEL_LIMITS.get(model, 200_000)

    candidates = [
        transcript_dir / f"{session_id}.jsonl",
    ]
    candidates.extend(sorted(transcript_dir.glob(f"{session_id}*.jsonl"),
                             key=lambda p: p.stat().st_mtime, reverse=True))

    matched = False
    for transcript_path in candidates:
        if not transcript_path.exists():
            continue
        matched = True

        # Read only the tail (last 64 KB) — that's where the latest tool results live.
        stat = transcript_path.stat()
        tail_size = min(stat.st_size, 65_536)
        with transcript_path.open("rb") as fh:
            fh.seek(stat.st_size - tail_size)
            # Skip first partial line
            leftover = fh.read(tail_size)
        # Split lines, drop first if it's partial
        lines = leftover.decode("utf-8", errors="replace").splitlines()
        if (len(lines) > 1 and stat.st_size > tail_size
                and not lines[0].startswith("{")):
            lines = lines[1:]

        total_in = 0
        total_out = 0
        total_cache_read = 0
        total_cache_write = 0

        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            # Update model name from the file if present
            model_from_file = (
                obj.get("message", {}).get("model") or  # anthropic Python
                obj.get("model", "")                     # raw API
            )
            if model_from_file:
                limit = DEFAULT_MODEL_LIMITS.get(
                    model_from_file.lower(), tokens_max
                )
                tokens_max = limit

            usage = obj.get("message", {}).get("usage", {})
            if not usage:
                usage = obj.get("usage", {})
            total_in += usage.get("input_tokens", 0)
            total_out += usage.get("output_tokens", 0)
            total_cache_read += usage.get("cache_read_input_tokens", 0)
            total_cache_write += usage.get("cache_creation_input_tokens", 0)

        grand = total_in + total_out + total_cache_read + total_cache_write
        return grand, tokens_max

    if not matched:
        log.debug("no transcript file found for session %s", session_id)
    return 0, tokens_max


def find_latest_session_id(claude_dir: Path) -> str | None:
    """Return the most-recently-written session ID from the sessions dir,
    or None if empty."""
    sessions_dir = claude_dir / "sessions"
    if not sessions_dir.exists():
        return None
    latest = None
    latest_mtime = 0
    for f in sessions_dir.glob("*.jsonl"):
        try:
            mtime = f.stat().st_mtime
            if mtime > latest_mtime:
                latest_mtime = mtime
                latest = f.stem
        except OSError:
            continue
    return latest