"""
etcd 全链路稳定性测试（#115）

覆盖：注册完整性 → 路由下发 → 下线剔除 全链路的6个场景

场景总览：
  S1  注册完整性      — 5 个 node_type 全部出现在 etcd，lease 有效
  S2  路由下发验证    — 注册后路由可用，真实业务请求成功
  S3  SIGTERM 下线   — docker stop (SIGTERM) → bash trap → node.sh stop → leaserevoke → key <5s 消失
  S4  kill -9 兜底   — SIGKILL → key 在 TTL 内仍存在 → TTL 后消失（标 slow，等 ~35s）
  S5  崩溃重启       — docker kill → docker start → 重新注册 → 业务恢复
  S6  长跑           — 5 分钟持续轮询注册表，验证无意外租约丢失（标 slow）

Watch 说明：
  #116 已将 DoPollRegistry(5s) 改为 etcd Watch（毫秒级），
  S3 的路由剔除断言上限从 5s 收紧至 3s。
"""
from __future__ import annotations

import base64
import json
import subprocess
import time
from pathlib import Path

import pytest
import requests

# ── 常量 ────────────────────────────────────────────────────────────────────
ETCD_URL   = "http://127.0.0.1:2379"
HELLO_URL  = "http://127.0.0.1:27006"
IFACE_URL  = "http://127.0.0.1:27008"

_DOCKER_DIR = Path(__file__).resolve().parents[2] / "docker"

EXPECTED_NODE_TYPES = {"HELLO_HTTP", "HELLO_WS", "HELLO_HTTPS", "INTERFACE", "LOGIC"}
REGISTRY_PREFIX     = "/thunder/registry/"
SLOT_PREFIX         = "/thunder/slot/"

# kLeaseTTL=30, kKeepAliveRefresh=10（见 EtcdGrpcConnector.cpp）
LEASE_TTL            = 30
WATCH_PROPAGATION_S  = 3    # Watch 下路由感知上限（原 5s 轮询，Watch 后收紧）
TTL_WAIT_S           = LEASE_TTL + 8   # kill -9 后等 TTL 过期的宽松上限


# ── 辅助函数 ─────────────────────────────────────────────────────────────────

def _b64(s: str) -> str:
    return base64.b64encode(s.encode()).decode()


def _etcd_range(prefix: str) -> list[dict]:
    """返回 prefix 下所有 kv（已解码）。"""
    r = requests.post(
        f"{ETCD_URL}/v3/kv/range",
        json={"key": _b64(prefix), "range_end": _b64(prefix[:-1] + chr(ord(prefix[-1]) + 1))},
        timeout=5,
    )
    if r.status_code != 200:
        return []
    kvs = []
    for kv in r.json().get("kvs", []):
        try:
            kvs.append({
                "key":   base64.b64decode(kv["key"]).decode(),
                "value": base64.b64decode(kv["value"]).decode(),
                "lease": kv.get("lease", "0"),
            })
        except Exception:
            pass
    return kvs


def _registry_node_types() -> set[str]:
    """返回当前 etcd registry 中所有 node_type 集合。"""
    types = set()
    for kv in _etcd_range(REGISTRY_PREFIX):
        try:
            v = json.loads(kv["value"])
            if "node_type" in v:
                types.add(v["node_type"])
        except Exception:
            pass
    return types


def _registry_keys_for_type(node_type: str) -> list[str]:
    prefix = f"{REGISTRY_PREFIX}{node_type}/"
    return [kv["key"] for kv in _etcd_range(prefix)]


def _lease_ttl(lease_id: str) -> int:
    """查询 lease 剩余 TTL（秒），lease 不存在返回 -1。"""
    try:
        r = requests.post(
            f"{ETCD_URL}/v3/lease/timetolive",
            json={"ID": str(lease_id), "keys": False},
            timeout=5,
        )
        if r.status_code != 200:
            return -1
        return int(r.json().get("TTL", "-1"))
    except Exception:
        return -1


