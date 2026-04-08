from __future__ import annotations

import os
import socket
import subprocess
import time
from pathlib import Path
from typing import Iterable


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def docker_dir() -> Path:
    return repo_root() / "deploy" / "docker"


def run_cmd(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=merged, text=True, capture_output=True, check=False)


def require_ok(cp: subprocess.CompletedProcess[str], title: str) -> None:
    if cp.returncode != 0:
        raise AssertionError(f"{title} failed({cp.returncode})\nSTDOUT:\n{cp.stdout}\nSTDERR:\n{cp.stderr}")


def wait_port(host: str, port: int, timeout_s: float = 40.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(1.0)
            if s.connect_ex((host, port)) == 0:
                return
        time.sleep(0.5)
    raise AssertionError(f"port not ready: {host}:{port}")


def ensure_ports(host: str, ports: Iterable[int], timeout_s: float = 60.0) -> None:
    for p in ports:
        wait_port(host, p, timeout_s)

