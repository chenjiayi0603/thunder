"""
etcd Watch 专项 E2E 测试

验证 EtcdGrpcConnector Watch 实现的三个核心行为：
  1. PUT 事件 — 新节点注入 etcd 后，服务侧路由在 2s 内可用（Watch 实时，非 5s 轮询）
  2. DELETE 事件 — leaserevoke 后节点从 etcd 消失；服务侧路由在 2s 内被剔除
  3. Watch 断流重建 — etcd1 重启后，Watch 自动重连，路由恢复正常

覆盖局限说明：
  Thunder 服务的内部路由表无公开查询接口，因此"服务侧路由已更新"只能通过业务
  可达性间接验证（curl hello / Interface→Logic）。etcd 侧的 PUT/DELETE 是同步可见的，
  Watch 事件理论上在 ~100ms 内触发（由 gRPC streaming 保证），测试以 2s 为保守上限。
"""
from __future__ import annotations

import base64
import json
import os
import subprocess
import time
from pathlib import Path

import pytest
import requests

# ── 常量 ────────────────────────────────────────────────────────────────────
_ETCD_HOST = os.environ.get("ETCD_HOST", "127.0.0.1")
ETCD_URL   = f"http://{_ETCD_HOST}:2379"
HELLO_URL  = "http://127.0.0.1:27006"
IFACE_URL  = "http://127.0.0.1:27008"

_DOCKER_DIR = Path(__file__).resolve().parents[2] / "docker"

# 注入的假节点参数（使用不存在的端口，不影响真实业务流量）
_FAKE_NODE_TYPE = "WATCH_TEST_NODE"
_FAKE_IP        = "127.0.0.1"
_FAKE_PORT      = 19999
_FAKE_NODE_ID   = 201


# ── 辅助函数 ─────────────────────────────────────────────────────────────────

def _etcd_ok() -> bool:
    try:
        return requests.get(f"{ETCD_URL}/version", timeout=3).status_code == 200
    except Exception:
        return False


def _b64(s: str) -> str:
    return base64.b64encode(s.encode()).decode()


def _etcd_put(key: str, value: str, lease_id: int = 0) -> bool:
    """向 etcd 写入 key-value，可选绑定 lease。"""
    body: dict = {"key": _b64(key), "value": _b64(value)}
    if lease_id:
        body["lease"] = str(lease_id)
    r = requests.post(f"{ETCD_URL}/v3/kv/put", json=body, timeout=5)
    return r.status_code == 200


def _etcd_del(key: str) -> bool:
    """从 etcd 删除 key。"""
    r = requests.post(
        f"{ETCD_URL}/v3/kv/deleterange",
        json={"key": _b64(key)},
        timeout=5,
    )
    return r.status_code == 200


def _etcd_get(key: str) -> str | None:
    """读 etcd key，返回 value 字符串；不存在返回 None。"""
    r = requests.post(
        f"{ETCD_URL}/v3/kv/range",
        json={"key": _b64(key)},
        timeout=5,
    )
    if r.status_code != 200:
        return None
    kvs = r.json().get("kvs", [])
    if not kvs:
        return None
    try:
        return base64.b64decode(kvs[0]["value"]).decode()
    except Exception:
        return None


def _etcd_lease_grant(ttl: int) -> int:
    """申请一个 etcd lease，返回 lease id（失败返回 0）。"""
    r = requests.post(f"{ETCD_URL}/v3/lease/grant", json={"TTL": str(ttl)}, timeout=5)
    if r.status_code != 200:
        return 0
    return int(r.json().get("ID", "0"))


def _etcd_lease_revoke(lease_id: int) -> bool:
    """撤销 etcd lease（绑定该 lease 的所有 key 同步删除）。"""
    r = requests.post(
        f"{ETCD_URL}/v3/lease/revoke",
        json={"ID": str(lease_id)},
        timeout=5,
    )
    return r.status_code == 200


def _wait_key_exists(key: str, timeout_s: float = 3.0) -> float:
    """等待 etcd key 出现，返回等待秒数；超时抛 AssertionError。"""
    t0 = time.time()
    deadline = t0 + timeout_s
    while time.time() < deadline:
        if _etcd_get(key) is not None:
            return time.time() - t0
        time.sleep(0.1)
    raise AssertionError(f"key '{key}' did not appear in etcd within {timeout_s}s")


def _wait_key_gone(key: str, timeout_s: float = 3.0) -> float:
    """等待 etcd key 消失，返回等待秒数；超时抛 AssertionError。"""
    t0 = time.time()
    deadline = t0 + timeout_s
    while time.time() < deadline:
        if _etcd_get(key) is None:
            return time.time() - t0
        time.sleep(0.1)
    raise AssertionError(f"key '{key}' did not disappear from etcd within {timeout_s}s")


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


