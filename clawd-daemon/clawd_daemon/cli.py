"""CLI entrypoint for `clawd-daemon`.

Supports:
    clawd-daemon run         — run the daemon in the foreground
    clawd-daemon install     — install launchd/systemd unit + write config
    clawd-daemon install-hooks — write hook config into ~/.claude/settings.json
    clawd-daemon test-push   — send a fake status to the ESP32 for visual test
    clawd-daemon uninstall   — remove the service unit
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from clawd_daemon import __version__, config, server


def _setup_logging(verbose: bool) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        level=level,
    )


# ---------- run ----------

def cmd_run(args: argparse.Namespace) -> int:
    import uvicorn  # local to keep CLI import-light

    cfg = config.load(args.config)
    server.daemon = server.Daemon(cfg)
    uvicorn.run(
        server.app,
        host=cfg.listen_host,
        port=cfg.listen_port,
        log_level="info" if not args.verbose else "debug",
        access_log=False,
    )
    return 0


# ---------- install ----------

LAUNCHD_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.clawd.mochi-daemon</string>
  <key>ProgramArguments</key>
  <array>
    <string>{exec}</string>
    <string>run</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>{log}</string>
  <key>StandardErrorPath</key><string>{log}</string>
</dict></plist>
"""

SYSTEMD_UNIT = """[Unit]
Description=Clawd Mochi Daemon (Claude Code bridge)
After=network.target

[Service]
ExecStart={exec} run
Restart=always
RestartSec=2

[Install]
WantedBy=default.target
"""


def cmd_install(args: argparse.Namespace) -> int:
    # 1. Write default config if not present
    cfg_path = config.DEFAULT_PATH
    if not cfg_path.exists() or args.overwrite_config:
        config.write_default_config(cfg_path, esp32_host=args.esp32_host)
        print(f"wrote config → {cfg_path}")
    else:
        print(f"config exists, kept → {cfg_path}  (use --overwrite-config to replace)")

    exec_path = shutil.which("clawd-daemon") or sys.argv[0]

    system = platform.system()
    if system == "Darwin":
        plist_dir = Path("~/Library/LaunchAgents").expanduser()
        plist_dir.mkdir(parents=True, exist_ok=True)
        plist_path = plist_dir / "com.clawd.mochi-daemon.plist"
        log_path = (
            Path("~/Library/Logs/clawd-daemon.log").expanduser()
        )
        log_path.parent.mkdir(parents=True, exist_ok=True)
        plist_path.write_text(
            LAUNCHD_PLIST.format(exec=exec_path, log=str(log_path))
        )
        print(f"wrote launchd → {plist_path}")
        subprocess.run(["launchctl", "unload", str(plist_path)], check=False)
        subprocess.run(["launchctl", "load", str(plist_path)], check=False)
        print(f"loaded; logs at {log_path}")
        return 0

    if system == "Linux":
        unit_dir = Path("~/.config/systemd/user").expanduser()
        unit_dir.mkdir(parents=True, exist_ok=True)
        unit_path = unit_dir / "clawd-daemon.service"
        unit_path.write_text(SYSTEMD_UNIT.format(exec=exec_path))
        print(f"wrote systemd unit → {unit_path}")
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=False)
        subprocess.run(
            ["systemctl", "--user", "enable", "--now", "clawd-daemon.service"],
            check=False,
        )
        print("enabled & started; check with: systemctl --user status clawd-daemon")
        return 0

    print(f"unsupported OS '{system}'. Run manually: clawd-daemon run")
    return 1


def cmd_uninstall(args: argparse.Namespace) -> int:
    system = platform.system()
    if system == "Darwin":
        plist_path = Path("~/Library/LaunchAgents/com.clawd.mochi-daemon.plist").expanduser()
        if plist_path.exists():
            subprocess.run(["launchctl", "unload", str(plist_path)], check=False)
            plist_path.unlink()
            print(f"removed {plist_path}")
    elif system == "Linux":
        subprocess.run(["systemctl", "--user", "disable", "--now", "clawd-daemon.service"], check=False)
        unit_path = Path("~/.config/systemd/user/clawd-daemon.service").expanduser()
        if unit_path.exists():
            unit_path.unlink()
            print(f"removed {unit_path}")
    return 0


# ---------- install-hooks ----------

