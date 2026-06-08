"""HTTP client for pushing status to the ESP32."""

from __future__ import annotations

import logging
import time

import httpx

log = logging.getLogger(__name__)


class Esp32Client:
    """Push state to the device with throttling and best-effort retry."""

    def __init__(self, base_url: str, push_interval_ms: int = 1000) -> None:
        self._base_url = base_url.rstrip("/")
        self._client = httpx.AsyncClient(timeout=2.0)
        self._push_interval = push_interval_ms / 1000.0
        self._last_push_ts: float = 0.0
        self._last_payload: dict | None = None

    async def close(self) -> None:
        await self._client.aclose()

    async def push_status(self, payload: dict, *, force: bool = False) -> bool:
        """Push status to ESP32. Returns True if pushed (False if throttled)."""
        now = time.time()

        # Always push on a "qualitative" change (state/tool/task), otherwise throttle.
        if not force and self._last_payload is not None:
            same_shape = (
                payload.get("state") == self._last_payload.get("state")
                and payload.get("tool") == self._last_payload.get("tool")
                and payload.get("task") == self._last_payload.get("task")
            )
            if same_shape and (now - self._last_push_ts) < self._push_interval:
                return False

        try:
            r = await self._client.post(f"{self._base_url}/api/status", json=payload)
            r.raise_for_status()
        except (httpx.HTTPError, httpx.TimeoutException) as e:
            log.debug("esp32 push failed: %s", e)
            return False

        self._last_push_ts = now
        self._last_payload = payload
        return True

    async def push_daily_stats(self, payload: dict) -> bool:
        try:
            r = await self._client.post(
                f"{self._base_url}/api/stats/daily", json=payload
            )
            r.raise_for_status()
            return True
        except (httpx.HTTPError, httpx.TimeoutException) as e:
            log.warning("daily stats push failed: %s", e)
            return False
