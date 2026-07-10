# Thunder 加权路由灰度（etcd 权重键 + Worker 进程内分流）

> 设计日期：2026-07-08  |  状态：📋 设计阶段  |  关联：K8s hostNetwork 部署
>
> **本文档是核心路由层设计。** 运维工具链（CRD + Operator 见 `35-k8s-canary-operator.md`，CI/CD 见 `36-k8s-canary-ci.md`）延后实施。

---

## 0. 为什么不用 K8s 原生方案

K8s 原生 **不支持权重流量**。Service 的负载均衡是内核级 iptables/IPVS 均匀分发，没有权重字段：

| 方案 | 原理 | 权重 | 额外组件 | 适用 |
|---|---|---|---|---|
| K8s Service | iptables/IPVS 均匀分发 | ❌ | 无 | 无权重场景 |
| nginx-ingress canary | L7 入口按 header/权重分流 | ✅ | Ingress Controller | 入口流量，不走内网 |
| Istio/Linkerd | sidecar 代理拦截 + 权重转发 | ✅ | 全套 Service Mesh | 运维复杂，性能损耗大 |
| **Thunder + etcd** | Worker 进程内选后端，读 etcd 权重 | ✅ | **零** | 内网 RPC + 网关，~75 行代码 |

Thunder 的优势：etcd 本来就在用做服务发现，路由层本来就有 `GetNodeIdentify`，加权重就是一个 if/else。不需要 sidecar、不需要改网络层、不需要额外组件。

> **注意**：灰度的是 Logic（被调方），但路由选择发生在 Interface/HelloHttp 等**网关（主调方）**。一个 Logic 可能被多个网关调用——etcd 权重键是中心化的，所有上游网关 Worker 同时读同一份权重，不需要各自配置。

---

## 1. 目标

**仅通过 etcd 一个权重键 + ~75 行 C++**，实现：

- **权重分流**：按百分比将流量切到新版本实例
- **秒级回滚**：改 etcd 权重即可，不杀 Pod
- **不中断**：旧实例继续服务剩余流量，新实例逐步接管

### 数据流全景

```
┌─ 运维操作 ──────────────────────────────────────────────────┐
│                                                              │
│  etcdctl put /thunder/canary/LOGIC/weights '{"v1":90,"v2":10}'│
│                                                              │
└──────────────────────┬───────────────────────────────────────┘
                       │ etcd key 变更
┌─ EtcdCenterConnector (已有，需扩展 watch 前缀) ──────────────┐
│ Watch /thunder/canary/ 前缀                                   │
│   → OnWatchEvent 解析权重                                     │
│   → m_canaryWeights[LOGIC] = {"v1":90,"v2":10}               │
│   → AssembleAndPushRouteUpdated() 拼成 NodeNotice            │
│   → SetNodeNotice() 写共享内存                                │
└──────────────────────┬───────────────────────────────────────┘
                       │ 共享内存 (已有 RouteNoticeVersionData)
┌─ Manager (已有) ─────────────────────────────────────────────┐
│ OnCenterEvent(RouteUpdated)                                  │
│   → 读 NodeNotice 中的 canary_weights                        │
│   → 展开 version 权重 → ip:port 权重                          │
│     (v1→2节点各45, v2→1节点10)                                │
│   → SetNodeNotice() 共享内存 mirror                          │
│   → version++                                                │
└──────────────────────┬───────────────────────────────────────┘
                       │ fork 时共享内存指针
┌─ Worker (已有) ──────────────────────────────────────────────┐
│ 事件循环每轮:                                                 │
│   GetRouteNoticeVersionData().GetNodeNoticeVersion() ≠ 本地?  │
│     → 版本变了，读最新 NodeNotice                             │
│       → 解析出 ip:port 权重表                                 │
│                                                              │
│ 请求需路由到 Logic:                                           │
│   Nodes::GetNodeIdentify("LOGIC", hashKey) ← 只改这一处       │
│     ├─ 权重表存在?                                           │
│     │   ├─ 是 → 加权随机                                     │
│     │   │       192.168.3.61:16068 → 45                      │
│     │   │       192.168.3.61:16069 → 45                      │
│     │   │       192.168.3.61:16071 → 10                      │
│     │   └─ 否 → 现有一致性哈希 (不变)                         │
│     └─ 返回选中 node 的 ip:port                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. etcd 键设计

### 权重键（新增）

```
/thunder/canary/{NODE_TYPE}/weights
  → {"v1":90,"v2":10}
