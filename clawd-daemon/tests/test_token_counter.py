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


def test_count_basic_uses_last_context_not_sum(tmp_path):
    """context_used is the LAST request's input context, not a running sum.

    Each request's input_tokens already contains the full prior conversation,
    so summing across requests double-counts history. We must report the most
    recent occupancy only, and exclude output_tokens (those don't occupy the
    input context window).
    """
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
    used, tmax, model = count_tokens(tmp_path, sid, "sonnet")
    # Last request: input 200 + cache_creation 500 (no cache_read) = 700.
    assert used == 700
    assert tmax == 200_000
    assert model == "sonnet"


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
    used, tmax, model = count_tokens(tmp_path, sid, "sonnet")
    assert used == 50
    assert tmax == 200_000  # comes from opus entry in DEFAULT_MODEL_LIMITS
    assert model == "opus"


def test_context_includes_cache_read(tmp_path):
    """cache_read_input_tokens are part of the current context window too."""
    sid = "s3"
    events = [
        {
            "message": {
                "model": "sonnet",
                "usage": {
                    "input_tokens": 300,
                    "cache_read_input_tokens": 12000,
                    "cache_creation_input_tokens": 800,
                },
            }
        },
    ]
    _write_transcript(tmp_path, sid, events)
    used, tmax, model = count_tokens(tmp_path, sid, "sonnet")
    assert used == 300 + 12000 + 800
    assert tmax == 200_000


def test_glm_context_window_from_config(tmp_path):
    """Non-Claude models resolve their limit from the passed model_limits.

    A config entry 'glm' matches model name 'glm-5.2' via substring match.
    """
    sid = "s4"
    events = [
        {
            "message": {
                "model": "glm-5.2",
                "usage": {"input_tokens": 42000},
            }
        },
    ]
    _write_transcript(tmp_path, sid, events)
    limits = {"sonnet": 200_000, "opus": 200_000, "haiku": 200_000, "glm": 1_000_000}
    used, tmax, model = count_tokens(tmp_path, sid, "sonnet", model_limits=limits)
    assert used == 42000
    assert tmax == 1_000_000
    assert model == "glm-5.2"


def test_glm_falls_back_to_default_without_config(tmp_path):
    """Without a configured limit, glm falls back to the 200k default."""
    sid = "s5"
    events = [
        {
            "message": {
                "model": "glm-5.2",
                "usage": {"input_tokens": 1000},
            }
        },
    ]
    _write_transcript(tmp_path, sid, events)
    used, tmax, model = count_tokens(tmp_path, sid, "sonnet")
    assert used == 1000
    assert tmax == 200_000
    assert model == "glm-5.2"


def test_empty_session_returns_zeros(tmp_path):
    used, tmax, model = count_tokens(tmp_path, "nonexistent", "haiku")
    assert used == 0
    assert tmax == 200_000
    assert model == "haiku"


def test_no_session_id():
    used, tmax, model = count_tokens(Path("."), None, "sonnet")
    assert used == 0
    assert tmax == 200_000
    assert model == "sonnet"
