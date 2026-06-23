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
    cwd: str | None = None,
    model_limits: dict[str, int] | None = None,
) -> tuple[int, int, str]:
    """Return (context_used, context_max, model_name) for the session.

    ``context_used`` is the **current context-window occupancy** — the input
    token count of the most recent request in the transcript
    (``input_tokens + cache_read_input_tokens + cache_creation_input_tokens``).
    It is NOT the cumulative tokens consumed by the session: every request's
    ``input_tokens`` already contains the full prior conversation, so summing
    across requests would double-count history and report a number far larger
    than the actual context window. The device progress bar divides this by
    ``context_max`` to show how full the window is.

    ``context_max`` is the model's context-window size, resolved from
    ``model_limits`` (the daemon config's ``[model_token_limits]``) by exact
    match then longest substring match — so a ``glm`` entry matches
    ``glm-5.2``, and ``sonnet`` matches ``claude-sonnet-4-5-...``.

    Claude Code stores per-project session transcripts at
    ``~/.claude/projects/<cwd-hash>/<session_id>.jsonl`` where ``<cwd-hash>``
    is the working directory with every ``/`` replaced by ``-`` (so
    ``/Users/x/proj`` → ``-Users-x-proj``). The older ``~/.claude/sessions/``
    location is kept as a fallback.
    """
    from clawd_daemon.config import DEFAULT_MODEL_LIMITS  # noqa: PLC0415

    limits = model_limits if model_limits is not None else DEFAULT_MODEL_LIMITS

    if not session_id:
        return 0, _resolve_limit(model, limits), model

    tokens_max = _resolve_limit(model, limits)
    detected_model = model

    # Build candidate transcript paths. The session transcript lives at
    # ~/.claude/projects/<launch-dir-hash>/<session_id>.jsonl, but
    # <launch-dir-hash> is derived from the dir where `claude` was INVOKED
    # — NOT the hook's cwd (which can be a subdirectory of the launch dir).
    # So we glob across ALL project dirs by session id (the session id is
    # unique), then fall back to the cwd-hash dir and the legacy flat
    # sessions/ dir.
    candidates: list[Path] = []
    projects_dir = claude_dir / "projects"
    if projects_dir.exists():
        for d in projects_dir.iterdir():
            if d.is_dir():
                candidates.extend(d.glob(f"{session_id}*.jsonl"))
    if cwd:
        project_hash = cwd.replace("/", "-")
        candidates.append(claude_dir / "projects" / project_hash / f"{session_id}.jsonl")
    candidates.append(claude_dir / "sessions" / f"{session_id}.jsonl")

    # Dedupe by path, newest first (read the freshest match)
    seen: set[Path] = set()
    uniq: list[Path] = []
    for p in candidates:
        if p in seen:
            continue
        seen.add(p)
        uniq.append(p)
    uniq.sort(key=lambda p: p.stat().st_mtime if p.exists() else 0, reverse=True)

    matched = False
    for transcript_path in uniq:
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

        # Most recent context occupancy found in the tail. We keep the LAST
        # usage record only (see docstring: summing would double-count history).
        last_context = 0

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
                # Keep the full model name (e.g. "glm-5.2", "deepseek-v4-pro")
                # so the device shows the exact model. Claude models still map
                # to the short family name (sonnet/opus/haiku) for token limits.
                mf = model_from_file.lower()
                matched = None
                for short in ("sonnet", "opus", "haiku"):
                    if short in mf:
                        matched = short
                        break
                detected_model = matched if matched else mf
                tokens_max = _resolve_limit(detected_model, limits, tokens_max)

            usage = obj.get("message", {}).get("usage", {})
            if not usage:
                usage = obj.get("usage", {})
            if usage:
                # Current context = the input tokens of this request. output
                # tokens are NOT part of the input context window, so excluded.
                last_context = (
                    usage.get("input_tokens", 0)
                    + usage.get("cache_read_input_tokens", 0)
                    + usage.get("cache_creation_input_tokens", 0)
                )

        return last_context, tokens_max, detected_model

    if not matched:
        log.debug("no transcript file found for session %s", session_id)
    return 0, tokens_max, detected_model


def _resolve_limit(
    model_name: str, limits: dict[str, int] | None, default: int = 200_000
) -> int:
    """Context-window limit for a model name.

    Exact match first, then the longest configured key that is a substring of
    the (lower-cased) model name — so ``glm`` matches ``glm-5.2`` and
    ``sonnet`` matches ``claude-sonnet-4-5-...``. Falls back to ``default``.
    """
    if not model_name:
        return default
    name = model_name.lower()
    limits = limits or {}
    if name in limits:
        return limits[name]
    best_key = ""
    best_val = default
    for k, v in limits.items():
        kl = k.lower()
        if kl and kl in name and len(kl) > len(best_key):
            best_key = kl
            best_val = v
    return best_val


def find_latest_session_id(claude_dir: Path) -> str | None:
    """Return the most-recently-written session ID.

    Scans the per-project transcript dir ``~/.claude/projects/*/*.jsonl``
    (Claude Code's actual location) and falls back to the flat
    ``~/.claude/sessions/`` dir.
    """
    search_dirs: list[Path] = []
    projects_dir = claude_dir / "projects"
    if projects_dir.exists():
        search_dirs.extend(p for p in projects_dir.iterdir() if p.is_dir())
    sessions_dir = claude_dir / "sessions"
    if sessions_dir.exists():
        search_dirs.append(sessions_dir)

    latest = None
    latest_mtime = 0.0
    for d in search_dirs:
        try:
            for f in d.glob("*.jsonl"):
                mtime = f.stat().st_mtime
                if mtime > latest_mtime:
                    latest_mtime = mtime
                    latest = f.stem
        except OSError:
            continue
    return latest