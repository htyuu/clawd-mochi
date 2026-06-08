"""Daemon configuration — loaded from ~/.config/clawd-daemon/config.toml."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

if sys.version_info >= (3, 11):
    import tomllib
else:
    import tomli as tomllib

DEFAULT_PATH = Path(os.path.expanduser("~/.config/clawd-daemon/config.toml"))

# Model → context token limits (used to compute progress bar fill).
DEFAULT_MODEL_LIMITS = {
    "sonnet": 200_000,
    "opus":   200_000,
    "haiku":  200_000,
}


@dataclass
class Config:
    esp32_host: str = "192.168.4.1"
    esp32_port: int = 80
    listen_host: str = "127.0.0.1"
    listen_port: int = 7878
    claude_dir: Path = Path("~/.claude").expanduser()
    state_dir: Path = Path("~/.local/share/clawd-daemon").expanduser()
    push_interval_ms: int = 1000
    heartbeat_interval_s: int = 10
    model_token_limits: dict[str, int] = field(
        default_factory=lambda: dict(DEFAULT_MODEL_LIMITS)
    )

    @property
    def esp32_url(self) -> str:
        return f"http://{self.esp32_host}:{self.esp32_port}"


def load(path: Path | None = None) -> Config:
    path = path or DEFAULT_PATH
    if not path.exists():
        return Config()
    with path.open("rb") as fh:
        raw = tomllib.load(fh)
    cfg = Config()
    for key in (
        "esp32_host", "esp32_port",
        "listen_host", "listen_port",
        "push_interval_ms", "heartbeat_interval_s",
    ):
        if key in raw:
            setattr(cfg, key, raw[key])
    if "claude_dir" in raw:
        cfg.claude_dir = Path(os.path.expanduser(raw["claude_dir"]))
    if "state_dir" in raw:
        cfg.state_dir = Path(os.path.expanduser(raw["state_dir"]))
    if "model_token_limits" in raw:
        cfg.model_token_limits.update(raw["model_token_limits"])
    cfg.state_dir.mkdir(parents=True, exist_ok=True)
    return cfg


def write_default_config(path: Path | None = None, esp32_host: str = "192.168.4.1") -> Path:
    """Write a starter config file. Called by `clawd-daemon install`."""
    path = path or DEFAULT_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    content = f'''# clawd-daemon configuration
# Edit esp32_host after you connect your ESP32 to your home WiFi
# (find the IP on the device screen or via your router).

esp32_host = "{esp32_host}"
esp32_port = 80

listen_host = "127.0.0.1"
listen_port = 7878

claude_dir = "~/.claude"
state_dir  = "~/.local/share/clawd-daemon"

push_interval_ms   = 1000   # min ms between status pushes
heartbeat_interval_s = 10   # heartbeat when nothing changes

[model_token_limits]
sonnet = 200000
opus   = 200000
haiku  = 200000
'''
    path.write_text(content)
    return path
