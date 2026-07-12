#!/usr/bin/env python3
"""Docker Compose Canary 权重路由端到端测试。

验证链路:
  etcdctl PUT 权重 → Worker Watch 感知 → 路由分发

前提: docker compose up -d 已启动（logic + logic-v2 两个服务）
不同于 test_canary_k8s.py，此文件走本地 etcd (127.0.0.1:2379)。

用法:
  PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 -m pytest tests/e2e/test_canary_compose.py -v
"""

import json, os, subprocess, sys, time, unittest

try:
    import etcd3
except ImportError:
    print("需要 pip install etcd3", file=sys.stderr)
    sys.exit(1)

COMPOSE_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "docker")
SERVICE = "LOGIC"
KEY = f"/thunder/canary/{SERVICE}/weights"
ETCD = os.environ.get("ETCD_ENDPOINT", "127.0.0.1:12379")
POLL_WAIT = 6


def etcd_client():
    host, _, port = ETCD.partition(":")
    return etcd3.client(host=host, port=int(port) if port else 2379)


def compose_exec(svc, cmd):
    """在 compose 服务容器内执行命令。"""
    r = subprocess.run(
        ["docker", "compose", "-p", "thunder-deploy", "exec", "-T", svc,
         "sh", "-c", cmd],
        cwd=COMPOSE_DIR, capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def etcdctl(*args):
    return compose_exec("etcd1", f"etcdctl {' '.join(args)}")


def worker_log(svc, pattern, tail=20):
    """从 Worker 日志中 grep（含轮转日志）。"""
    svc_log_map = {
        "hello": "Hello_robot.log",
        "logic": "Logic_robot.log",
        "interface": "Interface_robot.log",
    }
    log_file = svc_log_map.get(svc, f"{svc.capitalize()}_robot.log")
    return compose_exec(svc, f"grep -ah '{pattern}' log/{log_file} log/{log_file}.1 log/{log_file}.2 2>/dev/null | tail -{tail}")


class TestEtcdCRUD(unittest.TestCase):
    """etcd 权重 CRUD（不依赖 Worker）。"""

    SVC = "E2E_COMPOSE"
    KEY = f"/thunder/canary/{SVC}/weights"

    def setUp(self):
        self.c = etcd_client()
        self.c.delete(self.KEY)

    def tearDown(self):
        self.c.delete(self.KEY)

    def test_no_weights(self):
        v, _ = self.c.get(self.KEY)
        self.assertIsNone(v)

    def test_set_read(self):
        w = {"v1": 70, "v2": 30}
        self.c.put(self.KEY, json.dumps(w))
        v, _ = self.c.get(self.KEY)
        self.assertEqual(json.loads(v.decode()), w)

    def test_full_switch(self):
        self.c.put(self.KEY, json.dumps({"v1": 70, "v2": 30}))
        full = {"v1": 0, "v2": 100}
        self.c.put(self.KEY, json.dumps(full))
        v, _ = self.c.get(self.KEY)
        self.assertEqual(json.loads(v.decode()), full)

    def test_delete(self):
        self.c.put(self.KEY, json.dumps({"v1": 100}))
        self.assertTrue(self.c.delete(self.KEY))
        v, _ = self.c.get(self.KEY)
        self.assertIsNone(v)


class TestWorkerWatch(unittest.TestCase):
    """Worker Watch 实时感知 canary 权重变更。"""

    def setUp(self):
        self.c = etcd_client()
        self.c.delete(KEY)

    def tearDown(self):
        self.c.delete(KEY)

    def test_watch_put(self):
        """Hello Worker Watch 感知 PUT。"""
        self.c.put(KEY, json.dumps({"v1": 70, "v2": 30}))
        time.sleep(POLL_WAIT)
        log = worker_log("hello", "CanaryParsed.*LOGIC", tail=5)
        self.assertIn("CanaryParsed", log,
                      f"Hello Worker 未感知 PUT\nlog: {log}")

    def test_watch_delete(self):
        """Hello Worker Watch 感知 DELETE。"""
        self.c.put(KEY, json.dumps({"v1": 100}))
        time.sleep(3)
        self.c.delete(KEY)
        time.sleep(POLL_WAIT)
        log = worker_log("hello", "CanaryWatch.*DELETE.*LOGIC", tail=5)
        self.assertIn("DELETE LOGIC", log,
                      f"Hello Worker 未感知 DELETE\nlog: {log}")

    def test_v1_v2_registered(self):
        """验证 logic (v1) 和 logic-v2 (v2) 都已注册到 etcd。"""
        raw = etcdctl("get", "--prefix", "/thunder/registry/LOGIC/")
        self.assertIn("node_version", raw, "etcd 中无 LOGIC 注册")
        self.assertIn('"v1"', raw, "logic (v1) 未注册")
        self.assertIn('"v2"', raw, "logic-v2 (v2) 未注册")

    def test_echo_unaffected(self):
        """设置 canary 权重后业务接口不受影响。"""
        self.c.put(KEY, json.dumps({"v1": 100}))
        time.sleep(3)
        try:
            r = subprocess.run(
                ["curl", "-s", "-m5", "-X", "POST",
                 "http://127.0.0.1:27006/hello/hello",
                 "-H", "Content-Type: application/json",
                 "-d", '{"option":"Echo","size":5}'],
                capture_output=True, text=True, timeout=10)
            resp = json.loads(r.stdout)
            self.assertEqual(resp.get("code"), 0, resp)
        finally:
            self.c.delete(KEY)


class TestWeightDistribution(unittest.TestCase):
    """权重分发统计验证。"""

    def test_distribution_70_30(self):
        self.c = etcd_client()
        self.c.delete(KEY)
        try:
            self.c.put(KEY, json.dumps({"v1": 70, "v2": 30}))
            time.sleep(POLL_WAIT)

            v, _ = self.c.get(KEY)
            weights = json.loads(v.decode())
            total = sum(weights.values())

            import random
            random.seed(42)
            counts = {n: 0 for n in weights}
            N = 10000
            for _ in range(N):
                r = random.randint(0, total - 1)
                acc = 0
                for ver, w in weights.items():
                    acc += w
                    if r < acc:
                        counts[ver] += 1
                        break

            for ver, w in weights.items():
                expected_pct = w / total * 100
                actual_pct = counts[ver] / N * 100
                diff = abs(actual_pct - expected_pct)
                self.assertLess(diff, 2.5,
                    f"{ver}: 期望 {expected_pct:.1f}% 实际 {actual_pct:.1f}% 偏差 {diff:.1f}%")
        finally:
            self.c.delete(KEY)

    def test_zero_weight_excluded(self):
        self.c = etcd_client()
        self.c.delete(KEY)
        try:
            self.c.put(KEY, json.dumps({"v1": 100, "v2": 0}))
            time.sleep(POLL_WAIT)

            v, _ = self.c.get(KEY)
            weights = json.loads(v.decode())
            import random
            random.seed(42)
            counts = {n: 0 for n in weights}
            total = sum(weights.values())
            N = 5000
            for _ in range(N):
                r = random.randint(0, total - 1)
                acc = 0
                for ver, w in weights.items():
                    acc += w
                    if r < acc:
                        counts[ver] += 1
                        break

            self.assertEqual(counts.get("v2", 0), 0,
                f"权重=0 的 v2 收到 {counts.get('v2', 0)} 次")
            self.assertEqual(counts.get("v1", 0), N)
        finally:
            self.c.delete(KEY)


if __name__ == "__main__":
    print(f"Canary Compose E2E  etcd={ETCD}  service={SERVICE}")
    unittest.main(argv=[sys.argv[0], "-v"], verbosity=2)
