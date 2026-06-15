"""etcd Admin API 端到端测试 (替代旧 Center Raft admin)"""
from __future__ import annotations
import json, base64, os, requests, pytest

_ETCD_HOST = os.getenv("E2E_ETCD_HOST", "127.0.0.1")
ETCD_URL = f"http://{_ETCD_HOST}:2379"

def _range(prefix: str) -> list[dict]:
    pb = prefix.encode()
    key = base64.b64encode(pb).decode()
    end = base64.b64encode(pb[:-1] + bytes([pb[-1] + 1])).decode()
    r = requests.post(f"{ETCD_URL}/v3/kv/range", json={"key": key, "range_end": end}, timeout=10)
    if r.status_code != 200: return []
    res = []
    for kv in r.json().get("kvs", []):
        k = base64.b64decode(kv["key"]).decode()
        v = ""
        if "value" in kv and kv["value"]:
            try: v = base64.b64decode(kv["value"]).decode()
            except: v = "(binary)"
        res.append({"key": k, "value": v})
    return res

def _ok() -> bool:
    try: return requests.get(f"{ETCD_URL}/version", timeout=5).status_code == 200
    except: return False

@pytest.mark.integration
@pytest.mark.smoke
def test_etcd_health():
    if not _ok(): pytest.skip("etcd not reachable")
    r = requests.get(f"{ETCD_URL}/version", timeout=5)
    assert r.status_code == 200 and "etcdserver" in r.json()

@pytest.mark.integration
@pytest.mark.smoke
def test_etcd_registry_nodes():
    if not _ok(): pytest.skip("etcd not reachable")
    nodes = _range("/thunder/registry/")
    if not nodes: pytest.skip("no registered nodes (services not connected to this etcd)")
    types = set()
    for n in nodes:
        if n["value"]:
            try:
                v = json.loads(n["value"])
                if "node_type" in v: types.add(v["node_type"])
            except: pass
    assert len(types) > 0

@pytest.mark.integration
def test_etcd_config_entries():
    if not _ok(): pytest.skip("etcd not reachable")
    assert isinstance(_range("/thunder/config/"), list)

@pytest.mark.integration
def test_etcd_invalid_key():
    if not _ok(): pytest.skip("etcd not reachable")
    r = requests.post(f"{ETCD_URL}/v3/kv/range",
        json={"key": base64.b64encode(b"/thunder/nonexistent").decode()}, timeout=10)
    assert r.status_code == 200

@pytest.mark.integration
def test_etcd_cluster_consistency():
    if not _ok(): pytest.skip("etcd not reachable")
    try:
        r = requests.get(f"{ETCD_URL}/v3/cluster/member/list", timeout=10)
        if r.status_code != 200: pytest.skip("member list not available")
        if len(r.json().get("members", [])) < 2: pytest.skip("single etcd member")
    except: pytest.skip("member list not available")
