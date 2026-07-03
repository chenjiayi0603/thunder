#!/usr/bin/env python3
"""Lua 热重载 E2E — 独立脚本，无 pytest/conftest 依赖，可直接运行。
用法: python3 tests/e2e/test_lua_hotreload_e2e_standalone.py
依赖: Docker Compose 环境已启动，admin-web :8090 可用。
"""
import sys, time, subprocess, requests

PASS = FAIL = 0
ADMIN = "http://127.0.0.1:8090"
HELLO = "http://127.0.0.1:27006"
WLOG  = "/home/tommychen/thunder/deploy/HelloHttp/log/Hello_robot_W0.log"
MLOG  = "/home/tommychen/thunder/deploy/HelloHttp/log/Hello_robot.log"

def push_script(marker):
    requests.post(f"{ADMIN}/api/sync-config", timeout=5)
    return requests.post(f"{ADMIN}/api/lua-scripts", json={
        "node_type":"HELLO_HTTP","name":"echo.lua","url_path":"/hello/lua_echo",
        "content": "function handle_request(msg)\n  SendToClientFast('{\"code\":0,\"msg\":\""+marker+
                    "\"}')\n  return true\nend"
    }, timeout=5).json()

def wait_msg(msg, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = requests.post(f"{HELLO}/hello/lua_echo", data="t", timeout=3)
            if r.json().get("msg") == msg:
                return True
        except:
            pass
        time.sleep(1)
    return False

def check_sibling(path, timeout=5):
    try:
        r = requests.post(f"{HELLO}{path}", data="t", timeout=timeout)
        return r.status_code == 200
    except:
        return False

def count_log(pattern, file):
    return int(subprocess.run(["grep","-c",pattern,file],
            capture_output=True,text=True).stdout.strip() or 0)

# ── Test 1: 推送新脚本 → 响应变更 ──
print("=== 1/4 热重载响应变更 ===")
m1 = f"E2E_STANDALONE_{int(time.time())}"
r = push_script(m1)
if r.get("ok") and wait_msg(m1):
    print(f"  ✅ live (v={r['version']})")
    PASS += 1
else:
    print(f"  ❌ failed: {r}")
    FAIL += 1

# ── Test 2: 重载不触发 dlclose ──
print("=== 2/4 无 SO unload ===")
m2 = f"E2E_NOSO_{int(time.time())}"
before = count_log("succeed in unload.*ModuleLua", WLOG)
push_script(m2); wait_msg(m2)
after = count_log("succeed in unload.*ModuleLua", WLOG)
if after <= before:
    print(f"  ✅ no unload ({before}→{after})")
    PASS += 1
else:
    print(f"  ❌ unloaded ({before}→{after})")
    FAIL += 1

# ── Test 3: 兄弟 URL 不受影响 ──
print("=== 3/4 兄弟模块不受影响 ===")
m3 = f"E2E_SIB_{int(time.time())}"
push_script(m3); wait_msg(m3)
ok = all(check_sibling(p) for p in ["/hello/lua_limit","/hello/lua_route","/hello/lua_node_type"])
print(f"  {'✅' if ok else '❌'} {'all ok' if ok else 'some broken'}")
if ok: PASS += 1
else: FAIL += 1

# ── Test 4: Manager 日志 ──
print("=== 4/4 Manager 日志验证 ===")
m4 = f"E2E_LOG_{int(time.time())}"
push_script(m4); wait_msg(m4)
logs = subprocess.run(["grep","-a","reload.*lua scripts in-place",MLOG],
        capture_output=True,text=True).stdout.strip()
if "in-place" in logs and "graceful restart" not in logs:
    print(f"  ✅ VM only, no restart")
    PASS += 1
else:
    print(f"  ❌ log issue: {logs[:80]}")
    FAIL += 1

print(f"\n═══ {PASS}/4 passed, {FAIL} failed ═══")
sys.exit(FAIL)
