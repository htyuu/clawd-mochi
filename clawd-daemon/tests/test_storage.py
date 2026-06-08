"""Storage / aggregation tests."""

from __future__ import annotations

import time
from datetime import date, datetime, timedelta

from clawd_daemon.storage import Storage


def _today_at(hour: int) -> int:
    today = datetime.now().replace(hour=hour, minute=0, second=0, microsecond=0)
    return int(today.timestamp())


def test_aggregate_today(tmp_path):
    db = Storage(tmp_path / "test.db")
    db.record_tool("Edit", success=True, tokens=100, session="s1")
    db.record_tool("Edit", success=True, tokens=120, session="s1")
    db.record_tool("Bash", success=False, tokens=50, session="s2")

    stats = db.aggregate(date.today())
    assert stats["tools_called"] == 3
    assert stats["tokens_total"] == 270
    assert stats["sessions"] == 2
    assert stats["errors"] == 1
    assert stats["date"] == date.today().isoformat()


def test_get_day_returns_stored_aggregate(tmp_path):
    db = Storage(tmp_path / "test.db")
    db.record_tool("Read", tokens=10)
    db.aggregate(date.today())
    fetched = db.get_day(date.today())
    assert fetched["tools_called"] == 1
    assert fetched["tokens_total"] == 10


def test_prune_drops_old_events(tmp_path):
    db = Storage(tmp_path / "test.db")
    # Insert one ancient row directly
    db._conn.execute(
        "INSERT INTO tool_events (ts, tool, success, tokens, session) "
        "VALUES (?, 'old', 1, 0, NULL)",
        (int(time.time()) - 365 * 86400,),
    )
    db.record_tool("recent")
    removed = db.prune(keep_days=30)
    assert removed == 1
