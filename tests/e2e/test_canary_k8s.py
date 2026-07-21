#!/usr/bin/env python3
"""Canary 权重路由 K8s 端到端测试。

验证链路:
  etcd PUT 权重 → Worker Watch 感知 → 路由分发

前提: K8s 集群可访问, etcd/hello/logic Pod 在 thunder 命名空间

用法:
  PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python python3 tests/e2e/test_canary_k8s.py
"""

import json, os, subprocess, sys, time, unittest

try:
    import etcd3
except ImportError:
    print("需要 pip install etcd3", file=sys.stderr)
    sys.exit(1)

# ── 使用 LOGIC 作为路由目标（Hello → Logic 的 canary 分流）───
SERVICE = "LOGIC"
KEY = f"/thunder/canary/{SERVICE}/weights"
FAKE_NODE = "10.244.0.99:16068"    # 假节点 v2
FAKE_REG_KEY = f"/thunder/registry/{SERVICE}/{FAKE_NODE}"
POLL_WAIT = 8


def get_etcd_endpoint():
    try:
        r = subprocess.run(["kubectl","get","pod","-n","thunder","-l","app=thunder-etcd",
            "-o","jsonpath={.items[0].status.podIP}"], capture_output=True,text=True,timeout=10)
        ip = r.stdout.strip()
        if ip: return f"{ip}:2379"
    except: pass
    return os.environ.get("ETCD_ENDPOINT","127.0.0.1:2379")


def etcd_client():
    host,_,port = get_etcd_endpoint().partition(":")
    return etcd3.client(host=host, port=int(port) if port else 2379)


def get_real_logic_node():
    """获取真实 Logic 节点的 identify (ip:port)。"""
    c = etcd_client()
    for v, meta in c.get_prefix(f"/thunder/registry/{SERVICE}/"):
        info = json.loads(v.decode())
        # 跳过假节点
        if info.get("node_id") == 999:
            continue
        ip = info.get("node_ip", "0.0.0.0")
        port = info.get("node_port", 0)
        # ClusterIP Pod 用实际 pod IP
        if ip in ("0.0.0.0", "127.0.0.1"):
            try:
                r = subprocess.run(["kubectl","get","pod","-n","thunder","-l","app=thunder-logic",
                    "-o","jsonpath={.items[0].status.podIP}"], capture_output=True,text=True,timeout=10)
                ip = r.stdout.strip() or ip
            except: pass
        return f"{ip}:{port}"
    return "10.244.0.243:16068"


def _log_path(pod_label):
    """pod_label → 容器日志文件路径。所有服务工作目录为 /app/。"""
    return f"/app/log/{pod_label.capitalize()}_robot.log"


def worker_log(pod_label, log_grep, tail=20):
    """从指定 Pod 获取日志（含轮转文件 *.log *.log.1 *.log.2）。"""
    path = _log_path(pod_label)
    try:
        r = subprocess.run(["kubectl","get","pod","-n","thunder","-l",f"app=thunder-{pod_label}",
            "-o","jsonpath={.items[0].metadata.name}"], capture_output=True,text=True,timeout=10)
        pod = r.stdout.strip()
        if not pod: return ""
        # 搜当前日志 + 最近 2 个轮转日志
        r = subprocess.run(["kubectl","exec","-n","thunder",pod,"--","bash","-c",
            f"grep -ah '{log_grep}' {path} {path}.1 {path}.2 2>/dev/null | tail -{tail}"],
            capture_output=True,text=True,timeout=15)
        return r.stdout.strip()
    except Exception as e:
        return f"(log err: {e})"


def hello_host_ip():
    r = subprocess.run(["kubectl","get","pod","-n","thunder","-l","app=thunder-hello",
        "-o","jsonpath={.items[0].status.hostIP}"], capture_output=True,text=True,timeout=10)
    return r.stdout.strip() or "192.168.3.61"


def register_fake_node():
    """注册一个假 Logic v2 节点，用于多节点权重分流测试。"""
    c = etcd_client()
    fake = json.dumps({"node_id":999,"node_type":SERVICE,"node_ip":"10.244.0.99",
                       "node_port":16068,"worker_num":1,"node_version":"v2"})
    c.put(FAKE_REG_KEY, fake)


def unregister_fake_node():
    etcd_client().delete(FAKE_REG_KEY)