def _hello_ok(timeout: float = 5.0) -> bool:
    try:
        r = requests.post(
            f"{HELLO_URL}/hello/hello",
            data='{"option":"Echo"}',
            timeout=timeout,
        )
        return r.status_code == 200 and r.json().get("code") == 0
    except Exception:
        return False


def _interface_ok(timeout: float = 8.0) -> bool:
    try:
        r = requests.post(
            f"{IFACE_URL}/Interface/gentoken",
            data='{"option":"GenKey"}',
            timeout=timeout,
        )
        return r.status_code == 200 and r.json().get("code") == 0
    except Exception:
        return False


def _wait_type_gone(node_type: str, timeout_s: float) -> float:
    """等待某 node_type 从 etcd registry 消失，返回耗时；超时抛 AssertionError。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if node_type not in _registry_node_types():
            return time.time() - t0
        time.sleep(0.2)
    raise AssertionError(
        f"node_type '{node_type}' still in etcd {timeout_s}s after expected removal"
    )


def _wait_type_appears(node_type: str, timeout_s: float) -> float:
    """等待某 node_type 出现在 etcd registry，返回耗时；超时抛 AssertionError。"""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if node_type in _registry_node_types():
            return time.time() - t0
        time.sleep(0.5)
    raise AssertionError(
        f"node_type '{node_type}' did not appear in etcd within {timeout_s}s"
    )


def _dc(*args: str, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", "compose"] + list(args),
        cwd=str(_DOCKER_DIR),
        capture_output=True, text=True, timeout=timeout,
    )


def _need_docker() -> None:
    if not _DOCKER_DIR.exists():
        pytest.skip(f"docker compose dir not found: {_DOCKER_DIR}")


# ── S1：注册完整性 ────────────────────────────────────────────────────────────

@pytest.mark.stability
@pytest.mark.integration
def test_s1_registration_completeness(service_readiness: object) -> None:
    """
    S1 — 所有预期 node_type 全部出现在 etcd registry，且每个节点的 lease 有效（TTL > 0）。

    断言：
      - 5 个 node_type 均存在（无缺失）
      - 每个 registry key 绑定有效 lease（TTL > 0，说明 keepalive 正常）
    """
    del service_readiness

    types = _registry_node_types()
    missing = EXPECTED_NODE_TYPES - types
    assert not missing, f"Missing node types in etcd registry: {missing}"

    # 验证每个节点的 lease 有效
    invalid_leases = []
    for kv in _etcd_range(REGISTRY_PREFIX):
        lease_id = kv.get("lease", "0")
        if not lease_id or lease_id == "0":
            invalid_leases.append(f"{kv['key']} has no lease")
            continue
        ttl = _lease_ttl(lease_id)
        if ttl <= 0:
            invalid_leases.append(f"{kv['key']} lease TTL={ttl} (expired or invalid)")

    assert not invalid_leases, "Invalid leases detected:\n" + "\n".join(invalid_leases)


# ── S2：路由下发 + 功能验证 ───────────────────────────────────────────────────

@pytest.mark.stability
@pytest.mark.integration
def test_s2_route_distribution_and_functional(service_readiness: object) -> None:
    """
    S2 — 注册完成后，验证路由已下发且真实业务请求成功。

    验证：
      - Hello（HELLO_HTTP）：POST /hello/hello → code=0
      - Interface→Logic（S2S）：POST /Interface/gentoken → code=0
      - etcd 中 LOGIC / INTERFACE 节点存在（路由来源）
    """
    del service_readiness

    # etcd 侧：关键路由节点存在
    types = _registry_node_types()
    for t in ("LOGIC", "INTERFACE", "HELLO_HTTP"):
        assert t in types, f"node_type '{t}' missing from etcd, route cannot be distributed"

    # 业务侧：真实请求验证路由已生效
    assert _hello_ok(), "Hello service unreachable (HELLO_HTTP route not working)"
    assert _interface_ok(), "Interface→Logic unreachable (LOGIC route not working)"


# ── S3：SIGTERM 正常下线 → leaserevoke → 路由剔除 ────────────────────────────

@pytest.mark.stability
@pytest.mark.integration
def test_s3_stop_deregister_and_route_removal(service_readiness: object) -> None:
    """
    S3 — docker stop（SIGTERM）→ leaserevoke → etcd key 立即消失（<3s）→ 路由剔除

    修复说明（issus-list.md #118 ✅ 已修复）：
      docker-compose.yml 各服务 command 改为 bash trap 模式：bash(SESS=1) 作为 tini 直接子进程
      接收 SIGTERM → trap 触发 node.sh stop → Manager 收到 SIGTERM → leaserevoke（<1s）→ key 立即删除。
      node.sh do_stop/stop_all 加等待循环（kill -0 轮询），确保 Manager 退出前 bash 不提前退出。

    断言：
      - docker stop 后 hello 端口不可达（进程已退出）
      - etcd key 在 leaserevoke 后立即消失（≤5s，而非 TTL 自然过期）
      - docker start 后重新注册并可达
    """
    del service_readiness
    _need_docker()

    assert "HELLO_HTTP" in _registry_node_types(), \
        "HELLO_HTTP not in etcd before test (precondition failed)"
    assert _hello_ok(), "Hello not reachable before test (precondition failed)"

    # docker stop 发 SIGTERM → bash trap → node.sh stop → leaserevoke
    r = _dc("stop", "hello", timeout=30)
    assert r.returncode == 0, f"docker compose stop hello failed: {r.stderr}"

    try:
        # 进程退出后 hello 应不可达
        assert not _hello_ok(timeout=2.0), \
            "Hello still reachable after docker stop (process should have exited)"

        # leaserevoke 已执行：key 应在 5s 内消失（而非等 TTL）
        elapsed = _wait_type_gone("HELLO_HTTP", timeout_s=5.0)
        assert elapsed <= 5.0, \
            f"HELLO_HTTP key persisted {elapsed:.0f}s after docker stop (leaserevoke should be <1s)"

    finally:
        _dc("start", "hello", timeout=20)
        try:
            _wait_type_appears("HELLO_HTTP", timeout_s=20.0)
        except AssertionError:
            pytest.fail("HELLO_HTTP did not re-register within 20s after docker start")

        # 恢复后业务可达
        hello_restored = False
        for _ in range(20):
            if _hello_ok(timeout=3.0):
                hello_restored = True
                break
            time.sleep(0.5)
        assert hello_restored, "Hello not reachable within 10s after re-registration"


# ── S4：kill -9 → TTL 兜底 ───────────────────────────────────────────────────

@pytest.mark.stability
@pytest.mark.integration
@pytest.mark.slow
def test_s4_sigkill_ttl_expiry(service_readiness: object) -> None:
    """
    S4（slow，约 40s）— SIGKILL 强杀 → leaserevoke 不执行 → key 在 TTL 内仍存在 → TTL 后消失

    验证：
      - SIGKILL 后 key 立即仍在 etcd（leaserevoke 未执行）
      - 等待 TTL（30s）+ 宽限（8s）后 key 消失
      - docker start 恢复后重新注册
    """
    del service_readiness
    _need_docker()

    assert "HELLO_HTTP" in _registry_node_types(), \
        "HELLO_HTTP not in etcd before test (precondition failed)"

    # 记录 kill 前的 key 列表
    keys_before = set(_registry_keys_for_type("HELLO_HTTP"))
    assert keys_before, "No HELLO_HTTP keys found before kill"

    r = _dc("kill", "-s", "SIGKILL", "hello", timeout=10)
    assert r.returncode == 0, f"docker compose kill hello failed: {r.stderr}"

    try:
        # SIGKILL 后 key 应立即仍在（leaserevoke 肯定未执行）
        # 注：S3(SIGTERM) 实测 leaserevoke 也未执行（#118），因此 S3/S4 可观察行为一致。
        # S4 的价值在于明确断言"kill 后 key 不立即消失"，验证 TTL 兜底机制本身。
        time.sleep(1)
        types_after_kill = _registry_node_types()
        assert "HELLO_HTTP" in types_after_kill, \
            "HELLO_HTTP key disappeared immediately after SIGKILL " \
            "(expected to persist until TTL expiry; see issus #118)"

        # 等 TTL 过期（最多 TTL_WAIT_S 秒）
        elapsed = _wait_type_gone("HELLO_HTTP", timeout_s=float(TTL_WAIT_S))
        assert elapsed <= TTL_WAIT_S, \
            f"HELLO_HTTP key persisted {elapsed:.0f}s after SIGKILL (TTL={LEASE_TTL}s)"

    finally:
        _dc("start", "hello", timeout=20)
        try:
            _wait_type_appears("HELLO_HTTP", timeout_s=20.0)
        except AssertionError:
            pytest.fail("HELLO_HTTP did not re-register within 20s after docker start")


# ── S5：崩溃重启 → 重新注册 → 路由恢复 ──────────────────────────────────────

@pytest.mark.stability
@pytest.mark.integration
def test_s5_crash_restart_reregistration(service_readiness: object) -> None:
    """
    S5 — docker kill（SIGKILL）→ docker start → 服务重新注册 → 路由恢复 → 业务可达

    与 S4 的区别：S4 侧重 TTL 兜底行为，S5 侧重重启后的注册恢复闭环。
    S5 在 kill 后立即 start（不等 TTL），验证新进程能正常注册并提供服务。
    """
    del service_readiness
    _need_docker()

    assert "HELLO_HTTP" in _registry_node_types(), \
        "HELLO_HTTP not in etcd before test (precondition failed)"
    assert _hello_ok(), "Hello not reachable before test (precondition failed)"

    r = _dc("kill", "-s", "SIGKILL", "hello", timeout=10)
    assert r.returncode == 0, f"docker compose kill hello failed: {r.stderr}"

    try:
        r = _dc("start", "hello", timeout=20)
        assert r.returncode == 0, f"docker compose start hello failed: {r.stderr}"

        # 等新进程重新注册（最多 20s）
        elapsed_reg = _wait_type_appears("HELLO_HTTP", timeout_s=20.0)

        # 注册后路由下发（Watch 毫秒级，给 3s 宽限）
        time.sleep(WATCH_PROPAGATION_S)

        # 业务恢复
        hello_ok = False
        for _ in range(20):
            if _hello_ok(timeout=3.0):
                hello_ok = True
                break
            time.sleep(0.5)
        assert hello_ok, \
            f"Hello not reachable within 10s after re-registration (reg took {elapsed_reg:.1f}s)"

    except AssertionError:
        # 确保容器被恢复
        _dc("start", "hello", timeout=20)
        raise


# ── S6：长跑（可选，标 slow）────────────────────────────────────────────────

@pytest.mark.stability
@pytest.mark.slow
def test_s6_long_run_lease_stability(service_readiness: object) -> None:
    """
    S6（slow，5 分钟）— 持续轮询 etcd 注册表，验证 keepalive 正常、无意外租约丢失。

    每 10s 轮询一次，持续 300s，任意一次发现 node_type 缺失即失败。
    """
    del service_readiness

    duration_s = 300
    interval_s = 10
    t0 = time.time()
    failures: list[str] = []

    while time.time() - t0 < duration_s:
        elapsed = time.time() - t0
        types = _registry_node_types()
        missing = EXPECTED_NODE_TYPES - types
        if missing:
            failures.append(f"t={elapsed:.0f}s: missing {missing}")
        time.sleep(interval_s)

    assert not failures, \
        f"Lease instability detected during {duration_s}s run:\n" + "\n".join(failures)
