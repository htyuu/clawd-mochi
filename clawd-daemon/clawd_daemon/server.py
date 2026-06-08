"""FastAPI application receiving hook events and pushing to ESP32."""

from __future__ import annotations

import asyncio
import logging
import time
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, Request

from clawd_daemon.collectors import git_info, todo_reader, token_counter
from clawd_daemon.config import Config
from clawd_daemon.esp32_client import Esp32Client
from clawd_daemon.state import ClaudeState, ClaudeStatus, StateManager
from clawd_daemon.storage import Storage, next_midnight

log = logging.getLogger(__name__)


class Daemon:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.state = StateManager()
        self.storage = Storage(cfg.state_dir / "clawd.db")
        self.client = Esp32Client(cfg.esp32_url, cfg.push_interval_ms)
        self._heartbeat_task: asyncio.Task | None = None
        self._midnight_task: asyncio.Task | None = None
        self._duration_task: asyncio.Task | None = None
        self._last_state_snapshot: str = ""
        self._current_session: str = ""
        self._working_dir: str = ""
        self._tool_start_ts: float = 0.0

        # Attempt to discover the running session at boot
        self._discover_session()

    def _discover_session(self) -> None:
        sid = token_counter.find_latest_session_id(self.cfg.claude_dir)
        if sid:
            self._current_session = sid
            log.info("discovered session: %s", sid)

    # -- Event handlers (called from FastAPI hooks) --------------------

    async def on_pre_tool(self, payload: dict) -> None:
        """A tool is about to be called."""
        tool = payload.get("tool", {}).get("name", "") or payload.get("tool", "") or ""
        cwd = payload.get("cwd", "")
        if cwd:
            self._working_dir = cwd
        self._tool_start_ts = time.time()

        # Collect task title from todo
        sid = payload.get("sessionId") or self._current_session
        if sid:
            self._current_session = sid
        task = todo_reader.read_active(self.cfg.claude_dir, self._current_session)

        # Git info
        gi = git_info.collect(cwd or self._working_dir)

        # Token count
        used, tmax = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session
        )

        new_status = ClaudeStatus(
            state=ClaudeState.THINKING,
            tool=tool,
            task=task,
            duration_s=0,
            tokens_used=used,
            tokens_max=tmax,
            git_branch=gi.branch,
            project=gi.project,
        )
        changed = self.state.apply(new_status)
        if changed or tool:
            await self._push()

    async def on_post_tool(self, payload: dict) -> None:
        """A tool just finished."""
        tool_field = payload.get("tool", "")
        if isinstance(tool_field, dict):
            tool = tool_field.get("name", "")
        else:
            tool = str(tool_field)
        err = payload.get("error", "")
        success = not err
        token_used = payload.get("tokens", 0) or payload.get("usage", {}).get("input_tokens", 0)
        token_used += payload.get("usage", {}).get("cache_read_input_tokens", 0)

        self.storage.record_tool(
            tool, success=success, tokens=token_used or 0,
            session=self._current_session,
        )

        used, tmax = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session
        )
        cwd = payload.get("cwd", "") or self._working_dir
        gi = git_info.collect(cwd) if cwd else self.state.status

        new_status = ClaudeStatus(
            state=ClaudeState.ERROR if err else ClaudeState.IDLE,
            tool=tool,
            task=self.state.status.task,
            duration_s=int(time.time() - self._tool_start_ts) if self._tool_start_ts else 0,
            tokens_used=used,
            tokens_max=tmax,
            git_branch=gi.branch if hasattr(gi, "branch") else "",
            project=gi.project if hasattr(gi, "project") else "",
        )
        changed = self.state.apply(new_status)
        if changed:
            await self._push()

    async def on_stop(self, payload: dict) -> None:
        """Claude Code finished the current run."""
        used, tmax = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session
        )
        new_status = ClaudeStatus(
            state=ClaudeState.DONE,
            tokens_used=used,
            tokens_max=tmax,
        )
        changed = self.state.apply(new_status)
        if changed:
            await self._push(force=True)

    async def on_notification(self, payload: dict) -> None:
        """A notification (often a permission prompt) arrived."""
        new_status = ClaudeStatus(
            state=ClaudeState.AWAITING,
            task=str(payload.get("type", "") or payload.get("body", "") or ""),
        )
        changed = self.state.apply(new_status)
        if changed:
            await self._push(force=True)

    async def on_prompt(self, payload: dict) -> None:
        """User submitted a prompt (reset duration)."""
        self._tool_start_ts = time.time()
        new_status = ClaudeStatus(state=ClaudeState.IDLE)
        self.state.apply(new_status)

    # -- Push + heartbeat -----------------------------------------------

    async def _push(self, *, force: bool = False) -> None:
        pushed = await self.client.push_status(
            self.state.status.to_payload(), force=force
        )
        if pushed:
            log.debug("pushed: %s", self.state.status.state.name)

    async def _heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.heartbeat_interval_s)
            if self.state.status.state != ClaudeState.IDLE:
                await self._push()

    async def _duration_loop(self) -> None:
        """Periodically update duration_s while a tool is running."""
        while True:
            await asyncio.sleep(1.0)
            if self.state.status.state == ClaudeState.THINKING and self._tool_start_ts > 0:
                self.state.status.duration_s = int(time.time() - self._tool_start_ts)
                # Don't force-push for duration-only changes — they throttle
                await self.client.push_status(
                    self.state.status.to_payload(), force=False
                )

    async def _midnight_loop(self) -> None:
        """Aggregate daily stats at midnight and push to ESP32."""
        while True:
            wait = next_midnight() - time.time()
            if wait > 0:
                await asyncio.sleep(wait)
            yesterday = time.localtime(time.time() - 3600)
            from datetime import date  # noqa: PLC0415
            day = date(yesterday.tm_year, yesterday.tm_mon, yesterday.tm_mday)
            stats = self.storage.aggregate(day)
            await self.client.push_daily_stats(stats)
            log.info("daily stats pushed: %s", stats)

    # -- Lifecycle ------------------------------------------------------

    async def start_background_tasks(self) -> None:
        self._heartbeat_task = asyncio.create_task(self._heartbeat_loop())
        self._midnight_task = asyncio.create_task(self._midnight_loop())
        self._duration_task = asyncio.create_task(self._duration_loop())

    async def stop_background_tasks(self) -> None:
        for t in (self._heartbeat_task, self._midnight_task, self._duration_task):
            if t:
                t.cancel()


