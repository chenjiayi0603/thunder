from __future__ import annotations

import os
from pathlib import Path

import pytest
import requests

from helpers.runtime import ensure_ports, require_ok, run_cmd


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--mode", action="store", default="local", choices=["local", "external"])
    parser.addoption("--keep-docker", action="store_true", default=False)
    parser.addoption("--skip-build", action="store_true", default=False)


@pytest.fixture(scope="session")
def mode(pytestconfig: pytest.Config) -> str:
    return str(pytestconfig.getoption("--mode"))


@pytest.fixture(scope="session", autouse=True)
def docker_stack(mode: str, pytestconfig: pytest.Config) -> None:
    if mode == "external":
        return
    repo = Path(__file__).resolve().parents[3]
    dd = repo / "deploy" / "docker"
    compose_env = {
        "DOCKER_BUILDKIT": "0",
        "COMPOSE_DOCKER_CLI_BUILD": "0",
    }
    # 先保证 deploy 产物与插件已安装（等价过去 dev_up_logs.sh 的关键步骤）。
    if not bool(pytestconfig.getoption("--skip-build")):
        build_dir = repo / "build"
        cp_cfg = run_cmd(
            ["cmake", "-S", str(repo), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
            cwd=repo,
        )
        require_ok(cp_cfg, "cmake configure")
        jobs = os.getenv("BUILD_JOBS", "1")
        cp_build_all = run_cmd(["cmake", "--build", str(build_dir), "-j", jobs], cwd=repo)
        require_ok(cp_build_all, "cmake build")
        cp_install = run_cmd(["cmake", "--install", str(build_dir)], cwd=repo)
        require_ok(cp_install, "cmake install")

    _ = run_cmd(["docker", "compose", "down", "--remove-orphans"], cwd=dd, env=compose_env)
    cp_build = run_cmd(["docker", "compose", "build"], cwd=dd, env=compose_env)
    if cp_build.returncode != 0 and "docker-buildx" in (cp_build.stderr or ""):
        cp_up = run_cmd(["docker", "compose", "up", "-d", "--no-build"], cwd=dd, env=compose_env)
        require_ok(cp_up, "docker compose up -d --no-build")
    else:
        require_ok(cp_build, "docker compose build")
        cp_up = run_cmd(["docker", "compose", "up", "-d"], cwd=dd, env=compose_env)
        require_ok(cp_up, "docker compose up -d")

    keep = bool(pytestconfig.getoption("--keep-docker"))
    if not keep:
        yield
        run_cmd(["docker", "compose", "down", "--remove-orphans"], cwd=dd, env=compose_env)
    else:
        yield


@pytest.fixture(scope="session", autouse=True)
def service_readiness(mode: str) -> None:
    # local 或 external 都做 readiness 探测（external 失败即明确提示环境未就绪）
    ensure_ports("127.0.0.1", [27006, 27008, 27443], timeout_s=90.0)
    if mode == "local":
        ensure_ports("127.0.0.1", [6379, 3306], timeout_s=120.0)


@pytest.fixture(scope="session")
def https_verify() -> str | bool:
    from pathlib import Path

    cert = Path(__file__).resolve().parents[2] / "HelloHttps" / "conf" / "certs" / "ca.crt"
    return str(cert) if cert.exists() else False


@pytest.fixture(scope="session")
def proxyless_env() -> dict[str, str]:
    env = os.environ.copy()
    for k in ("ALL_PROXY", "all_proxy", "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"):
        env.pop(k, None)
    return env


@pytest.fixture(scope="session")
def http_session() -> requests.Session:
    s = requests.Session()
    # 强制忽略 HTTP(S)_PROXY / ALL_PROXY，避免 SOCKS 依赖。
    s.trust_env = False
    return s

