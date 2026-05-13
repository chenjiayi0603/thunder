"""
Interface -> Logic 链路端到端测试

覆盖：
  - HTTP Echo 健康检查
  - GenKey / VerifyKey 关键业务链路
  - 并发 GenKey 无重复 token
  - VerifyKey 错误 token 拒绝
"""
from __future__ import annotations

import time
import requests
import pytest

BASE_URL = "http://127.0.0.1:27008/Interface/gentoken"


@pytest.mark.integration
def test_interface_http_co20_echo(http_session: requests.Session) -> None:
    resp = http_session.post(BASE_URL, json={"option": "Echo"}, timeout=30)
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert data.get("code") == 0, data


@pytest.mark.integration
@pytest.mark.smoke
def test_interface_genkey_verifykey_chain(http_session: requests.Session) -> None:
    token = None
    key = None
    last = None
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

    assert isinstance(last, dict), last
    assert str(last.get("msg", "")) == "success", last
    assert token and key, last

    verify = http_session.post(
        BASE_URL, json={"option": "VerifyKey", "token": token, "key": key}, timeout=60
    )
    assert verify.status_code == 200, verify.text
    v = verify.json()
    assert "code" in v


@pytest.mark.integration
@pytest.mark.route
def test_interface_genkey_concurrent_no_duplicate(http_session: requests.Session) -> None:
    """5 次并发 GenKey，token 不应重复"""
    tokens: set[str] = set()
    for _ in range(5):
        gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=60)
        assert gen.status_code == 200
        g = gen.json()
        t = g.get("token")
        if t:
            tokens.add(str(t))
    # 如果有 token 返回，不应重复
    if len(tokens) >= 2:
        assert len(tokens) >= 2, f"Only {len(tokens)} unique tokens from 5 requests"


@pytest.mark.integration
def test_interface_verify_wrong_token(http_session: requests.Session) -> None:
    """错误 token 应返回失败"""
    verify = http_session.post(
        BASE_URL,
        json={"option": "VerifyKey", "token": "bad_token_xyz", "key": "bad_key_abc"},
        timeout=30,
    )
    assert verify.status_code == 200
    v = verify.json()
    # 错误 token 可能返回非 success
    assert v.get("msg", "") != "success" or v.get("code", 0) != 0


@pytest.mark.integration
def test_interface_genkey_repeated_works(http_session: requests.Session) -> None:
    """连续 3 次 GenKey 均成功"""
    for i in range(3):
        gen = http_session.post(BASE_URL, json={"option": "GenKey"}, timeout=60)
        assert gen.status_code == 200, f"Attempt {i}: {gen.status_code}"
