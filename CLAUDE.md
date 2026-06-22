# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Clawd Mochi is a physical DIY desk companion — an ESP32-C3 driving a 1.54" 240×240 SPI TFT (ST7789) inside a 3D-printed crab case. Two firmware versions exist side-by-side, plus a Python daemon that bridges Claude Code hooks to the device.

## Repository layout (non-obvious)

```
clawd_mochi/clawd_mochi.ino              ← ORIGINAL firmware (DO NOT MODIFY)
clawd_mochi_companion/clawd_mochi_companion.ino  ← COMPANION firmware (active)
clawd-daemon/clawd_daemon/               ← Python daemon package
```

The companion firmware was **copied from the original** as a starting point, then extended. The original must remain untouched — users choose which `.ino` to flash in Arduino IDE.

## Building / flashing / testing

### ESP32 firmware

- **IDE**: Arduino IDE 2.x with ESP32 board support (Espressif Systems)
- **Board settings**: `ESP32C3 Dev Module`, USB CDC On Boot: **Enabled**, 160 MHz, 921600 upload speed
- **Required libraries**: Adafruit GFX Library, Adafruit ST7789 Library, **ArduinoJson** (companion only)
- **Upload**: Open the `.ino` in Arduino IDE → Tools → Port → select ESP32 → Upload
- **Companion extra libraries**: `WiFi.h`, `WiFiMulti.h`, `WebServer.h`, `DNSServer.h`, `ArduinoOTA.h`, `Preferences.h`, `ArduinoJson.h`
- **There is no compiler/CI for the firmware** — it's a single `.ino` file, edited and uploaded manually.

### Python daemon

```bash
conda activate clawd
cd clawd-daemon
pip install -e ".[dev]"
pytest                          # 13 tests, should all pass
clawd-daemon run                # foreground, for debugging
clawd-daemon test-push --state thinking  # visual test against real ESP32
```

### Key daemon CLI commands

| Command | Purpose |
|---------|---------|
| `clawd-daemon run` | Foreground (debug) |
| `clawd-daemon install --esp32-host <IP>` | Install launchd/systemd service + write config |
| `clawd-daemon install-hooks` | Add hook entries to `~/.claude/settings.json` |
| `clawd-daemon test-push --state <s>` | Send fake status (idle/thinking/awaiting/done/error) |
| `clawd-daemon uninstall` | Remove service |

## Architecture: data flow

```
Claude Code (PreToolUse/PostToolUse/Stop/Notification/UserPromptSubmit hooks)
  → curl POST localhost:7878/event/<type>
    → Daemon (FastAPI + StateManager)
      → StateManager.apply()  ← priority merging
      → Esp32Client.push_status()  ← throttled HTTP POST
        → ESP32 /api/status
          → applyStatus()  ← priority check on device too
          → drawStatusView()  ← renders on TFT
```

## State machine (both sides)

**ESP32 side** (`ClaudeState` enum in `.ino`):
- `CS_IDLE=0, CS_THINKING=1, CS_DONE=2, CS_AWAITING=3, CS_ERROR=4`
- Higher numeric = higher priority
- `applyStatus()` now implements priority merging: low-priority incoming states don't override transient high-priority ones (ERROR/AWAITING/DONE). They're saved as `pendingStatus` and applied after timeout.
- Timeouts: ERROR 5s, DONE 3s, daemon offline 60s

**Daemon side** (`state.py`):
- Same enum (`ClaudeState`)
- `StateManager.apply()` also merges — lower priority incoming only updates non-state fields
- `ClaudeStatus.to_payload()` serializes to JSON for ESP32 `/api/status`

## Views (companion firmware)

0. `VIEW_EYES_NORMAL` — pixel eyes with wiggle+blink
1. `VIEW_EYES_SQUISH` — `> <` chevron eyes
2. `VIEW_CODE` — Claude Code display + terminal mode
3. `VIEW_DRAW` — canvas (phone drawing syncs to TFT)
4. `VIEW_STATUS` — Claude work status (git, tool, task, token bar, mood-light bg, expression rotation)
5. `VIEW_SCREENSAVER` — day/night cycle (2 min idle auto-entry, any activity exits)

## Key constants (companion firmware)

- Pins: `TFT_CS=4, TFT_DC=1, TFT_RST=2, TFT_BLK=3`, SPI `SCK=8, MOSI=10`
- `SCREENSAVER_TIMEOUT_MS = 120000` (2 min)
- `EXPRESSION_INTERVAL_MS = 15000` (15s between idle expressions)
- `MAX_WIFI_CREDS = 5`
- `DAEMON_OFFLINE_TIMEOUT_MS = 60000`

## WiFi

- **AP**: always on, SSID `ClaWD-Mochi`, password `clawd1234`, IP `192.168.4.1`
- **STA**: WiFiMulti, up to 5 saved networks in NVS, auto-select strongest
- **Captive portal**: DNSServer redirects all DNS to `192.168.4.1` on AP

## Daemon data collectors

- `collectors/git_info.py` — subprocess git branch + project name
- `collectors/todo_reader.py` — reads `~/.claude/todos/<session>.json` for active task
- `collectors/token_counter.py` — reads tail of `~/.claude/sessions/<session>.jsonl`, returns `(tokens_used, tokens_max, model_name)` 3-tuple
- `storage.py` — SQLite (`tool_events` + `daily_stats`), midnight aggregation

## NVS namespaces (ESP32 Preferences)

- `wifi` — `count`, `s0..s4`, `p0..p4`
- `clawd` — `stats_date`, `stats_tools`, `stats_tokens`, `stats_sessions`, `stats_errors`, `mood`, `autosw`

## Don't commit automatically

The user prefers to review and commit manually. Stage changes but wait for explicit `commit` command.
