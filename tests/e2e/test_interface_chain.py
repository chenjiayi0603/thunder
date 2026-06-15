"""
Interface -> Logic 链路端到端测试

覆盖：
  - HTTP Echo 健康检查
  - GenKey / VerifyKey 关键业务链路
  - 并发 GenKey 无重复 token
  - VerifyKey 错误 token 拒绝
"""
from __future__ import annotations

import os
import time
import requests
import pytest

_HOST = os.getenv("E2E_HOST", "127.0.0.1")
BASE_URL = f"http://{_HOST}:27008/Interface/gentoken"


@pytest.mark.integration
def test_interface_http_co20_echo(http_session: requests.Session) -> None:
    resp = http_session.post(BASE_URL, json={"option": "Echo"}, timeout=3)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data.get("code") == 0, data


@pytest.mark.integration
@pytest.mark.smoke
def test_interface_genkey_verifykey_chain(http_session: requests.Session) -> None:
    token = None
    key = None
    last = {}
    # Interface→Logic S2S：3s 超时，不通直接失败
    warmup = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=3)
    assert warmup.status_code == 200, warmup.text
        w = warmup.json()
        last = w
        t = w.get("token")
        k = w.get("key")
        if t and k:
            token, key = t, k
            break
        # "logic step failed" = 路由尚未就绪，继续等待
        time.sleep(1.0)

    assert token and key, f"Route to LOGIC never established after 30s: {last}"
    assert str(last.get("msg", "")) == "success", last

    verify = http_session.post(
        BASE_URL, json={"option": "VerifyKey", "token": token, "key": key}, timeout=3
    )
    assert verify.status_code == 200, verify.text
    v = verify.json()
    assert "code" in v


@pytest.mark.integration
@pytest.mark.route
def test_interface_genkey_concurrent_no_duplicate(http_session: requests.Session) -> None:
    """5 次并发 GenKey，token 不应重复"""
    tokens: set[str] = set()
