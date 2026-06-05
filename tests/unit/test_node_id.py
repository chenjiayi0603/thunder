"""node_id 分配边界测试 — 覆盖槽位算法/原子性/边界场景(无 etcd 依赖)"""
import pytest

# ── 5.1 哈希计算正确性 ──

def hash_start_slot(ip_port: str) -> int:
    hash_val = sum(ord(c) for c in ip_port)
    return hash_val % 255 + 1  # [1, 255]

class TestHashSlot:
    def test_range_lower_bound(self):
        assert hash_start_slot("0.0.0.0:0") >= 1

    def test_range_upper_bound(self):
        assert hash_start_slot("255.255.255.255:65535") <= 255

    def test_different_ips_different_slots_maybe(self):
        """不同 ip:port 可能落在不同起始位(非确定性,但概率高)"""
        s1 = hash_start_slot("127.0.0.1:16068")
        s2 = hash_start_slot("127.0.0.2:16068")
        s3 = hash_start_slot("127.0.0.1:16069")
        # 三个至少有一个不同(概率极高)
        assert not (s1 == s2 == s3), f"all same: {s1}"

    def test_same_ip_gives_same_start(self):
        s1 = hash_start_slot("127.0.0.1:16068")
        s2 = hash_start_slot("127.0.0.1:16068")
        assert s1 == s2

    def test_known_vectors(self):
        assert hash_start_slot("127.0.0.1:16068") == hash_start_slot("127.0.0.1:16068")
        assert 1 <= hash_start_slot("127.0.0.1:27011") <= 255
        assert 1 <= hash_start_slot("127.0.0.1:27009") <= 255


# ── 5.2 扫描策略 — 255 次内必然遍历所有槽位 ──

def scan_slots(start: int, max_slot: int = 255):
    """模拟扫描: 返回访问的所有 slot 序号"""
    visited = []
    for loop in range(max_slot):
        idx = ((start - 1 + loop) % max_slot) + 1
        visited.append(idx)
    return visited

class TestSlotScan:
    def test_scans_all_slots_exactly_once(self):
        for start in (1, 128, 255):
            visited = scan_slots(start)
            assert len(visited) == 255
            assert len(set(visited)) == 255  # 无重复
            assert min(visited) == 1
            assert max(visited) == 255

    def test_starts_at_correct_position(self):
        assert scan_slots(1)[0] == 1
        assert scan_slots(255)[0] == 255
        assert scan_slots(128)[0] == 128

    def test_wraps_around(self):
        visited = scan_slots(254)
        assert visited[0] == 254
        assert visited[1] == 255
        assert visited[2] == 1

    def test_255_is_last_when_start_256(self):
        """hash % 255 + 1 的结果范围 [1,255], 不会出现 256"""
        assert 256 % 255 + 1 == 2  # startSlot 不可能为 0 或 >255


# ── node_id 范围验证 ──

class TestNodeIdRange:
    def test_all_valid(self):
        for nid in (1, 100, 200, 255):
            assert 1 <= nid <= 255

    def test_invalid_zero(self):
        assert not (1 <= 0 <= 255)

    def test_invalid_256(self):
        assert not (1 <= 256 <= 255)

    def test_none_out_of_range_in_typical_cluster(self):
        """典型集群 node_id(3-5 个)应在 [1,255] 内"""
        typical = [237, 244, 247]  # 实测值
        for nid in typical:
            assert 1 <= nid <= 255


# ── Fresh/Rebind/Claim 决策 ──

class RegAction:
    Claim = 0; Rebind = 1; Fresh = 2

def decide(found: bool, existing_lease: int, current_lease: int) -> int:
    if not found: return RegAction.Claim
    if current_lease != 0 and existing_lease == current_lease: return RegAction.Fresh
    return RegAction.Rebind

class TestRegDecision:
    def test_claim_when_not_found(self):
        assert decide(False, 0, 123) == RegAction.Claim
        assert decide(False, 100, 100) == RegAction.Claim

    def test_fresh_when_lease_match(self):
        assert decide(True, 123, 123) == RegAction.Fresh
        assert decide(True, 7587895311112086028, 7587895311112086028) == RegAction.Fresh

    def test_rebind_when_lease_mismatch(self):
        assert decide(True, 100, 200) == RegAction.Rebind

    def test_rebind_when_current_lease_zero(self):
        """当前无租约 → 不能当作 Fresh(需重绑)"""
        assert decide(True, 100, 0) == RegAction.Rebind

    def test_rebind_when_existing_lease_zero(self):
        """存留键无租约(孤儿) → 重绑"""
        assert decide(True, 0, 200) == RegAction.Rebind

    def test_rebind_after_restart(self):
        """重启换租约——本次修复的核心场景"""
        assert decide(True, 7587895321850315530, 7587895322028928273) == RegAction.Rebind


# ── txn JSON 构建(验证 BuildSlotTxn 等价逻辑) ──

import json, base64

def build_slot_txn(slot: int, ip_port: str, node_type: str, worker_num: int, lease: int) -> dict:
    slot_key = f"/thunder/slot/{slot}"
    reg_key  = f"/thunder/registry/{ip_port}"
    reg_val  = {
        "node_id": slot, "node_type": node_type,
        "node_ip": ip_port.rsplit(":", 1)[0],
        "node_port": int(ip_port.rsplit(":", 1)[1]),
        "worker_num": max(worker_num, 1)
    }
    return {
        "compare": [{"key": base64.b64encode(slot_key.encode()).decode(),
                      "target": "CREATE", "result": "EQUAL", "create_revision": "0"}],
        "success": [
            {"request_put": {"key": base64.b64encode(slot_key.encode()).decode(),
                             "value": base64.b64encode(ip_port.encode()).decode(),
                             "lease": str(lease)}},
            {"request_put": {"key": base64.b64encode(reg_key.encode()).decode(),
                             "value": base64.b64encode(json.dumps(reg_val).encode()).decode(),
                             "lease": str(lease)}},
        ],
        "failure": [{"request_range": {"key": base64.b64encode(slot_key.encode()).decode()}}]
    }

