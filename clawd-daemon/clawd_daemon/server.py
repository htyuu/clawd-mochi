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
    # How often (seconds) to push an IDLE heartbeat so the device keeps its
    # clock / online indicator fresh during idle periods.
    IDLE_HEARTBEAT_S = 30.0

    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.state = StateManager()
        self.storage = Storage(cfg.state_dir / "clawd.db")
        self.client = Esp32Client(cfg.esp32_url, cfg.push_interval_ms)
        self._heartbeat_task: asyncio.Task | None = None
        self._midnight_task: asyncio.Task | None = None
        self._duration_task: asyncio.Task | None = None
        self._last_state_snapshot: str = ""
        self._last_push_ts: float = 0.0  # for IDLE heartbeat throttling
        self._current_session: str = ""
        self._working_dir: str = ""
        self._tool_start_ts: float = 0.0
        self._session_start_ts: float = 0.0
        # True while a conversation turn is active (UserPromptSubmit →
        # Stop/idle). _duration_loop ticks session_duration_s only while
        # this is set, so the on-screen "turn elapsed" counter freezes at
        # the turn's total when the turn ends, instead of running forever.
        self._turn_active: bool = False
        self._last_token_used: int = 0  # for per-tool token delta in tool_events
        self._tool_count: int = 0
        self._current_model: str = ""

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
        tool_field = payload.get("tool", "")
        if isinstance(tool_field, dict):
            tool = tool_field.get("name", "")
        else:
            tool = str(tool_field)
        # Claude Code PreToolUse hooks also send tool_name directly
        tool = tool or payload.get("tool_name", "") or ""
        cwd = payload.get("cwd", "")
        if cwd:
            self._working_dir = cwd
        self._tool_start_ts = time.time()

        # If the device is stuck on a transient state (DONE/AWAITING/ERROR)
        # from a previous turn — e.g. Stop fired, or a permission prompt was
        # abandoned — a new tool call means that turn is over. Clear the
        # transient first so this THINKING isn't suppressed by priority
        # merging (DONE=2 > THINKING=1) and the device shows "thinking"
        # instead of staying stuck on the previous turn's colour.
        cur = self.state.status.state
        if cur in (ClaudeState.DONE, ClaudeState.AWAITING, ClaudeState.ERROR):
            self.state.reset()

        # Claude Code hooks send `session_id` (snake_case); accept both.
        sid = payload.get("session_id") or payload.get("sessionId") or ""
        if not sid:
            # Re-discover the latest live session — handles project switches
            # where the boot-discovered session is now stale.
            sid = token_counter.find_latest_session_id(self.cfg.claude_dir) or self._current_session
        if sid:
            # When the active session changes (e.g. user switched project
            # windows), reset the per-turn clock so the new session's
            # duration starts fresh.
            if sid != self._current_session:
                self._session_start_ts = 0.0
                self._tool_count = 0
            self._current_session = sid
        task = todo_reader.read_active(self.cfg.claude_dir, self._current_session)

        # Git info
        gi = git_info.collect(cwd or self._working_dir)

        # Track session start + tool count
        if self._session_start_ts == 0.0:
            self._session_start_ts = time.time()
        self._tool_count += 1

        # Token count + model
        used, tmax, model, _cost, _cached = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session,
            cwd=cwd or self._working_dir,
            model_limits=self.cfg.model_token_limits,
        )
        if model:
            self._current_model = model
        log.info(
            "pre_tool session=%s cwd=%s model=%r used=%d",
            self._current_session, cwd, model, used,
        )
        sdur = int(time.time() - self._session_start_ts) if self._session_start_ts > 0 else 0

        new_status = ClaudeStatus(
            state=ClaudeState.THINKING,
            tool=tool,
            task=task,
            duration_s=0,
            tokens_used=used,
            tokens_max=tmax,
            git_branch=gi.branch,
            project=gi.project,
            model=self._current_model,
            session_duration_s=sdur,
            tool_count=self._tool_count,
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
        # Claude Code hook payload uses tool_name directly
        tool = tool or payload.get("tool_name", "") or ""
        err = payload.get("error", "")
        success = not err

        cwd = payload.get("cwd", "") or self._working_dir
        used, tmax, _, cost, cached = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session,
            cwd=cwd,
            model_limits=self.cfg.model_token_limits,
        )
        # Per-tool token split from the most recent request in the transcript
        # (by PostToolUse time this should be this tool's request):
        #   cost   = input + output + cache_creation (non-cached, processed fresh)
        #   cached = cache_read (cache-hit portion of conversation history)
        # Previously this used (used - last_used) which logged the entire
        # accumulated context as the first tool's cost after a daemon restart,
        # and recorded huge phantom deltas whenever the context jumped.
        self._last_token_used = used

        # Record to storage for any session (local DB write is safe).
        self.storage.record_tool(
            tool, success=success, tokens=cost, tokens_cached=cached,
            session=self._current_session,
        )

        gi = git_info.collect(cwd) if cwd else self.state.status
        sdur = int(time.time() - self._session_start_ts) if self._session_start_ts > 0 else 0

        new_status = ClaudeStatus(
            state=ClaudeState.ERROR if err else ClaudeState.IDLE,
            tool=tool,
            task=self.state.status.task,
            # Tool just ended → no tool is running → duration resets to 0.
            # (The elapsed time of the finished tool is not useful to leave
            # frozen on screen; _duration_loop also enforces this.)
            duration_s=0,
            tokens_used=used,
            tokens_max=tmax,
            git_branch=gi.branch if hasattr(gi, "branch") else "",
            project=gi.project if hasattr(gi, "project") else "",
            model=self._current_model,
            session_duration_s=sdur,
            tool_count=self._tool_count,
        )
        changed = self.state.apply(new_status)
        if changed:
            await self._push()

    async def on_stop(self, payload: dict) -> None:
        """Claude Code finished the current run.

        Goes straight to IDLE (orange) — NOT DONE (green). A real Stop means
        the conversation turn is over and the device should relax to idle,
        not flash "done" green for a few seconds first (that was jarring).
        The DONE state is preserved for cockpit replay: when replay sends a
        ``tool: "replay:..."`` payload we honour the explicit state request
        so the green "done" replay still works.
        """
        # Turn over: freeze session_duration_s at this turn's total until
        # the next UserPromptSubmit resets the clock.
        self._turn_active = False
        used, tmax, _, _, _ = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session,
            cwd=self._working_dir,
            model_limits=self.cfg.model_token_limits,
        )
        sdur = int(time.time() - self._session_start_ts) if self._session_start_ts > 0 else 0

        # Cockpit replay forwards a tool name like "replay:done" — treat that
        # as an explicit DONE request (preserves the green replay). A real
        # Claude Code Stop has no such marker → go straight to IDLE.
        tool = ""
        tool_field = payload.get("tool", "")
        if isinstance(tool_field, dict):
            tool = tool_field.get("name", "")
        else:
            tool = str(tool_field)
        is_replay = tool.startswith("replay:")

        new_status = ClaudeStatus(
            state=ClaudeState.DONE if is_replay else ClaudeState.IDLE,
            tokens_used=used,
            tokens_max=tmax,
            model=self._current_model,
            session_duration_s=sdur,
            tool_count=self._tool_count,
        )
        changed = self.state.apply(new_status)
        if changed:
            await self._push(force=True)

    async def on_notification(self, payload: dict) -> None:
        """A notification arrived.

        Claude Code fires the Notification hook for TWO different cases:
        1. A permission prompt (needs the user to approve a tool) → AWAITING.
        2. An idle "waiting for your input" notice (Claude finished a reply
           and is sitting idle) → should revert to IDLE, NOT hold the device
           on the amber "awaiting" background.

        The previous code marked *every* notification as AWAITING, so the
        idle notice kept the device stuck yellow after each reply. We
        distinguish primarily via the ``notification_type`` field Claude Code
        sends ("idle_prompt" vs a permission type), falling back to the
        message text if the field is absent.
        """
        log.info("notification payload=%r", payload)
        used, tmax, model, _, _ = token_counter.count_tokens(
            self.cfg.claude_dir, self._current_session,
            cwd=self._working_dir,
            model_limits=self.cfg.model_token_limits,
        )
        if model:
            self._current_model = model

        msg = str(
            payload.get("message")
            or payload.get("body")
            or payload.get("type")
            or ""
        )
        ntype = str(payload.get("notification_type", "")).lower()
        low = msg.lower()
        # idle_prompt = Claude finished + waiting for input → IDLE.
        # Anything else (permission request, etc.) → AWAITING.
        is_idle = (
            ntype == "idle_prompt"
            or (not ntype and "waiting for your input" in low)
        )
        if is_idle:
            # Turn over: freeze session_duration_s at this turn's total.
            self._turn_active = False
            # Release the device back to idle — no longer "awaiting".
            new_status = ClaudeStatus(
                state=ClaudeState.IDLE,
                tokens_used=used,
                tokens_max=tmax,
                model=self._current_model,
            )
        else:
            new_status = ClaudeStatus(
                state=ClaudeState.AWAITING,
                task=msg[:60],
                tokens_used=used,
                tokens_max=tmax,
                model=self._current_model,
            )
        changed = self.state.apply(new_status)
        if changed:
            await self._push(force=True)

    async def on_prompt(self, payload: dict) -> None:
        """User submitted a prompt → start a fresh conversation turn.

        Reset the per-turn clock so the device's "session duration" reflects
        THIS conversation turn (prompt → reply), not the wall-clock time
        since the Claude Code session was first launched. Mark the turn
        active so _duration_loop ticks session_duration_s each second even
        during pure-LLM reasoning (no tool calls between prompt and stop).
        """
        self._session_start_ts = time.time()
        self._turn_active = True
        self._tool_start_ts = 0.0
        self._tool_count = 0
        # Reset the carried session_duration_s/tool_count so the device
        # immediately shows the fresh turn (0:00 #0), and force-push so it
        # doesn't wait up to 30s for the next heartbeat to reflect the reset.
        self.state.status.session_duration_s = 0
        self.state.status.tool_count = 0
        self.state.status.duration_s = 0
        self.state.status.tool = ""
        self.state.status.task = ""
        self.state.apply(ClaudeStatus(state=ClaudeState.IDLE))
        await self._push(force=True)

    # -- Push + heartbeat -----------------------------------------------

    async def _push(self, *, force: bool = False) -> None:
        # Time-box the push so a hung ESP32 POST can't block the heartbeat
        # loop forever (which would strand the state machine on a stale
        # transient state). 8s > the client's own 6s timeout + retry, so under
        # normal operation this never fires; it only catches the case where
        # the client's await itself wedges.
        try:
            pushed = await asyncio.wait_for(
                self.client.push_status(
                    self.state.status.to_payload(), force=force
                ),
                timeout=8.0,
            )
        except asyncio.TimeoutError:
            log.warning("push timed out after 8s (skipping)")
            return
        if pushed:
            self._last_push_ts = time.time()
            log.debug("pushed: %s", self.state.status.state.name)

    async def _heartbeat_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.heartbeat_interval_s)
            await self._heartbeat_tick()

    async def _heartbeat_tick(self) -> None:
        """One heartbeat iteration.

        Split out from the loop so each tick is isolated: an exception or a
        stuck ``await self._push()`` in one tick is caught here and cannot
        kill the whole heartbeat task (which would strand the state machine
        on a stale transient state forever, since nothing else expires it).

        The expire-checks run BEFORE any push, and reset() is synchronous
        (no await), so even if the trailing push hangs the next tick will
        still observe the post-reset IDLE state.
        """
        try:
            # Auto-expire transient states (ERROR/AWAITING/DONE) so the device
            # doesn't get stuck on a stale high-priority colour if no further
            # hook event arrives. Also expire orphan THINKING: if we've been
            # in THINKING for >120s with no tool progress (e.g. a conversation
            # was abandoned or the window was closed before firing Stop),
            # reset to IDLE so the device isn't frozen blue forever and the
            # screensaver can eventually activate.
            s = self.state.status.state
            if s in (ClaudeState.ERROR, ClaudeState.AWAITING, ClaudeState.DONE):
                timeout = {
                    ClaudeState.ERROR: 5.0,
                    ClaudeState.AWAITING: 30.0,
                    ClaudeState.DONE: 3.0,
                }[s]
                since = self.state.transient_since
                if since > 0 and time.time() - since > timeout:
                    log.info("transient %s expired after %.0fs → IDLE", s.name, timeout)
                    if self.state.reset():
                        await self._push(force=True)
                    return
            if s == ClaudeState.THINKING:
                if self._tool_start_ts > 0 and time.time() - self._tool_start_ts > 120.0:
                    log.info(
                        "orphan THINKING after %.0fs → IDLE",
                        time.time() - self._tool_start_ts,
                    )
                    self._turn_active = False
                    if self.state.reset():
                        await self._push(force=True)
                    return
            s = self.state.status.state
            if s != ClaudeState.IDLE:
                # Non-idle: push every heartbeat (keeps duration/tool fresh).
                await self._push()
            else:
                # IDLE: push a low-frequency heartbeat so the device keeps its
                # clock and "daemon online" indicator fresh even when no tool
                # is running. Without this the screen's time field freezes
                # during idle periods (daemon stopped pushing → last_update_ms
                # went stale → device stops redrawing the clock).
                now = time.time()
                if now - self._last_push_ts > self.IDLE_HEARTBEAT_S:
                    await self._push(force=True)
        except Exception:  # noqa: BLE001
            log.exception("heartbeat tick failed (will retry next interval)")

    async def _duration_loop(self) -> None:
        """Keep duration_s (per-tool) and session_duration_s (per-turn) live.

        duration_s ticks up while THINKING (seconds the current tool has run),
        clears to 0 otherwise — no tool running means no elapsed to show.

        session_duration_s ticks up while a turn is active (_turn_active,
        i.e. prompt → stop/idle), freezing at the last value when the turn
        ends until the next UserPromptSubmit resets it. Updating it here
        (not just in on_pre_tool/on_stop) keeps the counter moving during
        pure-LLM reasoning where no tool hooks fire between prompt and stop.

        Force-pushed each tick: push_status() throttles on (state, tool,
        task) shape, none of which change during a single tool call or
        turn, so a non-forced push would be dropped as a no-op and the
        on-screen counters would freeze.
        """
        while True:
            await asyncio.sleep(1.0)
            changed = False
            if self.state.status.state == ClaudeState.THINKING and self._tool_start_ts > 0:
                self.state.status.duration_s = int(time.time() - self._tool_start_ts)
                changed = True
            elif self.state.status.duration_s != 0:
                self.state.status.duration_s = 0
                changed = True
            if self._turn_active and self._session_start_ts > 0:
                self.state.status.session_duration_s = int(
                    time.time() - self._session_start_ts
                )
                changed = True
            if changed:
                await self.client.push_status(self.state.status.to_payload(), force=True)

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