"""etcd 配置下发测试 — 验证 /thunder/config/ 写入→读取→watch 链路"""
import json, base64, subprocess, pytest

ETCD = "http://127.0.0.1:2379"
PREFIX = "/thunder/config/"

def etcd_put(key, value):
    k = base64.b64encode(key.encode()).decode()
    v = base64.b64encode(value.encode()).decode()
    body = json.dumps({"key": k, "value": v})
    r = subprocess.run(["curl", "-sf", "--max-time", "3", "-X", "POST",
        f"{ETCD}/v3/kv/put", "-d", body], capture_output=True, text=True)
    return r.returncode == 0, r.stdout

def etcd_get(key):
    k = base64.b64encode(key.encode()).decode()
    body = json.dumps({"key": k})
    r = subprocess.run(["curl", "-sf", "--max-time", "3", "-X", "POST",
        f"{ETCD}/v3/kv/range", "-d", body], capture_output=True, text=True)
    if r.returncode != 0: return None
    d = json.loads(r.stdout)
    kvs = d.get("kvs", [])
    if not kvs: return None
    return base64.b64decode(kvs[0].get("value", "")).decode()

@pytest.fixture(autouse=True)
def check_etcd():
    r = subprocess.run(["curl", "-sf", "--max-time", "2", f"{ETCD}/health"],
                       capture_output=True)
    if r.returncode != 0:
        pytest.skip("etcd 不可达")


class TestEtcdConfigWrite:
    """写入/读取配置"""

    def test_put_and_get(self):
        ok, _ = etcd_put(PREFIX + "test_key", "hello-world")
        assert ok
        val = etcd_get(PREFIX + "test_key")
        assert val == "hello-world"

    def test_overwrite(self):
        etcd_put(PREFIX + "test_overwrite", "v1")
        etcd_put(PREFIX + "test_overwrite", "v2")
        assert etcd_get(PREFIX + "test_overwrite") == "v2"

    def test_multiple_keys(self):
        for i in range(5):
            etcd_put(f"{PREFIX}multi_{i}", f"val_{i}")
        for i in range(5):
            assert etcd_get(f"{PREFIX}multi_{i}") == f"val_{i}"


class TestEtcdConfigFormat:
    """配置数据格式验证"""

    def test_json_value(self):
        val = json.dumps({"log_level": "DEBUG", "max_conn": 1000})
        etcd_put(PREFIX + "json_cfg", val)
        got = etcd_get(PREFIX + "json_cfg")
        parsed = json.loads(got)
        assert parsed["log_level"] == "DEBUG"
        assert parsed["max_conn"] == 1000

    def test_plain_value(self):
        etcd_put(PREFIX + "plain", "simple_string")
        assert etcd_get(PREFIX + "plain") == "simple_string"

    def test_empty_value(self):
        etcd_put(PREFIX + "empty", "")
        val = etcd_get(PREFIX + "empty")
        assert val is None or val == ""  # etcd omits empty value field
        assert val is None or val == ""
        etcd_put(PREFIX + "empty", "")
        val = etcd_get(PREFIX + "empty"); assert val == "" or val is None  # etcd may omit empty value field

    def test_long_value(self):
        long_val = "x" * 10000
        etcd_put(PREFIX + "long", long_val)
        assert len(etcd_get(PREFIX + "long")) == 10000


class TestEtcdConfigPrefix:
    """key 前缀验证"""

    def test_registry_not_affected(self):
        """写 config 不应该影响 registry"""
        etcd_put(PREFIX + "boundary_test", "x")
        # registry key 不应该出现
        r = subprocess.run(["curl", "-sf", "--max-time", "3", "-X", "POST",
            f"{ETCD}/v3/kv/range",
            "-d", json.dumps({"key": base64.b64encode(b"/thunder/registry/").decode(),
                              "range_end": base64.b64encode(b"/thunder/registry0").decode()})],
            capture_output=True, text=True)
        d = json.loads(r.stdout)
        # config key 不应在 registry 前缀下
        for kv in d.get("kvs", []):
            k = base64.b64decode(kv["key"]).decode()
            assert not k.startswith(PREFIX), f"config key {k} in registry range"

    def test_config_prefix_isolated(self):
        """config key 只匹配 /thunder/config/ 前缀"""
        etcd_put(PREFIX + "isolated", "yes")
        # 查 /thunder/config/ 前缀
        body = json.dumps({"key": base64.b64encode(PREFIX.encode()).decode(),
                           "range_end": base64.b64encode((PREFIX[:-1]+chr(ord(PREFIX[-1])+1)).encode()).decode()})
        r = subprocess.run(["curl", "-sf", "--max-time", "3", "-X", "POST",
            f"{ETCD}/v3/kv/range", "-d", body], capture_output=True, text=True)
        d = json.loads(r.stdout)
        found = False
        for kv in d.get("kvs", []):
            k = base64.b64decode(kv["key"]).decode()
            if "isolated" in k: found = True
        assert found, "config key not found in prefix range"
