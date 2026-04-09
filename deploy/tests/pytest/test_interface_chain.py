from __future__ import annotations

import time
import requests
import pytest

BASE_URL = "http://127.0.0.1:27008/Interface/gentoken"


@pytest.mark.integration
def test_interface_http_co20_echo(http_session: requests.Session) -> None:
    # 测试目的：确认 Interface 基础 HTTP 链路可用（最小健康检查）。
    resp = http_session.post(BASE_URL, json={"option": "Echo"}, timeout=30)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data.get("code") == 0, data


@pytest.mark.integration
@pytest.mark.smoke
def test_interface_genkey_verifykey_chain(http_session: requests.Session) -> None:
    # 测试目的：覆盖 Interface -> Logic 的关键业务链路（GenKey -> VerifyKey）。
    token = None
    key = None
    last = None
    # Center/Logic/Interface 链路刚启动时路由下发可能存在短窗口，允许重试。
    for _ in range(20):
        gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=60)
        assert gen.status_code == 200, gen.text
        g = gen.json()
        last = g
        token = g.get("token")
        key = g.get("key")
        if token and key:
            break
        time.sleep(0.5)
    # 必须成功并拿到 token/key。
    assert isinstance(last, dict), last
    assert str(last.get("msg", "")) == "success", last
    assert token and key, last

    verify = http_session.post(BASE_URL, json={"option": "VerifyKey", "token": token, "key": key}, timeout=60)
    assert verify.status_code == 200, verify.text
    v = verify.json()
    assert "code" in v

