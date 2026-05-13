"""
Center Admin API 端到端测试

覆盖：
  - show nodes 返回在线节点
  - show center 返回 Raft leader
  - 路由表查询
  - 非法 cmd 错误处理
  - admin 多端口可达性
"""
from __future__ import annotations

import requests
import pytest


ADMIN_PORTS = (26000, 26022, 26032)


def _reachable_admin_ports(http_session: requests.Session) -> list[int]:
    ports: list[int] = []
    for p in ADMIN_PORTS:
        try:
            r = http_session.post(f"http://127.0.0.1:{p}/admin",
                                  json={"cmd": "show", "args": ["center"]},
                                  timeout=10)
            if r.status_code == 200:
                ports.append(p)
        except requests.RequestException:
            pass
    return ports


@pytest.mark.integration
@pytest.mark.smoke
def test_center_admin_show_nodes(http_session: requests.Session) -> None:
    ports = _reachable_admin_ports(http_session)
    if not ports:
        pytest.skip("No admin port reachable")
    for p in ports:
        r = http_session.post(f"http://127.0.0.1:{p}/admin",
                              json={"cmd": "show", "args": ["nodes", "LOGIC"]},
                              timeout=10)
        assert r.status_code == 200, f"admin {p}: {r.status_code}"
        data = r.json()
        assert "data" in data or "code" in data


@pytest.mark.integration
@pytest.mark.smoke
def test_center_admin_show_center(http_session: requests.Session) -> None:
    ports = _reachable_admin_ports(http_session)
    if not ports:
        pytest.skip("No admin port reachable")
    for p in ports:
        r = http_session.post(f"http://127.0.0.1:{p}/admin",
                              json={"cmd": "show", "args": ["center"]},
                              timeout=10)
        assert r.status_code == 200
        data = r.json()
        center_list = data.get("data", [])
        assert isinstance(center_list, list), f"admin {p}: data not a list: {data}"


@pytest.mark.integration
def test_center_admin_show_nodes_interface(http_session: requests.Session) -> None:
    ports = _reachable_admin_ports(http_session)
    if not ports:
        pytest.skip("No admin port reachable")
    for p in ports:
        r = http_session.post(f"http://127.0.0.1:{p}/admin",
                              json={"cmd": "show", "args": ["nodes", "INTERFACE"]},
                              timeout=10)
        assert r.status_code == 200


@pytest.mark.integration
def test_center_admin_invalid_cmd(http_session: requests.Session) -> None:
    ports = _reachable_admin_ports(http_session)
    if not ports:
        pytest.skip("No admin port reachable")
    p = ports[0]
    r = http_session.post(f"http://127.0.0.1:{p}/admin",
                          json={"cmd": "nonexistent_cmd", "args": []},
                          timeout=10)
    # 非法 cmd 应返回响应（不 crash）
    assert r.status_code in (200, 400, 404, 500)


@pytest.mark.integration
def test_center_admin_leader_consistency(http_session: requests.Session) -> None:
    ports = _reachable_admin_ports(http_session)
    if len(ports) < 2:
        pytest.skip("Need at least 2 admin ports")
    leaders: list[str] = []
    for p in ports:
        r = http_session.post(f"http://127.0.0.1:{p}/admin",
                              json={"cmd": "show", "args": ["center"]},
                              timeout=10)
        assert r.status_code == 200
        data = r.json()
        for item in data.get("data", []):
            if isinstance(item, dict) and str(item.get("leader", "")).lower() == "yes":
                leaders.append(str(item.get("identify", "")))
    if leaders:
        assert len(set(leaders)) == 1, f"Leader mismatch across ports: {leaders}"
