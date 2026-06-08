"""SQLite-backed daily statistics aggregator."""

from __future__ import annotations

import sqlite3
import time
from datetime import date, datetime, timedelta
from pathlib import Path


SCHEMA = """
CREATE TABLE IF NOT EXISTS daily_stats (
    date         TEXT PRIMARY KEY,
    tools_called INTEGER NOT NULL DEFAULT 0,
    tokens_total INTEGER NOT NULL DEFAULT 0,
    sessions     INTEGER NOT NULL DEFAULT 0,
    errors       INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS tool_events (
    ts        INTEGER NOT NULL,
    tool      TEXT,
    success   INTEGER NOT NULL DEFAULT 1,
    tokens    INTEGER NOT NULL DEFAULT 0,
    session   TEXT
);

CREATE INDEX IF NOT EXISTS idx_tool_events_ts ON tool_events(ts);
"""


class Storage:
    def __init__(self, db_path: Path) -> None:
        db_path.parent.mkdir(parents=True, exist_ok=True)
        self.db_path = db_path
        self._conn = sqlite3.connect(db_path, isolation_level=None)
        self._conn.executescript(SCHEMA)

    def close(self) -> None:
        self._conn.close()

    # -- event ingestion ------------------------------------------------

    def record_tool(
        self,
        tool: str,
        *,
        success: bool = True,
        tokens: int = 0,
        session: str | None = None,
    ) -> None:
        self._conn.execute(
            "INSERT INTO tool_events (ts, tool, success, tokens, session) "
            "VALUES (?, ?, ?, ?, ?)",
            (int(time.time()), tool, 1 if success else 0, tokens, session),
        )

    # -- aggregation ----------------------------------------------------

    def aggregate(self, day: date) -> dict:
        """Aggregate tool_events for the given day into daily_stats."""
        start = int(datetime.combine(day, datetime.min.time()).timestamp())
        end   = start + 24 * 3600

        cur = self._conn.execute(
            "SELECT "
            "COUNT(*), "
            "COALESCE(SUM(tokens), 0), "
            "COUNT(DISTINCT session), "
            "SUM(CASE WHEN success=0 THEN 1 ELSE 0 END) "
            "FROM tool_events WHERE ts >= ? AND ts < ?",
            (start, end),
        )
        tools_called, tokens_total, sessions, errors = cur.fetchone()
        result = {
            "date":         day.isoformat(),
            "tools_called": int(tools_called or 0),
            "tokens_total": int(tokens_total or 0),
            "sessions":     int(sessions or 0),
            "errors":       int(errors or 0),
        }
        self._conn.execute(
            "INSERT OR REPLACE INTO daily_stats "
            "(date, tools_called, tokens_total, sessions, errors) "
            "VALUES (?, ?, ?, ?, ?)",
            (
                result["date"],
                result["tools_called"],
                result["tokens_total"],
                result["sessions"],
                result["errors"],
            ),
        )
        return result

    def get_day(self, day: date) -> dict | None:
        cur = self._conn.execute(
            "SELECT date, tools_called, tokens_total, sessions, errors "
            "FROM daily_stats WHERE date = ?",
            (day.isoformat(),),
        )
        row = cur.fetchone()
        if not row:
            return None
        return {
            "date":         row[0],
            "tools_called": row[1],
            "tokens_total": row[2],
            "sessions":     row[3],
            "errors":       row[4],
        }

    def prune(self, keep_days: int = 90) -> int:
        """Drop tool_events older than *keep_days* (daily_stats is kept)."""
        cutoff = int(time.time()) - keep_days * 86400
        cur = self._conn.execute(
            "DELETE FROM tool_events WHERE ts < ?", (cutoff,)
        )
        return cur.rowcount


def next_midnight() -> float:
    now = datetime.now()
    tomorrow = (now + timedelta(days=1)).replace(
        hour=0, minute=0, second=5, microsecond=0
    )
    return tomorrow.timestamp()
