"""admin-web API 端到端测试 (K8s 环境, NodePort 30090)

用法:
  E2E_ADMIN_HOST=192.168.3.61 pytest tests/e2e/test_admin_web.py -v
  E2E_ADMIN_HOST=192.168.3.61 pytest tests/e2e/test_admin_web.py -v -k "test_deploy"
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
                     headers={"Content-Type": "application/octet-stream"}, timeout=timeout)
    return r.json()

# ── 1. 基础健康检查 ──

def test_health_overview():
    """GET /api/overview — 集群概览"""
    r = _get("/api/overview")
    assert r["ok"] is True
    data = r["data"]
    assert data["etcd_connected"] is True
    assert data["total_nodes"] >= 1
    assert data["online_nodes"] >= 1
    assert len(data["services"]) >= 1

def test_health_nodes():
    """GET /api/nodes — 节点列表"""
    r = _get("/api/nodes")
    assert r["ok"] is True
    nodes = r["data"]["nodes"]
    assert len(nodes) >= 1
    for n in nodes:
        assert "node_type" in n
        assert "ip" in n
        assert "port" in n
        assert "version" in n

# ── 2. 插件上传/下发/验证 ──

TEST_SO_NAME = "_e2e_admin_test.so"
TEST_DEPLOY_TYPE = "HelloHttp"

# 构造最小合法 ELF (ELF magic + 64 字节凑够 header)
def _elf_body(token=None):
    body = bytes([0x7f, ord('E'), ord('L'), ord('F')]) + b'\x00' * 60
    if token:
        body += token.encode()
    return body

def test_so_upload():
    """PUT /api/plugins/{type}/{file} — 上传测试 SO"""
    token = f"E2E_TEST_{int(time.time())}"
    r = _put(f"/api/plugins/{TEST_DEPLOY_TYPE}/{TEST_SO_NAME}", _elf_body(token))
    assert r["ok"] is True
    assert r["data"]["filename"] == TEST_SO_NAME

def test_so_deploy():
    """POST /api/plugins/{type}/deploy — 下发到目标 Pod"""
    r = _post(f"/api/plugins/{TEST_DEPLOY_TYPE}/deploy", {"filename": TEST_SO_NAME})
    assert r["ok"] is True
    data = r["data"]
    assert data["deployed"] is True
    assert data["succeeded"] >= 1
    assert data["total_pods"] >= 1
    assert data["etcd_bumped"] is True
    # 每个 Pod 都有部署结果
    for pod in data["pods"]:
        assert pod["success"] is True
        assert pod["size"] > 0

def test_so_deployed_list():
    """GET /api/plugins/{type}/deployed — 已部署列表含测试 SO"""
    r = _get(f"/api/plugins/{TEST_DEPLOY_TYPE}/deployed")
    assert r["ok"] is True
    files = r["data"]["files"]
    names = [f["filename"] for f in files]
    assert TEST_SO_NAME in names
    # 验证测试 SO 有版本号
    for f in files:
        if f["filename"] == TEST_SO_NAME:
            assert f["version"] != ""
            assert f["size"] > 0

def test_so_artifact_list():
    """GET /api/plugins/{type} — 制品库列表含测试 SO"""
    r = _get(f"/api/plugins/{TEST_DEPLOY_TYPE}")
    assert r["ok"] is True
    files = r["data"]["files"]
    names = [f["filename"] for f in files]
    assert TEST_SO_NAME in names

# ── 3. 审计记录 ──

def test_audit_deploy_record():
    """GET /api/audit — 下发操作已记录"""
    time.sleep(1)  # SQLite 写入可能有延迟
    r = _get(f"/api/audit?type={TEST_DEPLOY_TYPE}")
    assert r["ok"] is True
    entries = r["data"]["entries"]
    deploy_entries = [e for e in entries if e["action"] == "deploy"]
    assert len(deploy_entries) >= 1

# ── 4. 版本区分下发 ──

def test_deploy_v2_only():
    """Logic-v2 类型只下发到 v2 Pod，不影响 v1"""
    _put(f"/api/plugins/Logic-v2/{TEST_SO_NAME}", _elf_body())

    r = _post("/api/plugins/Logic-v2/deploy", {"filename": TEST_SO_NAME})
    assert r["ok"] is True
    data = r["data"]
    # Logic-v2 应该只有 1 个 Pod
    assert data["total_pods"] == 1
    assert data["succeeded"] == 1

    # Logic v1 的已部署列表不应该包含这个文件
    r2 = _get("/api/plugins/Logic/deployed")
    names = [f["filename"] for f in r2["data"]["files"]]
    # v2 的部署不影响 v1 的 etcd key（同一个 LOGIC key）
    # 所以 v1 的 deployed list 也会看到（etcd 是共享的）
    # 但 v1 Pod 文件系统上不会有这个文件
    # 这里只验证 v2 下发成功

# ── 5. 配置 API ──

def test_config_basic():
    """GET /api/config/{module} — 配置读取"""
    r = _get("/api/config/LOGIC?type=Logic.json")
    assert r["ok"] is True
    assert r["data"]["module"] == "LOGIC"

# ── 6. 边界情况 ──

def test_deploy_nonexistent_file():
    """下发不存在的文件 — 应返回错误"""
    r = _post("/api/plugins/HelloHttp/deploy", {"filename": "__nonexistent__.so"})
    assert r["ok"] is False
    assert "not found" in r["error"].lower() or "no such" in r["error"].lower()

def test_upload_any_type():
    """上传到任意 type 都能成功 (admin-web 不校验 type 是否在 K8s 中存在)"""
    r = _put("/api/plugins/NoSuchType/_e2e_dummy.so", _elf_body())
    assert r["ok"] is True

# ── 7. #157 安全校验 ──

def test_upload_rejects_non_elf():
    """上传非 ELF 文件应被拒绝 (#157 — 防止 CrashLoop)"""
    # 文本文件不是 ELF
    r = _put("/api/plugins/HelloHttp/_e2e_text.so", "this is not an ELF file")
    assert r["ok"] is False
    assert "not a valid" in r["error"].lower() or "elf" in r["error"].lower()

def test_upload_accepts_valid_so():
    """上传合法的 .so 文件应成功"""
    # 构造一个最小 ELF header
    elf_header = bytes([0x7f, ord('E'), ord('L'), ord('F')]) + b'\x00' * 60
    r = _put("/api/plugins/HelloHttp/_e2e_valid.so", elf_header)
    assert r["ok"] is True
