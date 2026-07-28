#!/usr/bin/env python3
"""Thunder canary weight management CLI.

用法:
  canary.py <service>                        # 查看当前权重
  canary.py <service> canary <ver> <pct>      # 设置灰度权重
  canary.py <service> full <ver>             # 全量切换到指定版本
  canary.py <service> rollback [ver]          # 回滚到上一稳定版本（或指定版本）
  canary.py <service> reset                  # 删除权重键，恢复默认路由

环境变量:
  ETCD_ENDPOINT    etcd 地址 (默认 127.0.0.1:2379, Docker Compose 用 127.0.0.1:12379)

依赖: pip install etcd3
"""
import sys
import json
import os

# Python 3.14 + protobuf 兼容（避免 "Descriptors cannot be created directly"）
os.environ.setdefault("PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION", "python")

ETCD_HOST = os.environ.get("ETCD_ENDPOINT", "127.0.0.1:2379")
KEY_PREFIX = "/thunder/canary"

# -- etcd client -----------------------------------------------------------
try:
    import etcd3
except ImportError:
    print("需要 pip install etcd3", file=sys.stderr)
    sys.exit(1)


def get_client():
    host, _, port_str = ETCD_HOST.partition(":")
    port = int(port_str) if port_str else 2379
    try:
        return etcd3.client(host=host, port=port)
    except Exception as e:
        print(f"连接 etcd 失败 ({ETCD_HOST}): {e}")
        sys.exit(1)


def show(service):
    """查看当前权重配置（带可视化进度条）"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"
    value, _ = client.get(key)
    if value is None:
        print(f"  {service}: 无灰度配置（使用默认一致性哈希路由）")
        return
    weights = json.loads(value.decode())
    total = sum(weights.values())
    print(f"  {service} 灰度权重:")
    for ver, w in sorted(weights.items()):
        pct = w / total * 100 if total > 0 else 0
        bar = "█" * int(pct / 5) + "░" * (20 - int(pct / 5))
        print(f"    {ver}: {w:>4} ({pct:5.1f}%)  {bar}")
    print(f"    total={total}")


def canary(service, new_ver, pct):
    """设置灰度权重: new_ver 占 pct%，旧版本按比例分摊剩余"""
    pct = int(pct)
    if not 0 <= pct <= 100:
        print(f"❌ 权重必须在 0~100 之间，got {pct}")
        sys.exit(1)

    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    # 读现有权重
    old_value, _ = client.get(key)
    if old_value:
        old_weights = json.loads(old_value.decode())
    else:
        old_weights = {"v1": 100}

    new_weights = {}
    remaining = 100 - pct

    # 新版本不在旧权重中时需要补上
    if new_ver not in old_weights:
        new_weights[new_ver] = pct

    old_total = sum(w for v, w in old_weights.items() if v != new_ver)

    for ver, w in old_weights.items():
        if ver == new_ver:
            new_weights[ver] = pct
        elif old_total > 0:
            new_weights[ver] = int(w * remaining / old_total)
        else:
            new_weights[ver] = remaining

    # 补误差到第一个旧版本
    diff = 100 - sum(new_weights.values())
    if diff != 0:
        for ver in new_weights:
            if ver != new_ver:
                new_weights[ver] += diff
                break

    client.put(key, json.dumps(new_weights))
    print(f"✅ {service}: {json.dumps(new_weights)}")
    show(service)


def full(service, ver):
    """全量切换到指定版本，旧版本标 weight=0 方便排查"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    # 保留旧版本，标记 weight=0
    old_value, _ = client.get(key)
    new_weights = {}
    if old_value:
        old_weights = json.loads(old_value.decode())
        for v in old_weights:
            new_weights[v] = 0
    new_weights[ver] = 100

    client.put(key, json.dumps(new_weights))
    print(f"✅ {service} 全量切换 → {ver}")
    show(service)


def rollback(service, to_version=None):
    """回滚到稳定版本。未指定时自动检测上一个非零权重版本"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    if to_version:
        client.put(key, json.dumps({to_version: 100}))
        print(f"✅ {service} 已回滚 → {to_version}=100%")
    else:
        old_value, _ = client.get(key)
        if not old_value:
            print(f"❌ {service}: 无灰度配置，无需回滚")
            return
        old_weights = json.loads(old_value.decode())
        sorted_vers = sorted(old_weights.items(), key=lambda x: x[1], reverse=True)

        # 如果最高权重=100，说明是全量切换 → 回滚到最近的非满权重版本
        if len(sorted_vers) >= 2 and sorted_vers[0][1] == 100:
            for v, w in sorted_vers[1:]:
                if w > 0:
                    stable_ver = v
                    break
            else:
                stable_ver = "v1"
        elif len(sorted_vers) >= 1:
            # 灰度场景: 最高权重 = 稳定版本
            stable_ver = sorted_vers[0][0]
        else:
            stable_ver = "v1"

        client.put(key, json.dumps({stable_ver: 100}))
        print(f"✅ {service} 已回滚 → {stable_ver}=100%")
    show(service)


def reset(service):
    """删除权重键，恢复默认一致性哈希路由"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"
    deleted = client.delete(key)
    if deleted:
        print(f"✅ {service} 灰度配置已清除，恢复一致性哈希路由")
    else:
        print(f"  {service}: 本来就没有灰度配置")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    service = sys.argv[1]
    cmd = sys.argv[2] if len(sys.argv) > 2 else "show"

    if cmd == "show":
        show(service)
    elif cmd == "canary":
        if len(sys.argv) != 5:
            print("用法: canary.py <service> canary <version> <pct>")
            sys.exit(1)
        canary(service, sys.argv[3], int(sys.argv[4]))
    elif cmd == "full":
        if len(sys.argv) != 4:
            print("用法: canary.py <service> full <version>")
            sys.exit(1)
        full(service, sys.argv[3])
    elif cmd == "rollback":
        target = sys.argv[3] if len(sys.argv) > 3 else None
        rollback(service, target)
    elif cmd == "reset":
        reset(service)
    else:
        print(f"未知命令: {cmd}")
        print(__doc__)
        sys.exit(1)
