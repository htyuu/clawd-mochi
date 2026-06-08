"""Read git branch + project name from a working directory.

Falls back gracefully if the directory isn't a git repo.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass
class GitInfo:
    branch: str = ""
    project: str = ""


def collect(cwd: str | Path | None) -> GitInfo:
    if not cwd:
        return GitInfo()
    cwd_path = Path(cwd).expanduser()
    if not cwd_path.exists():
        return GitInfo(project=Path(str(cwd)).name)

    info = GitInfo(project=cwd_path.name)
    try:
        out = subprocess.run(
            ["git", "-C", str(cwd_path), "branch", "--show-current"],
            capture_output=True, text=True, timeout=1.0,
        )
        if out.returncode == 0:
            info.branch = out.stdout.strip()
        # If we're in a subdir of a repo, prefer the repo's name as project
        rep = subprocess.run(
            ["git", "-C", str(cwd_path), "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, timeout=1.0,
        )
        if rep.returncode == 0:
            top = rep.stdout.strip()
            if top:
                info.project = Path(top).name
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return info
