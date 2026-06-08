"""Token counter tests — uses fixtures, not live files."""

from __future__ import annotations

import json
from pathlib import Path

from clawd_daemon.collectors.token_counter import count_tokens


def _write_transcript(dir_path: Path, sid: str, events: list[dict]) -> Path:
    sessions = dir_path / "sessions"
    sessions.mkdir(exist_ok=True)
    p = sessions / f"{sid}.jsonl"
    lines = "\n".join(json.dumps(e) for e in events)
    p.write_text(lines)
    return p


def test_count_basic(tmp_path):
    sid = "test-session"
    events = [
        {
            "message": {
                "model": "sonnet",
                "usage": {"input_tokens": 100, "output_tokens": 50},
            }
        },
        {
            "message": {
                "usage": {
                    "input_tokens": 200,
                    "output_tokens": 75,
                    "cache_creation_input_tokens": 500,
                }
            }
        },
    ]
    _write_transcript(tmp_path, sid, events)
    used, tmax = count_tokens(tmp_path, sid, "sonnet")
    assert used == 100 + 50 + 200 + 75 + 500
    assert tmax == 200_000


def test_count_with_model_name(tmp_path):
    sid = "s2"
    events = [
        {
            "message": {
                "model": "opus",
                "usage": {"input_tokens": 50},
            }
        },
    ]
    _write_transcript(tmp_path, sid, events)
    used, tmax = count_tokens(tmp_path, sid, "sonnet")
    assert used == 50
    assert tmax == 200_000  # comes from opus entry in DEFAULT_MODEL_LIMITS


def test_empty_session_returns_zeros(tmp_path):
    used, tmax = count_tokens(tmp_path, "nonexistent", "haiku")
    assert used == 0
    assert tmax == 200_000


def test_no_session_id():
    used, tmax = count_tokens(Path("."), None, "sonnet")
    assert used == 0
    assert tmax == 200_000