"""admin-web Lua API 端到端测试 (K8s 环境, NodePort 30090)

覆盖: 上传→制品库→下发→已部署→验证内容

用法:
  E2E_ADMIN_HOST=192.168.3.61 pytest tests/e2e/test_admin_web_lua.py -v
"""

import json, os, time, pytest, requests

ADMIN_HOST = os.getenv("E2E_ADMIN_HOST", "192.168.3.61")
ADMIN_URL = f"http://{ADMIN_HOST}:30090"


def _get(path, timeout=10):
    r = requests.get(f"{ADMIN_URL}{path}", timeout=timeout)
    return r.json()


def _post(path, body=None, timeout=10):
    r = requests.post(f"{ADMIN_URL}{path}", json=body or {}, timeout=timeout)
    return r.json()


def _put(path, data, timeout=10):
    r = requests.put(f"{ADMIN_URL}{path}", data=data,
                     headers={"Content-Type": "application/octet-stream"}, timeout=10)
    return r.json()


# ── 测试常量 ──
TEST_LUA_NAME = "_e2e_lua_test.lua"
TEST_NODE_TYPE = "HELLO_HTTP"
TEST_URL_PATH = "/hello/_e2e_lua_test"
TEST_LUA_CONTENT = """function handle_request(msg)\n  local body = '{"code":0,"msg":"E2E_TEST_OK"}'\n  SendToClientFast(body)\n  return true\nend"""

TOKEN = f"E2E_LUA_{int(time.time())}"
TEST_LUA_TOKENIZED = TEST_LUA_CONTENT.replace("E2E_TEST_OK", TOKEN)


# ── 1. 上传 Lua 到制品库 ──

def test_lua_upload():
    """PUT /api/lua/{type}/{file} — 上传 Lua 脚本"""
    r = _put(f"/api/lua/{TEST_NODE_TYPE}/{TEST_LUA_NAME}",
             TEST_LUA_TOKENIZED.encode())
    assert r["ok"] is True
    assert r["data"]["filename"] == TEST_LUA_NAME
    assert r["data"]["size"] > 0
    assert "path" in r["data"]


def test_lua_upload_rejects_non_lua():
    """上传非 .lua 文件应被拒绝"""
    r = _put(f"/api/lua/{TEST_NODE_TYPE}/test.txt", b"hello")
    assert r["ok"] is False
    assert "only .lua" in r["error"].lower() or "lua" in r["error"].lower()


# ── 2. 制品库列表 ──

def test_lua_artifact_list():
    """GET /api/lua/{type}/files — 制品库含测试文件"""
    r = _get(f"/api/lua/{TEST_NODE_TYPE}/files")
    assert r["ok"] is True
    files = r["data"]["files"]
    assert len(files) >= 1
    names = [f["filename"] for f in files]
    assert TEST_LUA_NAME in names
    for f in files:
        assert "filename" in f
        assert "size" in f
        assert "mod_time" in f


# ── 3. 从制品库下发到 etcd ──

def test_lua_deploy():
    """POST /api/lua/{type}/deploy — 下发 Lua 到 etcd 触发热重载"""
    r = _post(f"/api/lua/{TEST_NODE_TYPE}/deploy", {
        "filename": TEST_LUA_NAME,
        "url_path": TEST_URL_PATH,
    })
    assert r["ok"] is True
    data = r["data"]
    assert data["node_type"] == TEST_NODE_TYPE
    assert data["url_path"] == TEST_URL_PATH
    assert data["filename"] == TEST_LUA_NAME
    assert data["size"] > 0
    # 首次下发 version=1，覆盖已有则 version>1
    assert "previous" in data  # 首次下发 previous=None/不存在，覆盖则有


def test_lua_deploy_auto_urlpath():
    """下发时自动推导 url_path: 文件名 → /{type_lower}/{name}"""
    # 上传另一个测试文件
    name2 = "_e2e_lua_auto.lua"
    _put(f"/api/lua/{TEST_NODE_TYPE}/{name2}", b"function h() return true end")

    r = _post(f"/api/lua/{TEST_NODE_TYPE}/deploy",
              {"filename": name2})
    assert r["ok"] is True
    assert r["data"]["url_path"] == "/hello/_e2e_lua_auto"


# ── 4. 已部署列表 ──

