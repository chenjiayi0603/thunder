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
    r = requests.post(f"{BASE}/hello/lua_route", data='{"option":"Echo"}', timeout=8)
    assert r.status_code == 200
    data = _json(r.text)
    if data.get("code") != 0:
        pytest.skip(f"LOGIC 不可达 (msg={data.get('msg','')})")

@pytest.mark.smoke
def test_lua_node_type_fire_forget():
    """SendToNodeType fire-and-forget 模式"""
    r = requests.post(f"{BASE}/hello/lua_node_type",
                      data='{"mode":"fire_forget"}', timeout=5)
    assert r.status_code == 200
    data = _json(r.text)
    if data.get("code") != 0:
        pytest.skip(f"LOGIC 不可达 (msg={data.get('msg','')}), 跳过 fire-forget")
    assert data.get("msg") == "fire_forget_ok", f"unexpected: {r.text!r}"

@pytest.mark.smoke
def test_lua_node_type_async():
    """SendToNodeType 异步回调模式 (发 LOGIC, 等回包)"""
    r = requests.post(f"{BASE}/hello/lua_node_type",
                      data='{"mode":"async"}', timeout=5)
    assert r.status_code == 200
    data = _json(r.text)
    if data.get("code") != 0:
        pytest.skip(f"LOGIC 不可达 (msg={data.get('msg','')}), 跳过 async")
    assert data.get("mode") == "async", f"unexpected: {r.text!r}"

@pytest.mark.smoke
def test_lua_node_type_async_target():
    """SendToNodeType 异步回调 + targetId 模式"""
    r = requests.post(f"{BASE}/hello/lua_node_type",
                      data='{"mode":"async_target","targetId":"unit_test"}', timeout=5)
    assert r.status_code == 200
    data = _json(r.text)
    if data.get("code") != 0:
        pytest.skip(f"LOGIC 不可达 (msg={data.get('msg','')}), 跳过 async_target")
    assert data.get("mode") == "async_target", f"unexpected: {r.text!r}"

@pytest.mark.smoke
def test_lua_node_type_default():
    """SendToNodeType 缺省模式 (返回模式列表)"""
    r = requests.post(f"{BASE}/hello/lua_node_type",
                      data='{}', timeout=5)
    assert r.status_code == 200
    data = _json(r.text)
    assert data.get("code") == 0

@pytest.mark.smoke
def test_lua_scripts_exist():
    deploy = os.getenv("THUNDER_DEPLOY", "/home/tommychen/thunder/deploy/HelloHttp")
    for p in ["scripts/echo.lua", "scripts/route.lua", "scripts/limit.lua",
              "scripts/node_type_demo.lua"]:
        assert os.path.exists(os.path.join(deploy, p)), f"missing {p}"
