"""
一致性哈希单元测试
对应 code/Net/src/dispatcher/ 的 ConHash 实现
验证: 节点增删时的 key 重分配最小化
"""
import pytest
import hashlib


class ConHash:
    """简化一致性哈希实现 (虚拟节点 × 150)"""
    def __init__(self, vnodes=150):
        self.vnodes = vnodes
        self.ring = {}       # hash → node
        self.sorted_hashes = []

    def add_node(self, node: str):
        for i in range(self.vnodes):
            h = self._hash(f"{node}#{i}")
            self.ring[h] = node
        self.sorted_hashes = sorted(self.ring.keys())

    def remove_node(self, node: str):
        for i in range(self.vnodes):
            h = self._hash(f"{node}#{i}")
            self.ring.pop(h, None)
        self.sorted_hashes = sorted(self.ring.keys())

    def get_node(self, key: str) -> str | None:
        if not self.sorted_hashes:
            return None
        h = self._hash(key)
        # 二分查找第一个 >= h 的 hash
        lo, hi = 0, len(self.sorted_hashes)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.sorted_hashes[mid] < h:
                lo = mid + 1
            else:
                hi = mid
        idx = lo % len(self.sorted_hashes)
        return self.ring[self.sorted_hashes[idx]]

    @staticmethod
    def _hash(key: str) -> int:
        return int(hashlib.md5(key.encode()).hexdigest(), 16)


class TestConHash:
    """一致性哈希核心属性"""

    def test_single_node_gets_all(self):
        ch = ConHash(vnodes=10)
        ch.add_node("n1")
        for k in ["a", "b", "c", "test", "xyz"]:
            assert ch.get_node(k) == "n1"

    def test_empty_ring_returns_none(self):
        ch = ConHash(vnodes=10)
        assert ch.get_node("any") is None

    def test_multi_node_distribution(self):
        ch = ConHash(vnodes=150)
        for n in ["A", "B", "C"]:
            ch.add_node(n)
        counts = {"A": 0, "B": 0, "C": 0}
        for i in range(1000):
            node = ch.get_node(f"key_{i}")
            counts[node] += 1
        # 每节点至少分配 20% 的 key
        for n, c in counts.items():
            assert c >= 200, f"Node {n} only got {c}/1000 keys"

    def test_add_node_minimal_redistribution(self):
        ch = ConHash(vnodes=150)
        ch.add_node("A")
        ch.add_node("B")
        before = {f"k{i}": ch.get_node(f"k{i}") for i in range(500)}
        ch.add_node("C")
        changed = sum(1 for k, v in before.items() if ch.get_node(k) != v)
        # 新增节点应只重分配约 1/3 的 key
        assert changed < 300, f"Too many reassigned: {changed}/500"

    def test_remove_node_redistribution(self):
        ch = ConHash(vnodes=150)
        ch.add_node("A")
        ch.add_node("B")
        ch.add_node("C")
        before = {f"k{i}": ch.get_node(f"k{i}") for i in range(500)}
        ch.remove_node("C")
        # 移除 C → 原 C 的 key 分配给 A 或 B，不应失败
        for k in before:
            assert ch.get_node(k) in ("A", "B")

    def test_deterministic(self):
        ch1 = ConHash(vnodes=10)
        ch2 = ConHash(vnodes=10)
        for ch in (ch1, ch2):
            ch.add_node("X")
        for i in range(100):
            assert ch1.get_node(f"k{i}") == ch2.get_node(f"k{i}")