```

极简。version tag 跟节点注册时的 `node_version` 字段对应。

### 节点注册键（扩展现有字段）

```
/thunder/nodes/LOGIC/192.168.3.61:16068
  → {"node_id":80,"node_type":"LOGIC","node_version":"v1",...}
                                                 ↑ 新增字段
```

节点启动时读 `NODE_VERSION` 环境变量，写入注册信息。未设置时默认 `"v1"`。

### 权重展开（Manager 侧）

Manager 的 `AssembleAndPushRouteUpdated` 读节点列表 + 权重键，展开为 ip:port 权重表：

```
输入:
  节点: 192.168.3.61:16068 (v1), 192.168.3.61:16069 (v1), 192.168.3.61:16071 (v2)
  权重: {"v1":90,"v2":10}

输出 (推到共享内存):
  192.168.3.61:16068 → 45   (90/2)
  192.168.3.61:16069 → 45   (90/2)
  192.168.3.61:16071 → 10   (10/1)
```

Worker 拿到的就是一张扁平表（ip:port → 权重），不需要知道 "v1"/"v2"。

> **边界处理**：如果某个节点的 `node_version` 不在权重 map 中（例如 `full v2` 后权重键只有 `{"v2":100}`，v1 节点还在注册表里），Manager 将该节点权重设为 0——节点保持注册但不接收灰度流量。权重键被删除后，所有节点恢复一致性哈希路由。

### 为什么按 version 不按 node_id

| | 按 node_id | 按 version |
|---|---|---|
| v2 扩容 2→10 副本 | 权重键要加 8 个 node_id | **不用改**，新节点自动带 version=v2 |
| 权重键内容 | `{"80":45,"81":45,"82":10}` | `{"v1":90,"v2":10}` |
| 新增 v3 灰度 | 手动维护元数据 | 新部署自带 version=v3，自动入组 |
| 回滚 | 需要知道哪些 node_id 属于 v2 | `{"v1":100,"v2":0}` 一刀切 |

---

## 3. 代码改动清单

**全部在 Thunder C++ 仓库内，0 新依赖。**

| 模块 | 改动 | 行数 |
|---|---|---|
| 节点注册 | 读 `NODE_VERSION` 环境变量，注册时写入 etcd | +5 |
| etcd Watch | 新增 `/thunder/canary/` 前缀 watch，复用现有 Watch 框架 | +30 |
| protobuf | `NodeNotice` 加 `canary_weights` map + `node_version` 字段 | +5 |
| EtcdCenterConnector | `OnWatchEvent` 解析权重，`AssembleAndPushRouteUpdated` 按 version 展开权重 | +20 |
| `Nodes::GetNodeIdentify` | 加权随机分支 | +15 |
| **合计** | | **~75** |

### 路由选择伪代码

```cpp
// code/Net/src/dispatcher/Nodes.cpp — 唯一需要改逻辑的地方
const std::string& Nodes::GetNodeIdentify(
    const std::string& strNodeType,
    const std::string& strHashKey)
{
    // 读共享内存中的权重表（Manager 已展开为 ip:port → weight）
    auto& weights = GetCanaryWeights(strNodeType);

    if (weights.empty()) {
        // 没有灰度配置 → 走现有一致性哈希
        return GetNodeIdentifyByHash(strNodeType, strHashKey);
    }

    // 有灰度配置 → 加权随机
    int totalWeight = 0;
    for (auto& [addr, w] : weights) totalWeight += w;

    int randVal = GetWorkerLocalRandom() % totalWeight;
    int acc = 0;
    for (auto& [addr, w] : weights) {
        acc += w;
        if (randVal < acc) return addr;
    }
    return weights.begin()->first;  // fallback
}
```

### 不需要改的地方

| 模块 | 原因 |
|---|---|
| etcd Watch | 已有 `RouteUpdated` 事件，Manager 自动 watch |
| 共享内存 mirror | 已有 `RouteNoticeVersionData`，push 到 Worker 零拷贝 |
| 热重载 | 已有 `GracefulRestartWorker`，权重变更不需要重启 |
| 服务注册 | 已有 `DoRegister`，新节点自动注册 |
| 连接管理 | 现有连接池、重连逻辑不受影响 |

---

## 4. 操作方式

操作分两层，可以混用（它们写的都是**同一个 etcd 权重键**）：

| 层级 | 方式 | 一句话 | 适合 |
|---|---|---|---|
| **推荐** | `./tools/canary.py LOGIC canary v2 10` | Python 封装，防手误 | 日常操作 |
| **兜底** | `etcdctl put .../weights '{...}'` | 直写 etcd，零依赖 | 调试/极端情况 |

> 未来会有第三层——`kubectl apply GrayRelease` CRD（声明式，Operator 自动建 Deployment + 调权重），见附录 B。完整 CI 自动化见 `36-k8s-canary-ci.md`。

### 4.0 前提：新版本 Pod 必须带 `NODE_VERSION` 环境变量

调权重之前，新版本 Pod 必须已在集群里跑着。路由层只关心一件事：**Pod 启动时读了 `NODE_VERSION` env 并注册到 etcd**。至于镜像怎么构建、Deployment 怎么 apply、CI 怎么自动化，全部见 `36-k8s-canary-ci.md`。

两句话概括 34 和 36 的分工：

| 文档 | 管什么 | 一句话 |
|---|---|---|
| **34（本文档）** | etcd 权重键的读写 | `etcdctl put .../weights '{"v1":90,"v2":10}'` |
| **36** | 从源码到 Pod 跑起来的全流程 | `cmake → docker build → kubectl apply → 灰度 Pipeline` |

> **不需要改 K8s Service**。新 Pod 注册到 etcd 后 Thunder Manager 自动发现。旧 Pod 继续服务所有流量，新 Pod 待命——因为 etcd 里还没有权重键，流量 100% 走一致性哈希 → v1。

### 4.1 调权重：etcdctl 原始命令（始终可用）

```bash
# 开始灰度：10% 流量 → v2
etcdctl put /thunder/canary/LOGIC/weights '{"v1":90,"v2":10}'

