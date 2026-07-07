"""
pytest 会话级 fixture：本地模式下负责构建、安装、Docker 编排与端口就绪检查。

与 tests/run_all.sh 配合使用；业务用例通过 helpers/runtime 访问服务。
"""
from __future__ import annotations

import os
import socket
import subprocess
import time
from pathlib import Path

import pytest
import requests

from helpers.runtime import ensure_ports, require_ok, run_cmd

# ── 端口常量（单一来源：tests/ports.env）─────────────────────────────────────
# 加载 ports.env 文件，文件不存在则使用默认值；环境变量优先（k8s/CI 场景可覆盖）
def _load_ports_env() -> None:
    ports_file = Path(__file__).parent.parent / "ports.env"
    if not ports_file.exists():
        return
    for line in ports_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip()
        if key and val and key not in os.environ:
            os.environ[key] = val

_load_ports_env()

HELLO_HTTP_PORT  = int(os.environ.get("THUNDER_HELLO_HTTP_PORT",  "27006"))
INTERFACE_PORT   = int(os.environ.get("THUNDER_INTERFACE_PORT",   "27008"))
HELLO_HTTPS_PORT = int(os.environ.get("THUNDER_HELLO_HTTPS_PORT", "27043"))
HELLO_WS_PORT    = int(os.environ.get("THUNDER_HELLO_WS_PORT",    "27010"))


def _phase(msg: str) -> None:
    """会话 setup 很长且无子进程输出时，避免误以为卡死（需配合 pytest -s 才实时落到终端）。"""
    print(f"[pytest integration] {msg}", flush=True)


# 等待 Center 端路由注册（即 LOGIC 和 INTERFACE 类型节点全部就绪），以避免测试时遇到 “no tagMsgShell match LOGIC” 错误。
# 遍历一组可能的 admin 端口，尝试通过 /admin 接口分别检查 LOGIC 和 INTERFACE 两类节点均已注册。
# 若所有 admin 端口均不可达，则进一步尝试通过业务链路上的 /Interface/gentoken 生成 token 检查服务完整性。
def _wait_cluster_route_ready(timeout_s: float = 90.0) -> None:
    """
    等待 Center 侧路由注册完整（LOGIC + INTERFACE），避免业务用例首发时命中
    `no tagMsgShell match LOGIC` 的短窗口。
    """
    deadline = time.time() + timeout_s
    last_msg = "cluster route not ready"
    admin_ports = (26000, 26022, 26032)
    genkey_url = f"http://127.0.0.1:{INTERFACE_PORT}/Interface/gentoken"
    with requests.Session() as s:
        s.trust_env = False
        while time.time() < deadline:
            reachable_admin = 0
            route_ready_by_admin = False
            for p in admin_ports:
                try:
                    # 构造 admin 接口 URL
                    url = f"http://127.0.0.1:{p}/admin"
                    # 查询 LOGIC 节点
                    r_logic = s.post(url, json={"cmd": "show", "args": ["nodes", "LOGIC"]}, timeout=8)
                    # 查询 INTERFACE 节点
                    r_if = s.post(url, json={"cmd": "show", "args": ["nodes", "INTERFACE"]}, timeout=8)
                    # 查询 center 视图（GetRaftCenterJson: data=[{identify, leader, online}, ...]）
                    r_center = s.post(url, json={"cmd": "show", "args": ["center"]}, timeout=8)
                    # 检查 HTTP 状态码
                    if (
                        r_logic.status_code != 200 or
                        r_if.status_code != 200 or
                        r_center.status_code != 200
                    ):
                        last_msg = (
                            f"admin {p} status logic={r_logic.status_code} "
                            f"interface={r_if.status_code} center={r_center.status_code}"
                        )
                        continue
                    # 至少有一个 admin 可达
                    reachable_admin += 1
                    # 解析返回的 JSON，获取节点数据
                    j_logic = r_logic.json()
                    j_if = r_if.json()
                    j_center = r_center.json()
                    logic_nodes = j_logic.get("data") or []
                    interface_nodes = j_if.get("data") or []
                    center_data = j_center.get("data") or []
                    leader_id = ""
                    # 按当前 Center 实现：data=[{"identify":"...","leader":"yes|no","online":"yes"}...]
                    for item in center_data:
                        if isinstance(item, dict) and str(item.get("leader", "")).lower() == "yes":
                            leader_id = str(item.get("identify") or "")
                            if leader_id:
                                break
                    # LOGIC 和 INTERFACE 节点都已就绪且中心节点已选主
                    if logic_nodes and interface_nodes and leader_id:
                        route_ready_by_admin = True
                        break
                    # 记录节点数量情况和主节点情况，便于后续报错定位
                    last_msg = (
                        f"admin {p} route not ready: "
                        f"LOGIC={len(logic_nodes)} INTERFACE={len(interface_nodes)} LEADER={'yes' if leader_id else 'no'}"
                    )
                except Exception as e:  # noqa: BLE001
                    # 多 Center 场景下部分 admin 端口可能未开启，不应因单点不可达直接失败。
                    last_msg = f"admin {p} not reachable: {e}"
            if route_ready_by_admin:
                return
            if reachable_admin == 0:
                # 某些环境不开放 /admin，改以业务链路可用性作为“启动完整”判据。
                try:
                    r = s.post(genkey_url, json={"option": "GenKey"}, timeout=8)
                    if r.status_code == 200:
                        j = r.json()
                        if j.get("token") and j.get("key") and str(j.get("msg", "")) == "success":
                            return
                        last_msg = f"genkey not ready: msg={j.get('msg')} code={j.get('code')}"
                    else:
                        last_msg = f"genkey http status={r.status_code}"
                except Exception as e:  # noqa: BLE001
                    last_msg = f"genkey check error: {e}"
            time.sleep(1.0)
    raise AssertionError(f"cluster route not ready within {timeout_s:.0f}s: {last_msg}")


