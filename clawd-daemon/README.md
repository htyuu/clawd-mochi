# clawd-daemon

Bridges Claude Code hook events to your ESP32-C3 running the
`clawd_mochi_companion` firmware, so your Clawd Mochi knows what
Claude is doing in real-time.

## What it does

- Listens on `localhost:7878` for hook events posted by Claude Code
  (PreToolUse, PostToolUse, Stop, Notification, UserPromptSubmit)
- Aggregates them into a single `ClaudeStatus` (state + tool + task + tokens)
- Pushes the status to your ESP32's `/api/status` endpoint
- Tracks daily statistics in SQLite and pushes them at midnight

## Install

### 1. Flash the companion firmware first

In Arduino IDE, open `../clawd_mochi_companion/clawd_mochi_companion.ino`
and upload it to your ESP32-C3 (see the project README for board settings).

Then connect to its hotspot `ClaWD-Mochi` (password `clawd1234`) and visit
`http://192.168.4.1/wifi` to add your home WiFi. After the ESP32 connects,
note its IP address (shown on the device screen).

### 2. Install the daemon

```sh
# Requires Python 3.10+
cd clawd-daemon
pip install -e .

# Write config + install launchd/systemd service
clawd-daemon install --esp32-host 192.168.1.123   # ← your ESP32's IP

# Install hooks into ~/.claude/settings.json
clawd-daemon install-hooks

# Verify
clawd-daemon test-push --state thinking
```

The daemon now runs in the background and is auto-started on login.

## Commands

| Command                          | Purpose                                            |
| -------------------------------- | -------------------------------------------------- |
| `clawd-daemon run`               | Run in foreground (for debugging)                  |
| `clawd-daemon install`           | Install service unit + write default config        |
| `clawd-daemon uninstall`         | Remove service unit                                |
| `clawd-daemon install-hooks`     | Add hooks to `~/.claude/settings.json`             |
| `clawd-daemon test-push --state` | Send a fake status (idle/thinking/awaiting/done/error) |

## Config

Default location: `~/.config/clawd-daemon/config.toml`

```toml
esp32_host = "192.168.1.123"
esp32_port = 80
listen_host = "127.0.0.1"
listen_port = 7878
claude_dir = "~/.claude"
state_dir  = "~/.local/share/clawd-daemon"
push_interval_ms     = 1000
heartbeat_interval_s = 10

[model_token_limits]
sonnet = 200000
opus   = 200000
haiku  = 200000
```

## Architecture

```
~/.claude/settings.json (hooks)
       │
       │ on Pre/PostToolUse/Stop/Notification/UserPromptSubmit
       │ → curl POST localhost:7878/event/<type>
       ▼
clawd-daemon  (FastAPI on 127.0.0.1:7878)
  ├─ StateManager — priority-merged status
  ├─ collectors/  — git, todo, tokens
  ├─ storage.py   — SQLite daily stats
  └─ esp32_client.py — throttled HTTP push
       │
       │ POST /api/status   (real-time)
       │ POST /api/stats/daily  (every midnight)
       ▼
ESP32-C3 (http://<esp32-ip>)
```

## Development

```sh
pip install -e ".[dev]"
pytest
clawd-daemon run -v   # run in foreground with debug logs
```

## Troubleshooting

| Symptom                                  | Fix                                              |
| ---------------------------------------- | ------------------------------------------------ |
| `test-push` fails with timeout           | Check ESP32 IP in config; ping it; same network? |
| Hooks installed but ESP32 doesn't update | `systemctl --user status clawd-daemon` (Linux) / `launchctl list \| grep clawd` (mac); check the log |
| Wrong token count                        | Daemon reads from `~/.claude/sessions/*.jsonl`; format varies by Claude Code version. Report an issue with a sample line. |
| Daemon shows "offline" on screen         | Heartbeat hasn't arrived in 60s — check daemon is running and ESP32 is reachable |