# 逐步放量
etcdctl put /thunder/canary/LOGIC/weights '{"v1":50,"v2":50}'

# 全量切换
etcdctl put /thunder/canary/LOGIC/weights '{"v1":0,"v2":100}'

# 秒级回滚
etcdctl put /thunder/canary/LOGIC/weights '{"v1":100,"v2":0}'

# 查看当前权重
etcdctl get /thunder/canary/LOGIC/weights

# 删除权重键（恢复一致性哈希默认路由）
etcdctl del /thunder/canary/LOGIC/weights
```

修改后 Manager Watch 即时感知 → 共享内存 version++ → Worker 下一笔请求即用新权重。不用 reload、不用重启、不用进 Pod。

#### 验证流量分流

```bash
# 确认权重键已生效
etcdctl get /thunder/canary/LOGIC/weights

# 查 Worker 日志确认路由命中（如有打点）
kubectl logs -n thunder -l app=interface --tail=20 | grep "canary"

# 或用 canary_test_harness 发探测请求统计实际分布
./ci/canary_test_harness --service LOGIC --requests 200
# expect: v1=180, v2=20  (weight=10 时)
```

#### 灰度完成后：旧版本下线

```bash
# 全量切换后，等待存量连接排空，再缩容旧 Deployment
etcdctl put /thunder/canary/LOGIC/weights '{"v1":0,"v2":100}'
sleep 30    # 等待存量短连接自然结束

# 缩容旧版本（保留 Deployment 不删，方便快速回滚）
kubectl scale deploy logic-v1 -n thunder --replicas=0

# 确认旧 Pod 已全部终止
kubectl get pods -n thunder -l app=logic,version=v1

# 稳定运行 N 天后，确认不再需要回滚，再删除旧 Deployment
# kubectl delete deploy logic-v1 -n thunder
```

### 4.2 Python CLI（防误操作封装）

etcdctl 拼 JSON 字符串容易出错（引号转义、权重和不等于 100 等），提供一个轻量 Python 客户端封装：

```bash
# 安装（零额外依赖，仅需 etcd3 SDK）
pip install etcd3

