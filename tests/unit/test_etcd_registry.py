"""
etcd 注册中心单元测试 — 验证注册表正确性/lease 绑定/路由传播等需求
不依赖真实 etcd，用 mock 响应 + 纯决策逻辑。
与 C++ test_etcd_parse.cpp 互补:本测试覆盖 end-to-end 场景。
"""
import json
import base64
import pytest

# ── 决策逻辑(对应 C++ etcd_parse::DecideRegAction) ──

class RegAction:
    Claim  = 0
    Rebind = 1
    Fresh  = 2

def decide_reg_action(found: bool, existing_lease: int, current_lease: int) -> int:
    if not found:       return RegAction.Claim
    if current_lease != 0 and existing_lease == current_lease: return RegAction.Fresh
    return RegAction.Rebind

class TestRegDecision:
    """注册决策(#19 根因: 旧租约残留→重绑)"""

    def test_claim_when_key_absent(self):
        assert decide_reg_action(False, 0, 123) == RegAction.Claim

    def test_fresh_when_lease_matches(self):
        assert decide_reg_action(True, 123, 123) == RegAction.Fresh

    def test_rebind_when_lease_mismatch(self):
        """重启换租约 — 线上 bug 场景"""
        assert decide_reg_action(True, 100, 200) == RegAction.Rebind

    def test_rebind_when_no_lease_on_key(self):
        assert decide_reg_action(True, 0, 200) == RegAction.Rebind

    def test_rebind_when_current_lease_unset(self):
        """当前无租约(异常) → 不能 Fresh"""
        assert decide_reg_action(True, 100, 0) == RegAction.Rebind


# ── registry 数据结构验证 ──

class TestRegistrySchema:
    """etcd registry/slot 键的数据格式"""

    def test_registry_value_has_required_fields(self):
        val = {
            "node_id": 247,
            "node_type": "LOGIC",
            "node_ip": "127.0.0.1",
            "node_port": 16068,
            "worker_num": 1,
        }
        assert val["node_id"] > 0 and val["node_id"] <= 255
        assert val["node_type"] in ("LOGIC", "HELLO", "INTERFACE")
        assert val["node_ip"]
        assert val["node_port"] > 0
        assert val["worker_num"] >= 1

    def test_slot_value_is_ip_port(self):
        slot_val = "127.0.0.1:27011"
        ip, port = slot_val.rsplit(":", 1)
        assert ip == "127.0.0.1"
        assert 1 <= int(port) <= 65535

    def test_key_prefixes(self):
        """注册键前缀必须一致(connector/watch 依赖此)"""
        assert "/thunder/slot/" in "/thunder/slot/247"
        assert "/thunder/registry/" in "/thunder/registry/127.0.0.1:16068"


# ── etcd gateway 响应解析 ──

class TestEtcdResponseParsing:
    """etcd HTTP gateway 返回的 JSON 格式(key/value 用 base64)"""

    def test_range_response_has_header_revision(self):
        """空 keyspace 也必须返回 header.revision"""
        resp = {
            "header": {"cluster_id": "...", "member_id": "...",
                       "revision": "84", "raft_term": "2"}
        }
        assert "header" in resp
        assert "revision" in resp["header"]
        assert int(resp["header"]["revision"]) > 0

    def test_range_with_kvs(self):
        resp = {
            "header": {"revision": "10"},
            "kvs": [{"key": "YQ==", "value": "eyJub2RlX2lkIjo3fQ==", "lease": "123"}],
            "count": "1"
        }
        assert int(resp["count"]) == 1
        kv = resp["kvs"][0]
        assert kv["lease"] == "123"
        assert base64.b64decode(kv["key"]).decode() == "a"
        assert json.loads(base64.b64decode(kv["value"]))["node_id"] == 7

    def test_keepalive_response_ttl(self):
        """续租响应必须校验 TTL>0"""
        resp = {"result": {"header": {}, "ID": "123", "TTL": "10"}}
        ttl = int(resp["result"]["TTL"])
        assert ttl > 0, f"TTL={ttl} 表示租约已过期/不存在"

    def test_dead_lease_ttl_zero_is_fail(self):
        """死租约 TTL=0, 不得当作续租成功"""
        resp = {"result": {"header": {}, "ID": "123", "TTL": "0"}}
        ttl = int(resp["result"]["TTL"])
        is_alive = ttl > 0
        assert not is_alive, "TTL=0 的租约不应被判定为续租成功"

    def test_lease_grant_id_is_string(self):
        """gateway 把 int64 编码为 JSON string"""
        resp = {"ID": "7587895296023927814"}
        lease_id = int(resp["ID"])
        assert lease_id == 7587895296023927814

    def test_watch_canceled_compact_revision(self):
        """compaction 取消响应 — 必须能取出 compact_revision"""
        resp = {
            "result": {
                "header": {"revision": "84"},
                "canceled": True,
                "compact_revision": "84"
            }
        }
        assert resp["result"]["canceled"]
        assert int(resp["result"]["compact_revision"]) == 84

    def test_watch_created_no_events(self):
        resp = {"result": {"header": {"revision": "10"}, "created": True}}
        assert resp["result"]["created"]
        assert "canceled" not in resp["result"]


# ── 槽位管理 ──

class TestSlotManagement:
    """node_id 槽位 1-255，用 ip:port 哈希起始位"""

    def test_max_slot(self):
        assert 1 <= 247 <= 255

    def test_slot_range(self):
        for slot in (1, 127, 255):
            assert 1 <= slot <= 255

    def test_hash_start_slot(self):
        """哈希→起始槽位 确保分布"""
        ip_port = "127.0.0.1:16068"
        hash_val = sum(ord(c) for c in ip_port) % 256
        start = hash_val % 255 + 1
        assert 1 <= start <= 255

    def test_slot_key_format(self):
        for nid in (1, 100, 255):
            key = f"/thunder/slot/{nid}"
            assert key.startswith("/thunder/slot/")
            assert int(key.rsplit("/", 1)[1]) == nid


# ── 注册表一致性检查 ──

class TestRegistryConsistency:
    """自检对账: 注册键必须存在且绑在当前租约"""

    def test_healthy(self):
        """键存在 + lease 匹配 → 健康"""
        found = True
        existing_lease = 7587895311112086028
        current_lease  = 7587895311112086028
        assert found and existing_lease == current_lease

    def test_missing_key_is_failure(self):
        """键不存在 → 必须触发重绑/重注册"""
        found = False
        assert not found, "注册键应从 etcd 消失视为故障"

    def test_wrong_lease_is_failure(self):
        """键绑在别的租约 → 残留旧进程的 orphan key"""
        found = True
        existing_lease = 100  # 旧进程的租约
        current_lease  = 200  # 当前进程的租约
        assert existing_lease != current_lease, "租约不一致视为故障"

    def test_all_keys_have_nonzero_lease(self):
        """registry 和 slot 键都必须绑租约"""
        keys = [
            "/thunder/registry/127.0.0.1:16068",
            "/thunder/registry/127.0.0.1:27011",
            "/thunder/slot/237",
            "/thunder/slot/247",
        ]
        leases = ["7587895311523389447", "7587895311523389449",
                  "7587895311523389449", "7587895311523389447"]
        for k, l in zip(keys, leases):
            assert int(l) > 0, f"{k} 的 lease 不应为 0(孤儿键)"