HOOK_CONFIG = {
    "hooks": {
        "PreToolUse": [
            {"hooks": [{"type": "command",
                        "command": "curl -sX POST http://127.0.0.1:7878/event/pre_tool -H 'Content-Type: application/json' -d @-"}]}
        ],
        "PostToolUse": [
            {"hooks": [{"type": "command",
                        "command": "curl -sX POST http://127.0.0.1:7878/event/post_tool -H 'Content-Type: application/json' -d @-"}]}
        ],
        "Stop": [
            {"hooks": [{"type": "command",
                        "command": "curl -sX POST http://127.0.0.1:7878/event/stop -H 'Content-Type: application/json' -d @-"}]}
        ],
        "Notification": [
            {"hooks": [{"type": "command",
                        "command": "curl -sX POST http://127.0.0.1:7878/event/notification -H 'Content-Type: application/json' -d @-"}]}
        ],
        "UserPromptSubmit": [
            {"hooks": [{"type": "command",
                        "command": "curl -sX POST http://127.0.0.1:7878/event/prompt -H 'Content-Type: application/json' -d @-"}]}
        ],
    }
}


def cmd_install_hooks(args: argparse.Namespace) -> int:
    settings_path = Path(args.settings_path or "~/.claude/settings.json").expanduser()
    settings_path.parent.mkdir(parents=True, exist_ok=True)

    existing = {}
    if settings_path.exists():
        try:
            existing = json.loads(settings_path.read_text())
        except json.JSONDecodeError:
            print(f"WARNING: {settings_path} is not valid JSON; refusing to overwrite")
            return 1

    existing_hooks = existing.get("hooks", {})
    for evt, conf in HOOK_CONFIG["hooks"].items():
        existing_hooks.setdefault(evt, [])
        # Check if our hook is already installed
        already = any(
            "clawd" in str(h) or "7878" in str(h)
            for entry in existing_hooks[evt] for h in entry.get("hooks", [])
        )
        if not already:
            existing_hooks[evt].extend(conf)
    existing["hooks"] = existing_hooks
    settings_path.write_text(json.dumps(existing, indent=2))
    print(f"hooks installed → {settings_path}")
    print("restart any running Claude Code session for hooks to take effect.")
    return 0


# ---------- test-push ----------

def cmd_test_push(args: argparse.Namespace) -> int:
    import httpx

    cfg = config.load(args.config)
    payload = {
        "state": args.state,
        "tool": "Edit",
        "task": "Test push from clawd-daemon CLI",
        "duration_s": 7,
        "tokens_used": 45_000,
        "tokens_max": 200_000,
        "git_branch": "main",
        "project": "clawd-mochi",
    }
    try:
        r = httpx.post(f"{cfg.esp32_url}/api/status", json=payload, timeout=3.0)
        print(f"HTTP {r.status_code}: {r.text}")
        return 0 if r.is_success else 1
    except httpx.HTTPError as e:
        print(f"push failed: {e}")
        return 1


# ---------- main ----------

def main() -> int:
    p = argparse.ArgumentParser(prog="clawd-daemon", description=__doc__)
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("--verbose", "-v", action="store_true")
    p.add_argument("--config", type=Path, default=None)
    sub = p.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="run the daemon in the foreground")
    p_run.set_defaults(func=cmd_run)

    p_ins = sub.add_parser("install", help="install launchd/systemd unit")
    p_ins.add_argument("--esp32-host", default="192.168.4.1",
                       help="IP or hostname of the ESP32")
    p_ins.add_argument("--overwrite-config", action="store_true")
    p_ins.set_defaults(func=cmd_install)

    p_un = sub.add_parser("uninstall", help="remove launchd/systemd unit")
    p_un.set_defaults(func=cmd_uninstall)

    p_hk = sub.add_parser("install-hooks",
                          help="install Claude Code hooks into settings.json")
    p_hk.add_argument("--settings-path", default=None)
    p_hk.set_defaults(func=cmd_install_hooks)

    p_tp = sub.add_parser("test-push", help="send a fake status to the ESP32")
    p_tp.add_argument("--state", default="thinking",
                      choices=["idle", "thinking", "awaiting", "done", "error"])
    p_tp.set_defaults(func=cmd_test_push)

    args = p.parse_args()
    _setup_logging(args.verbose)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())