# 或直接使用项目内脚本
export ETCD_ENDPOINT=127.0.0.1:2379
./tools/canary.py LOGIC                  # 查看当前权重
./tools/canary.py LOGIC canary v2 10     # v2 占 10%，v1 自动算 90%
./tools/canary.py LOGIC canary v2 50     # v2 占 50%
./tools/canary.py LOGIC full v2          # v2 全量 100%
./tools/canary.py LOGIC rollback         # 回滚：100% → 上一稳定版本（自动检测）
./tools/canary.py LOGIC rollback v2      # 回滚到指定版本
./tools/canary.py LOGIC reset            # 删权重键，恢复一致性哈希
```

#### 脚本实现

```python
#!/usr/bin/env python3
# tools/canary.py — Thunder 灰度权重管理 CLI
"""
用法:
  canary.py <service>                        # 查看当前权重
  canary.py <service> canary <ver> <pct>      # 设置灰度权重
  canary.py <service> full <ver>             # 全量切换到指定版本
  canary.py <service> rollback [ver]          # 回滚到上一稳定版本（或指定版本）
  canary.py <service> reset                  # 删除权重键，恢复默认路由
"""
import sys, json, os
import etcd3

ETCD_HOST = os.environ.get("ETCD_ENDPOINT", "127.0.0.1")
ETCD_PORT = 2379
if ":" in ETCD_HOST:
    ETCD_HOST, ETCD_PORT = ETCD_HOST.rsplit(":", 1)
    ETCD_PORT = int(ETCD_PORT)

KEY_PREFIX = "/thunder/canary"

def get_client():
    return etcd3.client(host=ETCD_HOST, port=ETCD_PORT)

