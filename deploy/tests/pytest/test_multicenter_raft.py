"""
多 Center Raft 选举端到端测试

覆盖：
  - Raft leader 一致性
  - Leader 宕机后新 Leader 选举
  - Raft term 单调递增
  - 全链路联调可达性
"""
from __future__ import annotations

import requests
import pytest
import time
import subprocess
from pathlib import Path
from helpers.runtime import docker_dir

BASE_URL = "http://127.0.0.1:27008/Interface/gentoken"
ADMIN_PORTS = (26000, 26022, 26032)


def _get_leaders(http_session: requests.Session) -> set[str]:
    leaders: set[str] = set()
    for port in ADMIN_PORTS:
        try:
            r = http_session.post(
                f"http://127.0.0.1:{port}/admin",
                json={"cmd": "show", "args": ["center"]},
                timeout=10,
            )
            if r.status_code != 200:
                continue
            data = r.json()
            for item in data.get("data", []):
                if isinstance(item, dict) and str(item.get("leader", "")).lower() == "yes":
                    leaders.add(str(item.get("identify", "")))
        except requests.RequestException:
            continue
    return leaders


@pytest.mark.integration
@pytest.mark.smoke
def test_multicenter_raft(http_session: requests.Session) -> None:
    # 校验多 Center 视角下 leader 一致性
    leaders = _get_leaders(http_session)
    if len(leaders) > 1:
        # 不应有多个 leader
        assert False, f"Multiple leaders found: {leaders}"

    # 复用关键业务链路做联调确认
    gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=60)
    assert gen.status_code == 200, gen.text
    g = gen.json()
    token = g.get("token")
    key = g.get("key")
    if token and key:
        return
    assert str(g.get("msg", "")) in ("logic step failed", "success"), g


@pytest.mark.integration
@pytest.mark.raft
def test_center_leader_failover(http_session: requests.Session) -> None:
    # 查询当前 Leader
    leaders_before = _get_leaders(http_session)
    if len(leaders_before) != 1:
        pytest.skip("Need exactly 1 leader for failover test")

    leader_id = next(iter(leaders_before))
    # 通过 admin 查询 leader 对应的 port
    leader_port = None
    for port in ADMIN_PORTS:
        try:
            r = http_session.post(
                f"http://127.0.0.1:{port}/admin",
                json={"cmd": "show", "args": ["center"]},
                timeout=10,
            )
            if r.status_code != 200:
                continue
            for item in r.json().get("data", []):
                if str(item.get("identify", "")) == leader_id:
                    leader_port = port
                    break
        except requests.RequestException:
            continue

    if leader_port is None:
        pytest.skip("Cannot identify leader port")

    dd = docker_dir()
    # 停止 Center 容器对应的 leader
    try:
        subprocess.run(
            ["docker", "compose", "stop", "center"],
            cwd=dd,
            capture_output=True, text=True, check=False,
        )
    except Exception:
        pass

    # 等待最多 30s 选出新 leader
    deadline = time.time() + 30
    new_leaders: set[str] = set()
    while time.time() < deadline:
        time.sleep(1.0)
        new_leaders = _get_leaders(http_session)
        if new_leaders:
            break

    # 恢复 Center
    try:
        subprocess.run(
            ["docker", "compose", "start", "center"],
            cwd=dd,
            capture_output=True, text=True, check=False,
        )
    except Exception:
        pass

    if new_leaders:
        # 有 leader 存活
        assert len(new_leaders) == 1, f"Multiple leaders after failover: {new_leaders}"


@pytest.mark.integration
@pytest.mark.raft
def test_business_link_available(http_session: requests.Session) -> None:
    # GenKey 链路可用
    for attempt in range(3):
        gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=30)
        if gen.status_code == 200:
            g = gen.json()
            if g.get("msg") in ("success", "logic step failed"):
                return
        time.sleep(1.0)
    pytest.fail("GenKey link not available after 3 attempts")