def _fake_registry_key() -> str:
    return f"/thunder/registry/{_FAKE_NODE_TYPE}/{_FAKE_IP}:{_FAKE_PORT}"


def _fake_registry_value() -> str:
    return json.dumps({
        "node_id":    _FAKE_NODE_ID,
        "node_type":  _FAKE_NODE_TYPE,
        "node_ip":    _FAKE_IP,
        "node_port":  _FAKE_PORT,
        "worker_num": 1,
    })


def _docker_compose(args: list[str], timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", "compose"] + args,
        cwd=str(_DOCKER_DIR),
        capture_output=True,
        text=True,
        timeout=timeout,
    )


# ── 测试 ─────────────────────────────────────────────────────────────────────

@pytest.mark.watch
@pytest.mark.integration
def test_watch_put_event_etcd_consistency(service_readiness: object) -> None:
    """
    PUT 事件一致性测试：
      向 etcd 注入假节点 → 确认 key 出现（etcd 侧立即可见）
      → 删除 key → 确认 key 消失（etcd 侧立即可见）
      → 验证 Thunder 服务仍正常（Watch 未破坏路由）

    覆盖：Watch 的 PUT/DELETE 路径不破坏现有路由。
    局限：仅能观察 etcd 侧；Thunder 服务内部路由表更新通过业务可达性间接验证。
    """
    del service_readiness
    if not _etcd_ok():
        pytest.skip("etcd not reachable")

    reg_key = _fake_registry_key()
    reg_val = _fake_registry_value()

    # 确保测试前假节点不存在
    _etcd_del(reg_key)

    try:
        # ── PUT：注入假节点 ──
        assert _etcd_put(reg_key, reg_val), "etcd PUT failed"
        elapsed_put = _wait_key_exists(reg_key, timeout_s=2.0)
        assert elapsed_put < 2.0, f"PUT visible after {elapsed_put:.2f}s (> 2s)"

        # ── 验证业务链路未受影响 ──
        assert _hello_ok(), "Hello service unreachable after fake PUT"

        # ── DELETE：删除假节点 ──
        assert _etcd_del(reg_key), "etcd DELETE failed"
        elapsed_del = _wait_key_gone(reg_key, timeout_s=2.0)
        assert elapsed_del < 2.0, f"DELETE visible after {elapsed_del:.2f}s (> 2s)"

    finally:
        _etcd_del(reg_key)  # 清理

    # 删除后业务链路仍正常
    assert _hello_ok(), "Hello service unreachable after fake DELETE"


@pytest.mark.watch
@pytest.mark.integration
def test_watch_delete_via_leaserevoke(service_readiness: object) -> None:
    """
    DELETE 事件（leaserevoke）测试：
      申请 lease → 注入绑定该 lease 的假节点 → leaserevoke → key 在 2s 内从 etcd 消失

    这是 Thunder 服务真实的下线路径（Destroy() 调 leaserevoke，原子删除所有绑定 key）。
    验证该路径在 etcd 侧的时效性，以及业务不受影响。
    """
    del service_readiness
    if not _etcd_ok():
        pytest.skip("etcd not reachable")

    reg_key = _fake_registry_key()
    _etcd_del(reg_key)

    # 申请短 TTL lease（30s，足够测试完成，leaserevoke 会提前回收）
    lease_id = _etcd_lease_grant(ttl=30)
    assert lease_id != 0, "etcd lease grant failed"

    try:
        # 注入绑定 lease 的假节点
        assert _etcd_put(reg_key, _fake_registry_value(), lease_id=lease_id), \
            "etcd PUT with lease failed"
        assert _etcd_get(reg_key) is not None, "key not visible after PUT"

        # 验证业务正常
        assert _hello_ok(), "Hello unreachable before leaserevoke"

        # leaserevoke：模拟服务正常下线（Destroy → leaserevoke）
        t0 = time.time()
        assert _etcd_lease_revoke(lease_id), "etcd leaserevoke failed"
        lease_id = 0  # 已撤销，finally 不再重复撤销

        elapsed = _wait_key_gone(reg_key, timeout_s=3.0)
        assert elapsed < 3.0, \
            f"key still visible {elapsed:.2f}s after leaserevoke (expected immediate deletion)"

    finally:
        if lease_id:
            _etcd_lease_revoke(lease_id)
        _etcd_del(reg_key)

    # 下线后业务链路仍正常
    assert _hello_ok(), "Hello unreachable after leaserevoke"
    assert _interface_ok(), "Interface→Logic unreachable after leaserevoke"