def show(service):
    """查看当前权重配置"""
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
    """设置灰度权重: new_ver 占 pct%，v1 占剩余"""
    pct = int(pct)
    if not 0 <= pct <= 100:
        print(f"❌ 权重必须在 0~100 之间，got {pct}")
        sys.exit(1)

    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    # 如果 etcd 中已有其他 version，保留它们并按比例缩放
    old_value, _ = client.get(key)
    if old_value:
        old_weights = json.loads(old_value.decode())
    else:
        old_weights = {"v1": 100}

    new_weights = {}
    remaining = 100 - pct
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
    """全量切换到指定版本，旧版本标记 weight=0 方便排查"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    # 保留旧版本写入 weight=0，方便排查时看到完整的版本列表
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
    """回滚: 100% → 稳定版本（默认自动检测上一个非零权重版本）"""
    client = get_client()
    key = f"{KEY_PREFIX}/{service}/weights"

    if to_version:
        # 指定回滚目标
        client.put(key, json.dumps({to_version: 100}))
        print(f"✅ {service} 已回滚 → {to_version}=100%")
    else:
        # 自动检测：找权重最大的非最新版本作为回滚目标
        old_value, _ = client.get(key)
        if not old_value:
            print(f"❌ {service}: 无灰度配置，无需回滚")
            return
        old_weights = json.loads(old_value.decode())
        # 排除权重最大的那个 version（通常是正在灰度的新版本），回滚到次大的
        sorted_vers = sorted(old_weights.items(), key=lambda x: x[1], reverse=True)
        if len(sorted_vers) >= 2 and sorted_vers[0][1] > 0:
            stable_ver = sorted_vers[1][0]
        elif len(sorted_vers) >= 1:
            stable_ver = "v1"  # fallback
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
```

#### 对比 etcdctl 原生命令

| 操作 | etcdctl 原生命令 | Python CLI |
|---|---|---|
| 灰度 10% | `etcdctl put .../weights '{"v1":90,"v2":10}'` | `./canary.py LOGIC canary v2 10` |
| 全量 | `etcdctl put .../weights '{"v1":0,"v2":100}'` | `./canary.py LOGIC full v2` |
| 回滚 | `etcdctl put .../weights '{"v1":100,"v2":0}'` | `./canary.py LOGIC rollback` |
| 查看 | `etcdctl get .../weights` | `./canary.py LOGIC`（带可视化进度条） |
| 多版本灰度 | 手算 JSON | 自动按比例分摊 v1/v3 权重 |
| 权限校验 | 无 | pct 范围检查 0~100 |

> Python CLI 不替代 etcdctl，只是防手误的封装层。CI pipeline 和 Operator 后续都会直接操作 etcd 权重键，与 Python CLI 完全兼容——它们写的是同一个 key。

### 4.3 灰度流程图

```
v2 Deployment 已部署 (env NODE_VERSION=v2)
         │
         ▼
① etcdctl put weight=10
         │
         ▼ (～100ms)
② Manager Watch → 共享内存权重表更新
         │
         ▼ (下一笔请求)
③ Worker: 10% 请求 → v2 节点
         │
         ├─ 观察监控 OK → ④ etcdctl put weight=50 → ... → weight=100
         │
         └─ 出问题 → etcdctl put weight=0 → v2 断流，Pod 保留不杀
```

---

## 5. 实现计划

| 阶段 | 内容 | 预估 |
|------|------|:--:|
| **P0：节点权重注册** | 读 `NODE_VERSION` env → 注册时写入 etcd；Manager 按 version 展开权重表 | 1d |
| **P1：加权路由** | `Nodes::GetNodeIdentify` 加权随机模式；canary watch + protobuf + Connector 改动 | 2d |
| **P2：集成测试** | kubeadm 集群全流程验证（手动 `etcdctl put` 回滚） | 1d |
| **P3：文档** | 灰度发布运维手册 + 示例 | 0.5d |

**总计 ~4.5d，纯 C++，0 新依赖。**

---

## 6. 后续（按优先级）

核心路由（本文档）上线后即可手工灰度。以下能力按需推进，无技术阻塞（除自动回滚依赖 Prometheus）：

| 项 | 依赖本文档？ | 阻塞点 | 建议 |
|---|---|---|---|
| **CI/CD pipeline** | 否，独立 | 人力（~0.5d shell 脚本） | 可立即做，见 `36-k8s-canary-ci.md` |
| **CRD + Go Operator** | 否，etcd 键已定 | 人力（~3d kubebuilder） | 可立即做，见 `35-k8s-canary-operator.md` |
| **Admin Web 面板** | 否，调 etcd 即可 | 人力（~2d） | 按需做 |
| **自动回滚** | 是，权重键格式 | Prometheus 业务指标未就绪 | Prometheus 就绪后可做 |

> 一句话：**etcd 权重键是唯一的接口约定**。本文档把这个键定下来之后，CI、Operator、Admin UI 可以各自独立开发——它们都在操作同一个 key，不需要互相等。

---

## 附录 B：GrayRelease CRD 方案（暂不实施，留档参考）

> ⚠️ 注意：下面展示的是**用户最终看到的效果**，背后需要一整套 Operator 基础设施，不是一个 YAML 能搞定的。

和 Istio 生态的 Flagger 同模式：**CRD 定义期望状态 → Operator watch 并执行 → 调 etcd 权重键**。区别只在于 Flagger 调的是 Istio VirtualService，Thunder 调的是 etcd。

### 需要的 YAML 文件（共 4 个）

用户看到的只是一个 `kubectl apply graylease.yaml`，背后要部署以下全部：

#### 各 YAML 的作用和关系

```
① crd.yaml          告诉 K8s "GrayRelease 这个资源类型长什么样"
        │           注册后 kubectl 才认识 kind: GrayRelease
        │
② rbac.yaml         给 Operator 的 ServiceAccount 授权：
        │             - watch GrayRelease CRD（读用户提交的灰度任务）
        │             - 创建/删除 Deployment（自动建新版本 Pod）
        │             - etcd 权限不在 K8s RBAC 里，走 etcd 自己的认证
        │
③ operator.yaml     部署 Operator Pod，绑定上面的 ServiceAccount
        │            Pod 里的 Go 二进制启动后：
        │              → watch GrayRelease CRD（用户改了 weight？）
        │              → 调 K8s API 建 Deployment（新版本 Pod 不够？）
        │              → 调 etcd API 写权重键（通知 Thunder Worker 分流）
        │
④ graylease.yaml    用户唯一需要写的文件：声明 "我要灰度 LOGIC 到 v2"
                     Operator 读到它之后，自动执行 ②③ 授权范围内的一切操作
```

---

#### ① CRD 定义（`crd.yaml`，注册 GrayRelease 资源类型）

```yaml
apiVersion: apiextensions.k8s.io/v1
kind: CustomResourceDefinition
metadata:
  name: grayreleases.thunder.io
spec:
  group: thunder.io
  names:
    kind: GrayRelease
    singular: grayrelease
    plural: grayreleases
    shortNames: [gr]
  scope: Namespaced
  versions:
    - name: v1
      served: true
      storage: true
      schema:
        openAPIV3Schema:
          type: object
          required: [spec]
          properties:
            spec:
              type: object
              required: [service, newVersion, weight]
              properties:
                service:
                  type: string
                  description: 目标服务类型 (LOGIC/HELLO_HTTP/INTERFACE)
                newVersion:
                  type: string
                  description: 新版本标识 (v2)
                image:
                  type: string
                  description: 新版本镜像地址
                weight:
                  type: integer
                  minimum: 0
                  maximum: 100
                  description: 流向新版本的流量百分比
                steps:
                  type: array
                  items: { type: integer }
                  description: 灰度阶梯 [5,20,50,100]
                rollbackOnErrorRate:
                  type: number
                  description: 错误率阈值，超过自动回滚
            status:
              type: object
              properties:
                phase:
                  type: string
                  enum: [Pending, Progressing, Running, Completed, RollingBack, RolledBack, Failed]
                currentWeight: { type: integer }
                message:       { type: string }
```

#### ② RBAC 权限（`rbac.yaml`，Operator 操作 K8s 和 etcd 的凭证）

```yaml
---
apiVersion: v1
kind: ServiceAccount
metadata:
  name: thunder-operator
  namespace: thunder
---
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: thunder-operator
rules:
  # watch + 管理自己的 CRD
  - apiGroups: ["thunder.io"]
    resources: ["grayreleases"]
    verbs: ["get", "list", "watch", "update", "patch"]
  - apiGroups: ["thunder.io"]
    resources: ["grayreleases/status"]
    verbs: ["update", "patch"]
  # 创建/删除/缩容 Deployment
  - apiGroups: ["apps"]
    resources: ["deployments"]
    verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
  # 读 Pod 状态（等待 Ready）
  - apiGroups: [""]
    resources: ["pods"]
    verbs: ["get", "list", "watch"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRoleBinding
metadata:
  name: thunder-operator
roleRef:
  apiGroup: rbac.authorization.k8s.io
  kind: ClusterRole
  name: thunder-operator
subjects:
  - kind: ServiceAccount
    name: thunder-operator
    namespace: thunder
```

> etcd 的读写权限通过 etcd client 连接认证（不在 K8s RBAC 范围内），通常用 etcd 的证书或 username/password 认证。

#### ③ Operator Deployment（`operator.yaml`）

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: thunder-operator
  namespace: thunder
spec:
  replicas: 1
  selector:
    matchLabels:
      app: thunder-operator
  template:
    metadata:
      labels:
        app: thunder-operator
    spec:
      serviceAccountName: thunder-operator   # 绑定上面的 RBAC
      containers:
        - name: operator
          image: registry.thunder.io/thunder-operator:latest
          env:
            - name: ETCD_ENDPOINT
              value: "etcd.thunder:2379"
```

**镜像里的核心逻辑**（Go，基于 controller-runtime 框架）：

```go
// 这是 operator 镜像里编译进去的 Go 二进制核心代码骨架
// 框架负责: watch CRD + 入队 + 串行化 + 重试 + 选主
// 你只需实现: Reconcile 里 "当前状态 vs 期望状态 → 调 API 对齐"

func (r *GrayReleaseReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
    // 1. 读用户提交的 GrayRelease CRD（即 ④ graylease.yaml）
    var gr grayleasev1.GrayRelease
    if err := r.Get(ctx, req.NamespacedName, &gr); err != nil {
        return ctrl.Result{}, client.IgnoreNotFound(err)
    }

    service   := gr.Spec.Service    // "LOGIC"
    newVer    := gr.Spec.NewVersion // "v2"
    weight    := gr.Spec.Weight     // 10
    image     := gr.Spec.Image      // "registry.../thunder-logic:v2"

    // 2. 确保新版本 Deployment 存在
    dep := &appsv1.Deployment{ObjectMeta: metav1.ObjectMeta{
        Name: service + "-" + newVer, Namespace: gr.Namespace,
    }}
    _, err := controllerutil.CreateOrUpdate(ctx, r.Client, dep, func() error {
        dep.Spec = appsv1.DeploymentSpec{
            Replicas: ptr.To(int32(2)),
            Selector: &metav1.LabelSelector{MatchLabels: map[string]string{
                "app": service, "version": newVer,
            }},
            Template: corev1.PodTemplateSpec{
                ObjectMeta: metav1.ObjectMeta{Labels: map[string]string{
                    "app": service, "version": newVer,
                }},
                Spec: corev1.PodSpec{
                    HostNetwork: true,
                    Containers: []corev1.Container{{
                        Name:  service,
                        Image: image,
                        Env: []corev1.EnvVar{{
                            Name: "NODE_VERSION", Value: newVer,  // ← 关键
                        }},
                    }},
                },
            },
        }
        return nil
    })
    if err != nil { return ctrl.Result{}, err }

    // 3. 写 etcd 权重键（Thunder Manager Watch 到后自动推给 Worker）
    oldWeight := 100 - weight
    etcdClient.Put(ctx, "/thunder/canary/"+service+"/weights",
        fmt.Sprintf(`{"v1":%d,"%s":%d}`, oldWeight, newVer, weight))

    // 4. 灰度达到 100% → 缩容旧版本
    if weight == 100 {
        oldDep := &appsv1.Deployment{ObjectMeta: metav1.ObjectMeta{
            Name: service + "-v1", Namespace: gr.Namespace,
        }}
        r.Get(ctx, client.ObjectKeyFromObject(oldDep), oldDep)
        oldDep.Spec.Replicas = ptr.To(int32(0))
        r.Update(ctx, oldDep)
    }

    // 5. 更新 CRD status（用户 kubectl get gr 能看到进度）
    gr.Status.Phase = "Running"
    gr.Status.CurrentWeight = weight
    r.Status().Update(ctx, &gr)

    return ctrl.Result{}, nil
}
```

> 核心就三步：**读 CRD → 调 K8s API 管 Deployment → 调 etcd API 写权重**。`CreateOrUpdate` 保证幂等——重复调用不会创建重复资源。controller-runtime 框架负责 watch、入队、串行化，你只写业务逻辑。

---

#### ④ 用户使用的 GrayRelease 实例（`grayrelease.yaml`）

```yaml
apiVersion: thunder.io/v1
kind: GrayRelease
metadata:
  name: LOGIC-v2
  namespace: thunder
spec:
  service: LOGIC
  newVersion: v2
  image: registry.thunder.io/thunder-logic:v2
  weight: 10
  steps: [5, 20, 50, 100]
  rollbackOnErrorRate: 0.01
```

#### 部署顺序

```bash
kubectl apply -f crd.yaml            # ① 先注册资源类型
kubectl apply -f rbac.yaml           # ② 创建权限
kubectl apply -f operator.yaml       # ③ 部署 Operator
kubectl wait --for=condition=Ready pod -l app=thunder-operator -n thunder

kubectl apply -f graylease.yaml      # ④ 提交灰度任务（日常只操作这个）
kubectl patch gr LOGIC-v2 -p '{"spec":{"weight":50}}'
kubectl patch gr LOGIC-v2 -p '{"spec":{"weight":0}}'   # 回滚
```

| 组件 | 工作量 | 依赖 |
|---|---|---|
| CRD 定义 + RBAC + Operator Deployment（YAML） | 0.5d | — |
| Operator Go 二进制（kubebuilder，Reconcile 逻辑） | ~3d | etcd 权重键已定 |
| Prometheus + 指标采集 | 已有或需新建 | — |
| 自动回滚（Operator 查 Prometheus → 调权重） | ~1d | Prometheus 就绪 |

> **总结**：用户层面是 `kubectl apply graylease.yaml` + `kubectl patch weight` 两行命令，但背后是 CRD + RBAC + Operator Deployment + Go 二进制 +（可选的）Prometheus 回滚——5 个 YAML + 一个 Go 项目。

---

### Python CLI vs GrayRelease CRD 对比

| 维度 | Python CLI（§4.2） | GrayRelease CRD |
|---|---|---|
| **你能干什么** | 调权重、防手误 | 一样能做到 |
| **部署成本** | 零（`pip install etcd3`） | 4 个 YAML + Go 项目 + 镜像 |
| **团队 1-2 人** | ✅ 完全够用 | 过度设计 |
| **灰度频率低（周级）** | ✅ 完全够用 | 过度设计 |
| **灰度频率高（日级）** | 够用但手酸 | ✅ 值回票价 |
| **审计追溯** | ❌ 谁改了权重不知道 | ✅ `kubectl get gr -o yaml` + Git 记录 |
| **自动回滚** | ❌ 需人工观察指标 | ✅ Operator 查 Prometheus 自动 weight=0 |
| **学习成本** | `./canary.py LOGIC canary v2 10` | 需理解 CRD + Operator 模式 |

**结论**：当前阶段 Python CLI 足够，GrayRelease CRD 是正确但不急的事。etcd 权重键是所有操作的统一接口——先用 Python CLI 写它，以后任何时候切到 CRD，Operator 写的也是同一个 key，零迁移成本。