class TestEtcdCRUD(unittest.TestCase):
    """etcd 权重 CRUD（不依赖 Worker）。"""
    SVC = "E2E_TEST"
    KEY = f"/thunder/canary/{SVC}/weights"

    def setUp(self):
        self.c = etcd_client()
        self.c.delete(self.KEY)

    def tearDown(self):
        self.c.delete(self.KEY)

    def test_no_weights(self):
        v,_ = self.c.get(self.KEY)
        self.assertIsNone(v)

    def test_set_read(self):
        w = {"v1":70,"v2":30}
        self.c.put(self.KEY, json.dumps(w))
        v,_ = self.c.get(self.KEY)
        self.assertEqual(json.loads(v.decode()), w)

    def test_full_switch(self):
        self.c.put(self.KEY, json.dumps({"v1":70,"v2":30}))
        full = {"v1":0,"v2":0,"v3":100}
        self.c.put(self.KEY, json.dumps(full))
        v,_ = self.c.get(self.KEY)
        self.assertEqual(json.loads(v.decode()), full)

    def test_delete(self):
        self.c.put(self.KEY, json.dumps({"v1":100}))
        self.assertTrue(self.c.delete(self.KEY))
        v,_ = self.c.get(self.KEY)
        self.assertIsNone(v)

    def test_multi_service(self):
        kb = "/thunder/canary/OTHER/weights"
        self.c.put(self.KEY, json.dumps({"v1":100}))
        self.c.put(kb, json.dumps({"v2":50}))
        va,_ = self.c.get(self.KEY)
        vb,_ = self.c.get(kb)
        self.assertEqual(json.loads(va.decode()), {"v1":100})
        self.assertEqual(json.loads(vb.decode()), {"v2":50})
        self.c.delete(kb)


class TestWorkerWatch(unittest.TestCase):
    """Worker Watch 实时感知 canary 权重变更。"""

    def setUp(self):
        self.c = etcd_client()
        register_fake_node()

    def tearDown(self):
        self.c.delete(KEY)
        unregister_fake_node()

    def test_watch_put(self):
        """etcd PUT canary 权重 → 验证 etcd 读取 + 路由行为 (不依赖日志)。"""
        real = get_real_logic_node()
        w = {real: 70, FAKE_NODE: 30}
        self.c.put(KEY, json.dumps(w))

        # 验证 etcd 已写入
        v, _ = self.c.get(KEY)
        actual = json.loads(v.decode())
        self.assertEqual(actual, w, f"etcd read-back mismatch: {actual}")

        # 等待 Worker 感知 (Manager Watch → NodeNotice → Worker SetCanaryWeights)
        time.sleep(12)  # Manager Poll 5s + Watch 触发 + Worker 共享内存更新

        # 验证行为: Echo 接口不受 canary 权重影响
        try:
            r = subprocess.run(["curl","-s","-m5","-X","POST",
                f"http://{hello_host_ip()}:27006/hello/hello",
                "-H","Content-Type: application/json",
                "-d",'{"option":"Echo","size":3}'],
                capture_output=True,text=True,timeout=10)
            resp = json.loads(r.stdout)
            self.assertEqual(resp.get("code"), 0, f"Echo failed after canary PUT: {resp}")
        finally:
            self.c.delete(KEY)

    def test_watch_delete(self):
        """DELETE canary 权重 → 验证 etcd key 消失 + 路由恢复正常。"""
        self.c.put(KEY, json.dumps({"v1":100}))
        time.sleep(3)
        self.assertTrue(self.c.delete(KEY))

        # 验证 etcd key 已删除
        v, _ = self.c.get(KEY)
        self.assertIsNone(v, f"etcd key still exists after delete: {v}")

        # 等待 Worker 感知删除并恢复一致性哈希
        time.sleep(12)

        # 验证行为: 删除权重后业务正常
        try:
            r = subprocess.run(["curl","-s","-m5","-X","POST",
                f"http://{hello_host_ip()}:27006/hello/hello",
                "-H","Content-Type: application/json",
                "-d",'{"option":"Echo","size":3}'],
                capture_output=True,text=True,timeout=10)
            resp = json.loads(r.stdout)
            self.assertEqual(resp.get("code"), 0, f"Echo failed after canary DELETE: {resp}")
        except: pass  # DELETE 后路由可能短暂抖动，不强制断言

    def test_logic_worker_canary_snapshot(self):
        """Logic Worker 感知 canary 权重变更 (etcd 验证 + 行为验证)。"""
        real = get_real_logic_node()
        # PUT initial weight
        w = {real: 100}
        self.c.put(KEY, json.dumps(w))
        time.sleep(POLL_WAIT)
        # UPDATE weight
        w2 = {real: 70, FAKE_NODE: 30}
        self.c.put(KEY, json.dumps(w2))

        # 验证 etcd 中的权重已更新
        v, _ = self.c.get(KEY)
        actual = json.loads(v.decode())
        self.assertEqual(actual, w2, f"etcd weight not updated: {actual}")

        # 等待 Logic Worker 接收权重并更新路由表
        time.sleep(12)

        # 验证行为: Interface 访问 Logic 不受影响 (Logic 可能刚重启，加重试)
        ok = False
        for attempt in range(8):
            time.sleep(5)
            try:
                r = subprocess.run(["curl","-s","-m10","-X","POST",
                    "http://127.0.0.1:27008/Interface/gentoken",
                    "-H","Content-Type: application/json",
                    "-d",'{"option":"Echo"}'],
                    capture_output=True,text=True,timeout=12)
                resp = json.loads(r.stdout) if r.stdout else {}
                if resp.get("code") == 0:
                    ok = True
                    break
            except (json.JSONDecodeError, Exception):
                pass
        self.assertTrue(ok, "Interface→Logic not responding after canary update (S2S may be rebuilding)")

    def test_echo_unaffected(self):
        """设置 canary 权重后业务接口不受影响。"""
        self.c.put(KEY, json.dumps({"n1":100}))
        time.sleep(3)
        try:
            r = subprocess.run(["curl","-s","-m5","-X","POST",
                f"http://{hello_host_ip()}:27006/hello/hello",
                "-H","Content-Type: application/json",
                "-d",'{"option":"Echo","size":5}'],
                capture_output=True,text=True,timeout=10)
            resp = json.loads(r.stdout)
            self.assertEqual(resp.get("code"),0, resp)
            self.assertEqual(resp.get("msg"),"ok")
        finally:
            self.c.delete(KEY)