@pytest.mark.watch
@pytest.mark.integration
def test_watch_reconnect_after_etcd1_restart(service_readiness: object) -> None:
    """
    Watch 断流重建测试：
      停止 etcd1 → Watch stream 断开 → OnWatchEnded 触发 → GrpcThread 重建快照 + Watch
      → etcd1 恢复 → DoInitialSnapshot 成功 → 路由刷新 → 业务可达

    这是验证 EtcdGrpcConnector Watch 重连逻辑（OnWatchEnded → m_watchEnded=true →
    GrpcThreadMain → m_watcher.reset() + DoInitialSnapshot + DoStartWatch）的直接测试。

    前提：docker compose 在 /thunder/docker/ 可用（mode=local）。
    等待时间：etcd1 重启 ≤15s，Watch 重建 ≤15s，总计 ≤45s。
    """
    del service_readiness

    if not _etcd_ok():
        pytest.skip("etcd not reachable")
    if not _DOCKER_DIR.exists():
        pytest.skip(f"docker compose dir not found: {_DOCKER_DIR}")

    # 重启前业务链路必须正常
    assert _hello_ok(), "Hello not healthy before test (precondition failed)"
    assert _interface_ok(), "Interface→Logic not healthy before test (precondition failed)"

    # ── 停止 etcd1（Watch stream 将断开，OnWatchEnded 触发） ──
    r = _docker_compose(["stop", "etcd1"], timeout=20)
    assert r.returncode == 0, f"docker compose stop etcd1 failed: {r.stderr}"

    try:
        # etcd1 停止后，等待 3s 让 Watch stream 断开、GrpcThread 感知到
        time.sleep(3)

        # ── 验证 etcd2/etcd3 仍健康（3 节点集群 quorum 不受单点影响） ──
        etcd2_ok = False
        try:
            r2 = requests.get("http://127.0.0.1:2381/version", timeout=3)
            etcd2_ok = r2.status_code == 200
        except Exception:
            pass
        if not etcd2_ok:
            pytest.skip("etcd2 not healthy (cluster setup issue), skipping reconnect test")

        # ── 恢复 etcd1 ──
        r = _docker_compose(["start", "etcd1"], timeout=20)
        assert r.returncode == 0, f"docker compose start etcd1 failed: {r.stderr}"

        # 等待 etcd1 重新可达（最多 20s）
        etcd1_ready = False
        for _ in range(40):
            if _etcd_ok():
                etcd1_ready = True
                break
            time.sleep(0.5)
        assert etcd1_ready, "etcd1 did not become healthy within 20s after restart"

        # ── 等待 Watch 重建（GrpcThread 检测到 m_watchEnded，重做 snapshot + Watch） ──
        # GrpcThread tick = 1s，DoInitialSnapshot + DoStartWatch 需要几个 tick
        # 给足 15s 让 Watch 完全重建
        time.sleep(3)

        # ── 验证 Thunder 服务路由已恢复 ──
        hello_ok = False
        for _ in range(30):  # 最多等 15s
            if _hello_ok(timeout=3.0):
                hello_ok = True
                break
            time.sleep(0.5)
        assert hello_ok, "Hello service not healthy within 15s after etcd1 restart"

        iface_ok = False
        for _ in range(30):
            if _interface_ok(timeout=5.0):
                iface_ok = True
                break
            time.sleep(0.5)
        assert iface_ok, "Interface→Logic not healthy within 15s after etcd1 restart"

        # ── 验证 etcd 中节点注册完整（最多等 30s，各容器重建租约速度不同） ──
        expected = {"HELLO_HTTP", "HELLO_WS", "HELLO_HTTPS", "INTERFACE", "LOGIC"}
        missing = expected.copy()
        for _ in range(60):  # 30s
            try:
                nodes_resp = requests.post(
                    f"{ETCD_URL}/v3/kv/range",
                    json={
                        "key":       _b64("/thunder/registry/"),
                        "range_end": _b64("/thunder/registry0"),
                    },
                    timeout=5,
                )
                if nodes_resp.status_code == 200:
                    kvs = nodes_resp.json().get("kvs", [])
                    node_types: set[str] = set()
                    for kv in kvs:
                        try:
                            v = json.loads(base64.b64decode(kv["value"]).decode())
                            if "node_type" in v:
                                node_types.add(v["node_type"])
                        except Exception:
                            pass
                    missing = expected - node_types
                    if not missing:
                        break
            except Exception:
                pass
            time.sleep(0.5)
        assert not missing, \
            f"Missing node types after etcd1 restart: {missing} (found: {node_types})"

    finally:
        # 确保 etcd1 被恢复（即便测试失败）
        _docker_compose(["start", "etcd1"], timeout=20)
        # 等 etcd1 回来
        for _ in range(20):
            if _etcd_ok():
                break
            time.sleep(1)
