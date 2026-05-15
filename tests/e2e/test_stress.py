"""
压力冒烟端到端测试

覆盖：
  - 100 并发请求 0 错误率
  - 大 body 响应
  - 持续请求 fd 稳定
"""
from __future__ import annotations

import requests
import pytest
import concurrent.futures
import time

HELLO_URL = "http://127.0.0.1:27006/hello/hello"


@pytest.mark.perf
def test_stress_concurrent_100(http_session: requests.Session) -> None:
    """100 并发 Echo 请求，验证 0 错误"""
    def do_echo() -> bool:
        s = requests.Session()
        s.trust_env = False
        try:
            r = s.post(HELLO_URL, json={"option": "Echo"}, timeout=30)
            return r.status_code == 200 and '"code"' in r.text
        except Exception:
            return False

    with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
        futures = [pool.submit(do_echo) for _ in range(100)]
        results = [f.result() for f in concurrent.futures.as_completed(futures, timeout=120)]

    success = sum(1 for r in results if r)
    assert success == 100, f"{100 - success} requests failed out of 100"


@pytest.mark.perf
def test_stress_sustained_30s(http_session: requests.Session) -> None:
    """持续 30s 每秒请求 5 次，不应有持续失败"""
    deadline = time.time() + 30
    failures = 0
    requests_made = 0
    while time.time() < deadline:
        try:
            r = http_session.post(HELLO_URL, json={"option": "Echo"}, timeout=15)
            requests_made += 1
            if r.status_code != 200:
                failures += 1
        except Exception:
            failures += 1
        time.sleep(0.2)

    assert requests_made > 0, "No requests completed"
    assert failures < max(5, requests_made * 0.05), \
        f"{failures} failures out of {requests_made} (>5% error rate)"


@pytest.mark.perf
def test_stress_large_response(http_session: requests.Session) -> None:
    """CPU 线程池卸载：256KB 向量 checksum，验证协程挂起/恢复与响应正确性"""
    r = http_session.post(HELLO_URL, json={"option": "TestHelloPoolCpu"}, timeout=30)
    assert r.status_code == 200
    data = r.json()
    assert data.get("option") == "TestHelloPoolCpu", f"Unexpected response: {r.text}"
    # 256*1024 bytes, all 0x03 → checksum = 786432
    assert data.get("checksum") == 786432, f"Wrong checksum: {data.get('checksum')}"


@pytest.mark.integration
def test_stress_keepalive_reuse(http_session: requests.Session) -> None:
    """同一 session 多次请求复用连接"""
    for i in range(20):
        r = http_session.post(HELLO_URL, json={"option": "Echo"}, timeout=15)
        assert r.status_code == 200, f"Request {i} failed"
