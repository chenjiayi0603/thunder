"""ModuleLua 端到端测试"""
from __future__ import annotations
import os
import json
import requests
import pytest

BASE = os.getenv("LUA_TEST_HOST", "http://127.0.0.1:27006")

def _json(text):
    text = text.strip("\x00").strip()
    return json.loads(text) if text else {}

@pytest.mark.smoke
def test_lua_echo():
    r = requests.post(f"{BASE}/hello/lua_echo", data="test", timeout=10)
    assert r.status_code == 200
    assert _json(r.text).get("code") == 0

@pytest.mark.smoke
def test_lua_limit_short():
    r = requests.post(f"{BASE}/hello/lua_limit", data="hello", timeout=10)
    assert r.status_code == 200

@pytest.mark.smoke
def test_lua_limit_long():
    r = requests.post(f"{BASE}/hello/lua_limit", data="x" * 101, timeout=10)
    assert r.status_code == 200
    data = _json(r.text)
    assert data.get("code") == 1
    assert "body too long" in data.get("msg", "")

@pytest.mark.smoke
def test_lua_route():
    """完整链路: HEllo(Lua) -> LOGIC -> HEllo(Lua) -> 客户端"""
    r = requests.post(f"{BASE}/hello/lua_route", data='{"option":"Echo"}', timeout=15)
    assert r.status_code == 200
    data = _json(r.text)
    assert data.get("code") in (0, 1), f"unexpected: {r.text!r}"

@pytest.mark.smoke
def test_lua_scripts_exist():
    deploy = os.getenv("THUNDER_DEPLOY", "/home/tommychen/thunder/deploy/HelloHttp")
    for p in ["scripts/echo.lua", "scripts/route.lua", "scripts/limit.lua"]:
        assert os.path.exists(os.path.join(deploy, p)), f"missing {p}"