def test_lua_deployed_list():
    """GET /api/lua/{type} — 已部署列表含测试脚本"""
    r = _get(f"/api/lua/{TEST_NODE_TYPE}")
    assert r["ok"] is True
    scripts = r["data"]["scripts"]
    assert len(scripts) >= 1
    urls = [s["url_path"] for s in scripts]
    assert TEST_URL_PATH in urls
    for s in scripts:
        assert "script_name" in s
        assert "version" in s
        assert s["version"] >= 1


def test_lua_deployed_has_content():
    """已部署脚本 content 匹配原始内容"""
    r = _get(f"/api/lua/{TEST_NODE_TYPE}")
    scripts = r["data"]["scripts"]
    test_script = [s for s in scripts if s["url_path"] == TEST_URL_PATH]
    assert len(test_script) == 1
    assert TOKEN in test_script[0]["script_content"]


# ── 5. 版本递增 ──

def test_lua_version_bump():
    """重复下发同一 url_path → 版本自动 +1"""
    r1 = _get(f"/api/lua/{TEST_NODE_TYPE}")
    v1 = next(s["version"] for s in r1["data"]["scripts"]
              if s["url_path"] == TEST_URL_PATH)

    _post(f"/api/lua/{TEST_NODE_TYPE}/deploy", {
        "filename": TEST_LUA_NAME,
        "url_path": TEST_URL_PATH,
    })

    r2 = _get(f"/api/lua/{TEST_NODE_TYPE}")
    v2 = next(s["version"] for s in r2["data"]["scripts"]
              if s["url_path"] == TEST_URL_PATH)

    assert v2 == v1 + 1, f"version should bump: {v1} → {v2}"


# ── 6. 边界情况 ──

def test_lua_deploy_nonexistent_file():
    """下发不存在的文件 → 错误"""
    r = _post(f"/api/lua/{TEST_NODE_TYPE}/deploy",
              {"filename": "__nonexistent__.lua"})
    assert r["ok"] is False


def test_lua_empty_type_list():
    """未部署过的类型 → 返回空数组 [] 而非 null"""
    r = _get("/api/lua/HELLO_WSS")
    assert r["ok"] is True
    scripts = r["data"]["scripts"]
    assert scripts == [], f"expected [], got {type(scripts).__name__}: {scripts}"


def test_lua_artifact_empty_type():
    """无文件的类型制品库 → 返回空数组"""
    r = _get("/api/lua/NO_SUCH_TYPE/files")
    assert r["ok"] is True
    assert r["data"]["files"] == []


# ── 7. 编辑保存 (POST) ──

def test_lua_edit_save():
    """POST /api/lua/{type} — 直接编辑保存 (向后兼容)"""
    edit_token = f"EDIT_{int(time.time())}"
    content = f'function h(r)\n  return true -- {edit_token}\nend'

    r = _post(f"/api/lua/{TEST_NODE_TYPE}", {
        "url_path": "/hello/_e2e_edit",
        "script_content": content,
    })
    assert r["ok"] is True
    assert r["data"]["version"] >= 1

    # 验证已部署
    deployed = _get(f"/api/lua/{TEST_NODE_TYPE}")
    scripts = deployed["data"]["scripts"]
    found = [s for s in scripts if s["url_path"] == "/hello/_e2e_edit"]
    assert len(found) == 1
    assert edit_token in found[0]["script_content"]


# ── 8. Logic 节点 Lua ──

def test_logic_lua_artifact_and_deploy():
    """Logic 节点 Lua 制品库 + 下发"""
    # 上传
    content = f"function handle_request(msg)\n  return true -- E2E_LOGIC_TOKEN\nend"
    r = _put("/api/lua/LOGIC/_e2e_logic_test.lua", content.encode())
    assert r["ok"] is True

    # 制品库列出
    r = _get("/api/lua/LOGIC/files")
    names = [f["filename"] for f in r["data"]["files"]]
    assert "_e2e_logic_test.lua" in names

    # 下发
    r = _post("/api/lua/LOGIC/deploy", {
        "filename": "_e2e_logic_test.lua",
        "url_path": "/logic/_e2e_logic_test",
    })
    assert r["ok"] is True

    # 已部署验证
    r = _get("/api/lua/LOGIC")
    urls = [s["url_path"] for s in r["data"]["scripts"]]
    assert "/logic/_e2e_logic_test" in urls
