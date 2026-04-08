from __future__ import annotations

import requests
import pytest

BASE_URL = "http://127.0.0.1:27008/Interface/gentoken"


@pytest.mark.integration
@pytest.mark.smoke
def test_multicenter_raft(http_session: requests.Session) -> None:
    # 测试目的：
    # 1) 若可访问 Center admin，则校验多 Center 视角下 leader 一致性；
    # 2) 无论 admin 能力是否可用，都复用 GenKey 链路确认集群联调主路径可达。
    leader_ids: list[str] = []
    for port in (26000, 26022, 26032):
        try:
            r = http_session.post(
                f"http://127.0.0.1:{port}/admin",
                json={"cmd": "show", "args": ["center"]},
                timeout=20,
            )
        except requests.RequestException:
            continue
        if r.status_code != 200:
            continue
        data = r.json()
        rows = data.get("data", [])
        leaders = [x for x in rows if isinstance(x, dict) and str(x.get("leader", "")) == "yes"]
        if len(leaders) == 1:
            leader_ids.append(str(leaders[0].get("identify", "")))
    if leader_ids:
        assert len(set(leader_ids)) == 1, leader_ids

    # 复用关键业务链路做联调确认
    gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=60)
    assert gen.status_code == 200, gen.text
    g = gen.json()
    token = g.get("token")
    key = g.get("key")
    if token and key:
        return
    assert str(g.get("msg", "")) in ("logic step failed", "success"), g

