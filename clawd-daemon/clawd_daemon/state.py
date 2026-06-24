"""Shared state model and priority-merging logic."""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import IntEnum


class ClaudeState(IntEnum):
    """Priority-ordered state. Higher numeric = higher urgency."""
    IDLE = 0
    THINKING = 1
    DONE = 2
    AWAITING = 3
    ERROR = 4

    @classmethod
    def from_str(cls, s: str) -> ClaudeState:
        mapping = {
            "idle": cls.IDLE,
            "thinking": cls.THINKING,
            "done": cls.DONE,
            "awaiting": cls.AWAITING,
            "error": cls.ERROR,
        }
        return mapping.get(s.lower(), cls.IDLE)


@dataclass
class ClaudeStatus:
    state: ClaudeState = ClaudeState.IDLE
    tool: str = ""
    task: str = ""
    duration_s: int = 0
    tokens_used: int = 0
    tokens_max: int = 200_000
    git_branch: str = ""
    project: str = ""
    model: str = ""               # "sonnet" / "opus" / "haiku"
    session_duration_s: int = 0   # total session wall-clock
    tool_count: int = 0           # tool call count in current session
    updated_at: float = 0.0       # time.time()

    def to_payload(self) -> dict:
        """Serialize to the JSON body the ESP32 /api/status endpoint expects."""
        return {
            "state": self.state.name.lower(),
            "tool": self.tool,
            "task": self.task,
            "duration_s": self.duration_s,
            "tokens_used": self.tokens_used,
            "tokens_max": self.tokens_max,
            "git_branch": self.git_branch,
            "project": self.project,
            "model": self.model,
            "session_duration_s": self.session_duration_s,
            "tool_count": self.tool_count,
        }


class StateManager:
    """Aggregates hook events into a single canonical status.

    Handles priority merging: high-priority events (error, awaiting)
    can only be cleared by a higher-or-equal event or an explicit reset.
    """

    def __init__(self) -> None:
        self._status = ClaudeStatus(updated_at=time.time())
        # When the current transient state (ERROR/AWAITING/DONE) was first
        # entered. Used by the daemon heartbeat to auto-expire stale states
        # that would otherwise be held forever by priority-merging.
        self.transient_since: float = 0.0

    @property
    def status(self) -> ClaudeStatus:
        return self._status

    def apply(self, new: ClaudeStatus) -> bool:
        """Merge *new* into the current status.

        Returns True if the effective state changed (caller should push).
        """
        old_state = self._status.state
        now = time.time()
        new.updated_at = now

        # Only transient high-priority states should suppress lower-priority
        # updates. THINKING is not transient: PostToolUse/Stop must be able to
        # return it to IDLE immediately.
        current_is_transient = self._status.state in (
            ClaudeState.ERROR,
            ClaudeState.AWAITING,
            ClaudeState.DONE,
        )
        if current_is_transient and new.state < self._status.state:
            self._status.tool = new.tool
            self._status.task = new.task
            self._status.tokens_used = new.tokens_used
            self._status.tokens_max = new.tokens_max
            self._status.git_branch = new.git_branch
            self._status.project = new.project
            self._status.model = new.model
            self._status.session_duration_s = new.session_duration_s
            self._status.tool_count = new.tool_count
            self._status.updated_at = now
            return False

        changed = (
            self._status.state != new.state
            or self._status.tool != new.tool
            or self._status.task != new.task
        )
        # A transition INTO a transient state (or same transient refreshed by
        # a same-level event) starts the auto-expire timer. Downgrades to
        # IDLE/THINKING clear it.
        if new.state in (ClaudeState.ERROR, ClaudeState.AWAITING, ClaudeState.DONE):
            if old_state != new.state or self.transient_since == 0.0:
                self.transient_since = now
        else:
            self.transient_since = 0.0
        self._status = new
        return changed

    def reset(self) -> bool:
        """Reset to IDLE, preserving carry-over display fields.

        Only the state and per-tool fields (tool/task/duration_s) are
        cleared. model, project, git_branch, session_duration_s and
        tool_count are kept so the device keeps showing "this turn took
        M:SS, #N tools, model=glm-5.2" during the IDLE that follows a
        transient state (DONE/AWAITING/ERROR) expiring, instead of
        flashing blank / zero.
        """
        if self._status.state == ClaudeState.IDLE:
            return False
        self.transient_since = 0.0
        self._status.state = ClaudeState.IDLE
        self._status.tool = ""
        self._status.task = ""
        self._status.duration_s = 0
        self._status.updated_at = time.time()
        return True

    def tick_heartbeat(self) -> ClaudeStatus | None:
        """Return current status to push as heartbeat, or None if nothing."""
        self._status.updated_at = time.time()
        # Only push for non-IDLE states so the ESP32 knows we're alive.
        if self._status.state != ClaudeState.IDLE:
            return self._status
        return None
