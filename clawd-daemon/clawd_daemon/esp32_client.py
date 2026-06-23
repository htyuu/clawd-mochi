"""HTTP client for pushing status to the ESP32."""

from __future__ import annotations

import logging
import socket
import time
from urllib.parse import urlsplit

import httpx

log = logging.getLogger(__name__)


class Esp32Client:
    """Push state to the device with throttling and best-effort retry.

    The device is reached via an mDNS hostname (clawd-mochi.local) so the
    daemon doesn't need a hard-coded IP that changes with every router.
    httpx/httpcore don't resolve ``.local`` names on their own, so we resolve
    the hostname ourselves with ``socket.getaddrinfo`` (which goes through the
    system mDNSResponder on macOS) and connect to the IP directly. The
    resolved IP is cached for a short TTL and re-resolved on failure, so when
    the device roams to a different subnet its new IP is picked up.
    """

    # Re-resolve the hostname at most this often (seconds).
    _RESOLVE_TTL_S = 30.0

    def __init__(self, base_url: str, push_interval_ms: int = 1000) -> None:
        # Keep the original base_url for logging; we connect to a resolved IP.
        self._base_url = base_url.rstrip("/")
        parts = urlsplit(self._base_url)
        self._scheme = parts.scheme or "http"
        self._host = parts.hostname or ""
        self._port = parts.port or (443 if self._scheme == "https" else 80)
        # httpx resolves plain hostnames fine, but NOT .local mDNS names.
        # We resolve those ourselves below.
        self._needs_resolve = self._host.endswith(".local")
        self._resolved_ip: str = ""
        self._resolved_at: float = 0.0
        self._client = httpx.AsyncClient(timeout=6.0)
        self._push_interval = push_interval_ms / 1000.0
        self._last_push_ts: float = 0.0
        self._last_payload: dict | None = None

    def _resolve(self) -> str:
        """Return the current IP for the device, resolving via system DNS/mDNS.

        Cached for ``_RESOLVE_TTL_S``. On failure returns the last known IP
        (or the raw hostname as a last resort).
        """
        now = time.time()
        if not self._needs_resolve:
            return self._host
        if self._resolved_ip and (now - self._resolved_at) < self._RESOLVE_TTL_S:
            return self._resolved_ip
        try:
            infos = socket.getaddrinfo(
                self._host, self._port, type=socket.SOCK_STREAM
            )
            for info in infos:
                ip = info[4][0]
                if ip:
                    self._resolved_ip = ip
                    self._resolved_at = now
                    return ip
        except OSError:
            pass
        # Fall back to cached IP, or the raw hostname if we never resolved.
        return self._resolved_ip or self._host

    def _invalidate(self) -> None:
        """Force the next push to re-resolve (call after a connection failure)."""
        self._resolved_at = 0.0

    def _url(self, path: str) -> str:
        host = self._resolve()
        return f"{self._scheme}://{host}:{self._port}{path}"

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

        if not await self._post("/api/status", payload):
            return False

        self._last_push_ts = now
        self._last_payload = payload
        return True

    async def push_daily_stats(self, payload: dict) -> bool:
        return await self._post("/api/stats/daily", payload, warn=True)

    async def _post(self, path: str, payload: dict, *, warn: bool = False) -> bool:
        """POST payload to <path>; on failure, re-resolve and retry once."""
        for attempt in (0, 1):
            try:
                r = await self._client.post(self._url(path), json=payload)
                r.raise_for_status()
                return True
            except (httpx.HTTPError, httpx.TimeoutException, OSError) as e:
                if attempt == 0:
                    # The IP may be stale (device roamed). Force re-resolve + retry.
                    self._invalidate()
                    continue
                (log.warning if warn else log.debug)("esp32 push failed: %s", e)
                return False
        return False