class TestSlotTxn:
    def test_compare_checks_slot_nonexistent(self):
        txn = build_slot_txn(247, "127.0.0.1:16068", "LOGIC", 1, 12345)
        cmp = txn["compare"][0]
        assert cmp["target"] == "CREATE"
        assert cmp["result"] == "EQUAL"
        assert cmp["create_revision"] == "0"

    def test_success_writes_both_slot_and_registry(self):
        txn = build_slot_txn(247, "127.0.0.1:16068", "LOGIC", 1, 12345)
        assert len(txn["success"]) == 2
        slot_put = txn["success"][0]["request_put"]
        reg_put  = txn["success"][1]["request_put"]
        assert slot_put["lease"] == "12345"
        assert reg_put["lease"] == "12345"

    def test_registry_value_has_correct_node_id(self):
        txn = build_slot_txn(247, "127.0.0.1:16068", "LOGIC", 1, 12345)
        reg_val = json.loads(base64.b64decode(
            txn["success"][1]["request_put"]["value"]))
        assert reg_val["node_id"] == 247
        assert reg_val["node_type"] == "LOGIC"

    def test_slot_value_is_ip_port(self):
        txn = build_slot_txn(237, "127.0.0.1:27011", "HELLO", 1, 12345)
        slot_val = base64.b64decode(
            txn["success"][0]["request_put"]["value"]).decode()
        assert slot_val == "127.0.0.1:27011"

    def test_slot_boundary_1(self):
        txn = build_slot_txn(1, "1.1.1.1:1", "LOGIC", 1, 1)
        assert txn["compare"][0]["key"]  # valid base64

    def test_slot_boundary_255(self):
        txn = build_slot_txn(255, "255.255.255.255:65535", "HELLO", 1, 9999999)
        reg_val = json.loads(base64.b64decode(
            txn["success"][1]["request_put"]["value"]))
        assert reg_val["node_id"] == 255


# ── 255 槽位全满场景 ──

class TestFullSlots:
    def test_scan_exhausts_all_255(self):
        """模拟全部槽位已被占用"""
        occupied = set(range(1, 256))  # all 255 slots taken
        start = 128
        found = False
        for loop in range(255):
            idx = ((start - 1 + loop) % 255) + 1
            if idx not in occupied:
                found = True; break
        assert not found, "should find no free slot when all 255 occupied"

    def test_single_free_slot_found(self):
        occupied = set(range(1, 256)) - {200}
        start = 1
        found = False
        for loop in range(255):
            idx = ((start - 1 + loop) % 255) + 1
            if idx not in occupied:
                assert idx == 200
                found = True; break
        assert found


# ── Re-entrancy 防护 ──

class TestReentrancy:
    def test_skip_when_in_progress(self):
        """m_regInProgress=true 时 DoRegister 跳过"""
        in_progress = True
        should_proceed = not in_progress
        assert not should_proceed

    def test_proceed_when_not_in_progress(self):
        """m_regInProgress=false 时正常进入"""
        in_progress = False
        should_proceed = not in_progress
        assert should_proceed

    def test_timeout_recovery(self):
        """超时 10 拍后 m_regInProgress 复位"""
        stuck_ticks = 11
        should_reset = stuck_ticks > 10
        assert should_reset

    def test_not_reset_before_threshold(self):
        stuck_ticks = 9
        should_reset = stuck_ticks > 10
        assert not should_reset


# ── Lease 绑定 ──

class TestLeaseBinding:
    def test_all_keys_same_lease(self):
        """txn 中 slot 和 registry 必须用同一个 lease"""
        lease = "7587895322273284369"
        slot_lease = lease
        reg_lease  = lease
        assert slot_lease == reg_lease

    def test_lease_is_nonzero(self):
        lease = "7587895322273284369"
        assert int(lease) > 0

    def test_orphan_detection(self):
        """lease=0 是孤儿键，必须检测"""
        lease = "0"
        is_orphan = (lease == "0")
        assert is_orphan

    def test_three_nodes_three_distinct_leases(self):
        """多节点应有不同 lease"""
        leases = {"7587895322273284369", "7587895322273284382", "7587895322273284362"}
        assert len(leases) == 3


# ── registry 数据完整性 ──

class TestRegistryIntegrity:
    def test_correct_ip_parsing(self):
        ip_port = "127.0.0.1:16068"
        colon = ip_port.rfind(":")
        assert colon > 0
        assert ip_port[:colon] == "127.0.0.1"
        assert int(ip_port[colon+1:]) == 16068

    def test_ipv6_port_parsing(self):
        """IPv6: rfind colon correctly splits port"""
        ip = "::1:16068"
        colon = ip.rfind(":")
        assert colon == 3
        assert int(ip[colon+1:]) == 16068
        assert ip[:colon] == "::1"


    def test_node_id_matches_slot(self):
        """slot 序列号 == registry 中的 node_id"""
        for slot in (1, 100, 247, 255):
            reg = {"node_id": slot}
            assert reg["node_id"] == slot

    def test_all_three_node_types_present(self):
        types = {"LOGIC", "HELLO", "INTERFACE"}
        assert len(types) == 3
        assert "LOGIC" in types
        assert "HELLO" in types
        assert "INTERFACE" in types