def pytest_addoption(parser: pytest.Parser) -> None:
    """注册命令行选项（test_all.sh 通过 pytest 透传）。"""
    parser.addoption("--mode", action="store", default="local", choices=["local", "external"])
    parser.addoption("--keep-docker", action="store_true", default=False)
    parser.addoption("--skip-build", action="store_true", default=False)


@pytest.fixture(scope="session")
def mode(pytestconfig: pytest.Config) -> str:
    """当前运行模式：local=本机起栈；external=假定环境已在外部准备好。"""
    return str(pytestconfig.getoption("--mode"))


@pytest.fixture(scope="session", autouse=True)
def docker_stack(mode: str, pytestconfig: pytest.Config) -> None:
    """
    本地模式下一键拉起/ teardown Docker 编排栈（docker/）。

    步骤概览：
      1) external：不建栈，直接返回。
      2) 非 skip-build：cmake 配置 + 全量编译 + install（产出供镜像/挂载使用）。
      3) compose down 清场 → compose build/up 起服务。
      4) 会话结束：默认 compose down；--keep-docker 则保留容器便于排障。
    """
    # ---------- 步骤 1：external 模式跳过 Docker，由外部环境自备服务 ----------
    if mode == "external":
        _phase("mode=external：跳过 cmake / docker compose（请自行保证服务已起）")
        yield
        return

    _phase("会话 setup：开始（本地模式会先全量构建 + docker compose，可能数分钟且无子进程输出）…")
    repo = Path(__file__).resolve().parents[2]
    dd = repo / "docker"
    # 关闭 BuildKit：部分环境 buildx/缓存异常时更稳定（与仓库 docker 脚本保持一致）
    compose_env = {
        "DOCKER_BUILDKIT": "0",
        "COMPOSE_DOCKER_CLI_BUILD": "0",
    }

    # ---------- 步骤 2：构建并安装到约定前缀（与 dev_up_logs.sh 思路一致）----------
    if not bool(pytestconfig.getoption("--skip-build")):
        build_dir = repo / "build"
        _phase("cmake 配置…")
        cp_cfg = run_cmd(
            ["cmake", "-S", str(repo), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
            cwd=repo,
        )
        require_ok(cp_cfg, "cmake configure")
        jobs = os.getenv("BUILD_JOBS", "1")
        _phase(f"cmake --build -j {jobs}（最耗时，请等待）…")
        cp_build_all = run_cmd(["cmake", "--build", str(build_dir), "-j", jobs], cwd=repo)
        require_ok(cp_build_all, "cmake build")
        _phase("cmake --install…")
        cp_install = run_cmd(["cmake", "--install", str(build_dir)], cwd=repo)
        require_ok(cp_install, "cmake install")
    else:
        _phase("已 --skip-build：跳过 cmake 构建与 install")

    # ---------- 步骤 3：Docker Compose 清场、构建镜像、后台启动 ----------
    _phase("docker compose down（清场）…")
    _ = run_cmd(["docker", "compose", "down", "--remove-orphans"], cwd=dd, env=compose_env)
    _phase("docker compose build（可能较慢）…")
    cp_build = run_cmd(["docker", "compose", "build"], cwd=dd, env=compose_env)
    # build 若因 buildx 报错，退化用已有镜像 up（避免 CI/旧 Docker 直接挂死）
    if cp_build.returncode != 0 and "docker-buildx" in (cp_build.stderr or ""):
        _phase("compose build 遇 buildx 问题，改用 up --no-build…")
        cp_up = run_cmd(["docker", "compose", "up", "-d", "--no-build"], cwd=dd, env=compose_env)
        require_ok(cp_up, "docker compose up -d --no-build")
    else:
        require_ok(cp_build, "docker compose build")
        _phase("docker compose up -d…")
        cp_up = run_cmd(["docker", "compose", "up", "-d"], cwd=dd, env=compose_env)
        require_ok(cp_up, "docker compose up -d")
    _phase("Docker 编排已启动，即将等待端口就绪")

    # ---------- 步骤 4：会话 teardown —— 默认销毁栈；keep-docker 则保留 ----------
    keep = bool(pytestconfig.getoption("--keep-docker"))
    if not keep:
        yield
        run_cmd(["docker", "compose", "down", "--remove-orphans"], cwd=dd, env=compose_env)
    else:
        yield


@pytest.fixture(scope="session", autouse=True)
def service_readiness(mode: str, docker_stack: object) -> None:
    """
    等待业务与依赖端口就绪，避免用例刚开跑就连不上。

    依赖 docker_stack：保证 compose 起来后才轮询端口。
    - 端口来自 tests/ports.env（THUNDER_HELLO_HTTP_PORT / THUNDER_INTERFACE_PORT / THUNDER_HELLO_HTTPS_PORT）。
    - local 额外等 6379（Redis）、3306（MySQL），与 compose 内依赖一致。
    """
    del docker_stack  # 仅用于声明 fixture 顺序

    # 检测宿主机 IP（Docker host network 模式下端口可能绑定在宿主机 IP 上）
    host_ips = []
    # 优先用 E2E_HOST 环境变量（由 run_all.sh 传入）
    env_host = os.getenv("E2E_HOST")
    if env_host:
        host_ips.append(env_host)
    host_ips.append("127.0.0.1")
    try:
        out = subprocess.check_output(["hostname", "-I"], text=True).strip()
        for ip in out.split():
            ip = ip.strip()
            if ip and ip not in host_ips and not ip.startswith("127."):
                host_ips.append(ip)
    except Exception:
        pass

    _phase(f"等待端口 {HELLO_HTTP_PORT}/{INTERFACE_PORT}/{HELLO_HTTPS_PORT} 就绪（hosts={host_ips}，每项最长约 90s）…")
    for host in host_ips:
        try:
            ensure_ports(host, [HELLO_HTTP_PORT, INTERFACE_PORT, HELLO_HTTPS_PORT], timeout_s=90.0)
            break  # 某个 host 全部端口就绪
        except AssertionError:
            continue
    else:
        raise AssertionError(f"port not ready on any host: {host_ips}")
    if mode == "local":
        _phase("等待 Redis/MySQL 6379 / 3306 就绪（每项最长约 120s）…")
        ensure_ports("127.0.0.1", [6379, 3306], timeout_s=120.0)
        _phase("等待 Center 路由注册完成（LOGIC + INTERFACE）…")
        _wait_cluster_route_ready(timeout_s=90.0)
    _phase("端口就绪，开始跑用例")


@pytest.fixture(scope="session")
def https_verify() -> str | bool:
    """HTTPS 用例校验服务端证书：有 ca.crt 则走 verify=路径，否则关闭校验（仅测试环境）。"""
    from pathlib import Path

    cert = Path(__file__).resolve().parents[2] / "deploy" / "HelloHttps" / "conf" / "certs" / "ca.crt"
    return str(cert) if cert.exists() else False


@pytest.fixture(scope="session")
def proxyless_env() -> dict[str, str]:
    """子进程环境副本，去掉常见代理变量，避免请求走 SOCKS/公司代理导致联调失败。"""
    env = os.environ.copy()
    for k in ("ALL_PROXY", "all_proxy", "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"):
        env.pop(k, None)
    return env


@pytest.fixture(scope="function")
def http_session() -> requests.Session:
    """requests 会话（function scope：避免跨测试共享过期连接）。"""
    s = requests.Session()
    # 强制忽略 HTTP(S)_PROXY / ALL_PROXY
    s.trust_env = False
    return s