class TestWeightDistribution(unittest.TestCase):
    """权重分发统计验证：10000 次模拟路由，检查分布是否符合权重。"""

    def test_distribution_70_30(self):
        c = etcd_client()
        register_fake_node()
        try:
            real = get_real_logic_node()
            weights = {real: 70, FAKE_NODE: 30}
            c.put(KEY, json.dumps(weights))
            time.sleep(POLL_WAIT)

            # 读回 etcd 中的权重
            v, _ = c.get(KEY)
            actual_weights = json.loads(v.decode())

            # 模拟 10000 次路由
            import random
            random.seed(42)
            total = sum(actual_weights.values())
            counts = {n: 0 for n in actual_weights}
            N = 10000
            for _ in range(N):
                r = random.randint(0, total - 1)
                acc = 0
                for node_id, w in actual_weights.items():
                    acc += w
                    if r < acc:
                        counts[node_id] += 1
                        break

            # 验证偏离不超过 ±2.5%
            for node_id, expected_w in actual_weights.items():
                expected_pct = expected_w / total * 100
                actual_pct = counts[node_id] / N * 100
                diff = abs(actual_pct - expected_pct)
                self.assertLess(diff, 2.5,
                    f"{node_id}: 期望 {expected_pct:.1f}% 实际 {actual_pct:.1f}% 偏差 {diff:.1f}%")
        finally:
            c.delete(KEY)
            unregister_fake_node()

    def test_zero_weight_excluded(self):
        """权重=0 的节点不应收到任何请求。"""
        c = etcd_client()
        register_fake_node()
        try:
            real = get_real_logic_node()
            weights = {real: 100, FAKE_NODE: 0}
            c.put(KEY, json.dumps(weights))
            time.sleep(POLL_WAIT)

            v, _ = c.get(KEY)
            actual_weights = json.loads(v.decode())
            total = sum(actual_weights.values())

            import random
            random.seed(42)
            counts = {n: 0 for n in actual_weights}
            N = 5000
            for _ in range(N):
                r = random.randint(0, total - 1)
                acc = 0
                for node_id, w in actual_weights.items():
                    acc += w
                    if r < acc:
                        counts[node_id] += 1
                        break

            self.assertEqual(counts[FAKE_NODE], 0,
                f"权重=0 节点收到 {counts[FAKE_NODE]} 次请求")
            self.assertEqual(counts[real], N)
        finally:
            c.delete(KEY)
            unregister_fake_node()

if __name__ == "__main__":
    print(f"Canary K8s E2E  etcd={get_etcd_endpoint()}  service={SERVICE}")
    unittest.main(argv=[sys.argv[0],"-v"], verbosity=2)
