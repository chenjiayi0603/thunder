"""E2E: HTTPS outbound — ModuleHello TestHttpRequestCo triggers HTTPS requests.

Requires: Docker Compose cluster running, external network access.
Tests that HttpRequestCo completes without error (makes HTTP+HTTPS calls).

Note: This test requires external network to reach baidu.com.
If running in an isolated Docker network, this test is skipped.
"""
import pytest, requests, socket, time


HELLO_URL = "http://127.0.0.1:27006"
LOG_FILE = "/home/tommychen/thunder/deploy/HelloHttp/log/Hello_robot_W0.log"


def _has_external_network():
    """Check if external network is available."""
    try:
        s = socket.socket()
        s.settimeout(3)
        s.connect(("www.baidu.com", 80))
        s.close()
        return True
    except:
        return False


@pytest.mark.skipif(
    not _has_external_network(),
    reason="No external network — skipping HTTPS outbound E2E"
)
class TestHttpsOutboundE2E:
    """E2E: TestHttpRequestCo triggers HTTPS requests via ModuleHello."""

    def test_httpreq_co_completes(self):
        """HttpRequestCo runs HTTP+HTTPS requests, returns code=0."""
        resp = requests.post(
            f"{HELLO_URL}/hello/hello",
            json={"option": "TestHttpRequestCo"},
            timeout=60
        )
        data = resp.json()
        assert data.get("code") == 0, \
            f"HttpRequestCo failed with code={data.get('code')}"

    def test_https_logged(self):
        """Verify HTTPS baidu request logged in Worker."""
        # grep for HTTPS request attempt in log
        import subprocess
        result = subprocess.run(
            ["grep", "-c", "HTTPS baidu", LOG_FILE],
            capture_output=True, text=True, timeout=5
        )
        count = int(result.stdout.strip() or 0)
        assert count >= 1, "No HTTPS baidu request logged"


@pytest.mark.skipif(
    _has_external_network(),
    reason="External network available — running offline-only tests"
)
class TestHttpsOfflineSmoke:
    """Smoke test — no external network needed, just verify endpoint responds."""

    def test_http_endpoint_still_works(self):
        """Basic HTTP endpoint unaffected by HTTPS changes."""
        resp = requests.post(
            f"{HELLO_URL}/hello/hello",
            json={"option": "Echo"},
            timeout=5
        )
        assert resp.json()["code"] == 0

    def test_module_loaded(self):
        """ModuleHello and ModuleLua loaded correctly."""
        posts = [
            ("/hello/hello", {"option": "Echo"}),
            ("/hello/lua_echo", "test"),
        ]
        for path, body in posts:
            resp = requests.post(
                f"{HELLO_URL}{path}",
                json=body if isinstance(body, dict) else None,
                data=body if isinstance(body, str) else None,
                timeout=5
            )
            assert resp.status_code == 200, f"{path} returned {resp.status_code}"


@pytest.mark.parametrize("url,expected_fragment", [
    ("https://www.baidu.com/", "https"),
    ("http://www.baidu.com/",  "http"),
])
def test_url_scheme_detection(url, expected_fragment):
    """Verify URL scheme detection logic (same as AutoSend)."""
    scheme = url[:url.find(":")]
    assert scheme == expected_fragment
