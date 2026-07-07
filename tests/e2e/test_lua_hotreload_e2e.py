"""E2E: Lua hot-reload — admin API push → Manager Watch → Worker ReloadScript → response changes.

Requires:
  - Docker Compose cluster running (hello + etcd + logic + interface)
  - admin-web server on localhost:8090

Tests:
  1. Push new Lua script via admin API
  2. Wait for hot-reload to take effect
  3. Verify response contains new content
  4. Verify NO SO unloading (no log line with "succeed in unload")
  5. Verify sibling Lua modules unaffected (lua_limit, lua_route, lua_node_type)
"""
import pytest
import requests
import time
import json
import subprocess


HELLO_URL = "http://127.0.0.1:27006"
ADMIN_URL = "http://127.0.0.1:8090"
LOG_FILE = "/home/tommychen/thunder/deploy/HelloHttp/log/Hello_robot_W0.log"

# Each test generates its own unique marker to avoid cross-test version conflicts.


def push_lua_script(content: str) -> dict:
    """Push Lua script via admin API. Returns response JSON."""
    # Ensure module configs are synced first
    requests.post(f"{ADMIN_URL}/api/sync-config", timeout=5)
    resp = requests.post(f"{ADMIN_URL}/api/lua-scripts", json={
        "node_type": "HELLO_HTTP",
        "name": "echo.lua",
        "url_path": "/hello/lua_echo",
        "content": content,
    }, timeout=5)
    return resp.json()


def check_response(expected_msg: str, timeout: float = 15.0) -> bool:
    """Poll lua_echo endpoint until response contains expected_msg or timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            resp = requests.post(f"{HELLO_URL}/hello/lua_echo", data="test", timeout=3)
            data = resp.json()
            if data.get("msg") == expected_msg:
                return True
        except Exception:
            pass
        time.sleep(1)
    return False


def check_sibling(url_path: str, timeout: float = 10.0) -> bool:
    """Check a sibling Lua module still responds correctly."""
    try:
        resp = requests.post(f"{HELLO_URL}{url_path}", data="test", timeout=timeout)
        return resp.status_code == 200
    except Exception:
        return False


def grep_log(pattern: str, count_only: bool = False) -> str:
    """Grep Worker log for pattern."""
    try:
        result = subprocess.run(
            ["grep", "-c" if count_only else "-a", pattern, LOG_FILE],
            capture_output=True, text=True, timeout=5
        )
        return result.stdout.strip()
    except Exception:
        return ""


class TestLuaHotReloadE2E:
    """End-to-end Lua hot-reload tests."""

    @pytest.mark.integration
    def test_hotreload_changes_response(self):
        """Push new Lua script → response changes to new content."""
        marker = f"E2E_RESP_{int(time.time())}"
        content = f'''function handle_request(msg)
  SendToClientFast('{{"code":0,"msg":"{marker}"}}')
  return true
end'''
        result = push_lua_script(content)
        assert result["ok"], f"Push failed: {result}"

        # Wait for hot-reload to take effect
        assert check_response(marker, timeout=30), \
            f"Response did not change to {marker} within 30s"

    @pytest.mark.integration
    def test_hotreload_no_so_unload(self):
        """Verify hot-reload does NOT trigger SO dlclose (no 'succeed in unload' log)."""
        # Count "succeed in unload" lines before
        before = int(grep_log("succeed in unload.*ModuleLua", count_only=True) or 0)

        # Push new script
        marker2 = f"HOTRELOAD_NOSO_{int(time.time())}"
        content = f'''function handle_request(msg)
  SendToClientFast('{{"code":0,"msg":"{marker2}"}}')
  return true
end'''
        result = push_lua_script(content)
        assert result["ok"], f"Push failed: {result}"

        # Wait for response change
        assert check_response(marker2, timeout=30), \
            f"Response did not change to {marker2}"

        # After reload, "succeed in unload" count should NOT increase
        after = int(grep_log("succeed in unload.*ModuleLua", count_only=True) or 0)
        assert after == before, \
            f"SO was unloaded! unload count: {before} → {after} (should be unchanged)"

    @pytest.mark.integration
    def test_hotreload_siblings_unaffected(self):
        """Verify sibling Lua modules still work after hot-reloading one."""
        # Push just lua_echo, not touching limit/route/node_type
        marker3 = f"HOTRELOAD_SIB_{int(time.time())}"
        content = f'''function handle_request(msg)
  SendToClientFast('{{"code":0,"msg":"{marker3}"}}')
  return true
end'''
        result = push_lua_script(content)
        assert result["ok"]

        assert check_response(marker3, timeout=30), "lua_echo did not reload"

        # Siblings should still respond
        assert check_sibling("/hello/lua_limit"), "lua_limit broken after hot-reload"
        assert check_sibling("/hello/lua_route"), "lua_route broken after hot-reload"
        assert check_sibling("/hello/lua_node_type"), "lua_node_type broken after hot-reload"

    @pytest.mark.integration
    def test_hotreload_reload_lua_log(self):
        """Verify Manager generates 'reload lua scripts in-place' log (not 'graceful restart')."""
        # Push a new script
        marker4 = f"HOTRELOAD_LOG_{int(time.time())}"
        content = f'''function handle_request(msg)
  SendToClientFast('{{"code":0,"msg":"{marker4}"}}')
  return true
end'''
        push_lua_script(content)
        assert check_response(marker4, timeout=30)

        # Check Manager log
        manager_log = "/home/tommychen/thunder/deploy/HelloHttp/log/Hello_robot.log"
        logs = subprocess.run(
            ["grep", "-a", "reload.*lua scripts in-place", manager_log],
            capture_output=True, text=True, timeout=5
        ).stdout.strip()
        assert "VM only" in logs or "in-place" in logs, \
            f"Manager log missing 'reload lua scripts in-place': got '{logs[:200]}'"
        assert "graceful restart" not in logs, \
            "Manager should NOT trigger graceful restart for Lua reload"