# -- FastAPI app ----------------------------------------------------------

daemon: Daemon | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global daemon
    assert daemon is not None
    app.state.daemon = daemon
    await daemon.start_background_tasks()
    log.info("daemon ready on :%d", daemon.cfg.listen_port)
    yield
    await daemon.stop_background_tasks()
    await daemon.client.close()
    daemon.storage.close()


app = FastAPI(lifespan=lifespan)


def _json_body(r: Request) -> dict:
    """Return event payload, handling both Claude-hook JSON and fallback."""
    # Raw body is already parsed by the caller in route handlers,
    # but we also accept form-encoded "payload" fields.
    return {}


@app.post("/event/pre_tool")
async def evt_pre_tool(request: Request):
    payload = await request.json()
    await app.state.daemon.on_pre_tool(payload)
    return {"ok": True}


@app.post("/event/post_tool")
async def evt_post_tool(request: Request):
    payload = await request.json()
    await app.state.daemon.on_post_tool(payload)
    return {"ok": True}


@app.post("/event/stop")
async def evt_stop(request: Request):
    payload = await request.json()
    await app.state.daemon.on_stop(payload)
    return {"ok": True}


@app.post("/event/notification")
async def evt_notification(request: Request):
    payload = await request.json()
    await app.state.daemon.on_notification(payload)
    return {"ok": True}


@app.post("/event/prompt")
async def evt_prompt(request: Request):
    payload = await request.json()
    await app.state.daemon.on_prompt(payload)
    return {"ok": True}


@app.get("/health")
async def health():
    return {"ok": True}