"""State machine tests."""

from clawd_daemon.state import ClaudeState, ClaudeStatus, StateManager


def test_apply_promotes_to_higher_priority():
    sm = StateManager()
    assert sm.status.state == ClaudeState.IDLE

    sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tool="Edit"))
    assert sm.status.state == ClaudeState.THINKING

    # ERROR (higher priority) wins
    sm.apply(ClaudeStatus(state=ClaudeState.ERROR))
    assert sm.status.state == ClaudeState.ERROR


def test_apply_keeps_higher_priority_state_but_updates_fields():
    sm = StateManager()
    sm.apply(ClaudeStatus(state=ClaudeState.AWAITING))
    sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tokens_used=500))
    # State should still be AWAITING
    assert sm.status.state == ClaudeState.AWAITING
    # But tokens_used got updated
    assert sm.status.tokens_used == 500


def test_apply_allows_thinking_to_return_to_idle():
    sm = StateManager()
    sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tool="Bash"))

    changed = sm.apply(ClaudeStatus(state=ClaudeState.IDLE))

    assert changed is True
    assert sm.status.state == ClaudeState.IDLE
    assert sm.status.tool == ""


def test_reset_returns_to_idle():
    sm = StateManager()
    sm.apply(ClaudeStatus(state=ClaudeState.ERROR))
    assert sm.reset() is True
    assert sm.status.state == ClaudeState.IDLE
    # Resetting again is a no-op
    assert sm.reset() is False


def test_apply_returns_changed_flag():
    sm = StateManager()
    assert sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tool="Edit")) is True
    # Same payload again — state unchanged
    assert sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tool="Edit")) is False
    # Different tool — should change
    assert sm.apply(ClaudeStatus(state=ClaudeState.THINKING, tool="Read")) is True


def test_to_payload_uses_lowercase_state_name():
    sm = StateManager()
    sm.apply(ClaudeStatus(state=ClaudeState.AWAITING, tool="Bash"))
    p = sm.status.to_payload()
    assert p["state"] == "awaiting"
    assert p["tool"] == "Bash"
    assert "tokens_used" in p


def test_state_from_str():
    assert ClaudeState.from_str("thinking") == ClaudeState.THINKING
    assert ClaudeState.from_str("ERROR") == ClaudeState.ERROR
    assert ClaudeState.from_str("nonsense") == ClaudeState.IDLE
