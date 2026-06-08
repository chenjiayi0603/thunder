# 路由按需下发 — 设计原理

> 2026-06-08 | issue #38

---

## 1. 背景

### 1.1 原有行为

Thunder 节点通过 etcd 注册中心发现彼此。etcd 存储全部在线节点的 `IP:PORT → {node_type, node_id, ...}` 映射。

```
etcd
├─ /thunder/registry/10.42.0.93:27007  → {"node_type":"HELLO",...}
├─ /thunder/registry/10.42.0.94:27444  → {"node_type":"HELLO",...}
├─ /thunder/registry/10.42.0.95:27011  → {"node_type":"HELLO",...}
├─ /thunder/registry/10.42.0.96:27009  → {"node_type":"INTERFACE",...}
└─ /thunder/registry/10.42.0.97:16068  → {"node_type":"LOGIC",...}
```

每个节点 Manager 的 `EtcdCenterConnector` 通过 Watch 监听 `/thunder/registry/` 前缀变更，每次变更构造**全量 NodeNotice**，经共享内存下发给所有 Worker。

```
Manager(Interface)                  Worker(Interface)
  │                                     │
  │  Watch /thunder/registry/           │
  │  ────────────┐                      │
  │              │ 全量 NodeNotice       │
  │  ◄───────────┘  (5条)               │
  │                                     │
  │  SetNodeNotice → shm ──────────────►│ CheckShareMem → AddNodeIdentify
  │                                     │
  │                                     │  NodesMgr:
  │                                     │   HELLO     x3
  │                                     │   INTERFACE x1
  │                                     │   LOGIC     x1
```

### 1.2 问题

每个 Worker 收到**所有节点类型**的路由，但实际只用一部分：

| 节点 | 需要 | 收到 | 冗余 |
|------|------|------|------|
| Interface | LOGIC | LOGIC + HELLO×3 + INTERFACE | 4条 |
| Logic | (无) | 全部 5条 | 5条 |
| Hello | (无) | 全部 5条 | 5条 |

冗余路由的影响：
- 共享内存占用增大
- `NodesMgr` 无意义膨胀
- `SendToNext` 轮询时多遍历无关类型
- 旧 Pod IP 残留扩散到不相关的节点

---

## 2. 设计

### 2.1 核心思路

在配置层声明每个节点"关注"的上游节点类型，`EtcdCenterConnector` 构造 NodeNotice 时按声明过滤，Worker 只收到需要的路由。

```
Manager(Interface)                  Worker(Interface)
  upstream_types: ["LOGIC"]           │
  │                                   │
  │  Watch → m_nodeRegistry           │
  │  OnWatchAsync:                    │
  │    for (kv in m_nodeRegistry)     │
  │      if ntype not in upstream     │
  │        skip  ← 过滤               │
  │                                   │
  │  NodeNotice (1条: LOGIC)          │
  │  SetNodeNotice → shm ────────────►│ AddNodeIdentify(LOGIC, ...)
  │                                   │
  │                                   │  NodesMgr:
  │                                   │   LOGIC  x1  ← 只有需要的
```

### 2.2 数据流

```
配置文件                        EtcdCenterConnector            Worker NodesMgr
────────                        ──────────────────            ──────────────
Interface.json
  "upstream_types": ["LOGIC"]
       │
       ▼
Manager::LoadConf()
  m_setUpstreamTypes = {"LOGIC"}
       │
       ▼
CreateCenterConnector()
  p->SetUpstreamTypes(m_setUpstreamTypes)
       │
       ▼
OnWatchAsync()
  if upstreamTypes not empty:
    for node in m_nodeRegistry:
      if node.type ∉ upstreamTypes → skip  ← 关键过滤点
  → NodeNotice 仅含 LOGIC
       │
       ▼
共享内存 (RouteNoticeVersionData)
       │
       ▼
Worker::ReadRouteMirror()
  AddNodeIdentify(LOGIC, ...)
  → NodesMgr 仅含 LOGIC
```

### 2.3 兼容性

`upstream_types` 缺省/空数组时行为不变（全量下发），所有现有节点无需改配置。

```cpp
// EtcdCenterConnector.cpp:OnWatchAsync()
if (!m_upstreamTypes.empty() && m_upstreamTypes.find(ntype) == m_upstreamTypes.end())
    continue;  // 仅当配置了 upstream_types 且非空时才过滤
```

---

## 3. 实现细节

### 3.1 配置声明

`Interface.json`:

```json
{
    "node_type": "INTERFACE",
    "upstream_types": ["LOGIC"]
}
```

### 3.2 配置解析

`Manager::LoadConf()`:

```cpp
m_setUpstreamTypes.clear();
if (m_oCurrentConf["upstream_types"].IsArray()) {
    int n = m_oCurrentConf["upstream_types"].GetArraySize();
    for (int i = 0; i < n; ++i) {
        std::string t;
        if (m_oCurrentConf["upstream_types"].Get(i, t) && !t.empty())
            m_setUpstreamTypes.insert(t);
    }
}
```

### 3.3 注入连接器

`Manager::CreateCenterConnector()`:

```cpp
auto p = std::make_unique<EtcdCenterConnector>(centerConf);
p->SetLogger(GetLogger());
p->SetUpstreamTypes(m_setUpstreamTypes);  // ← 注入
return p;
```

### 3.4 过滤逻辑

`EtcdCenterConnector::OnWatchAsync()`:

```cpp
// 构造路由快照, 按 upstream_types 过滤
for (const auto& kv : m_nodeRegistry) {
    // ... parse key, value ...
    std::string ntype;
    oVal.Get("node_type", ntype);
    // 上游类型过滤: empty=全量, 否则仅下发关注类型
    if (!m_upstreamTypes.empty() && m_upstreamTypes.find(ntype) == m_upstreamTypes.end())
        continue;
    // ... add to NodeNotice ...
}
```

### 3.5 涉及文件

| 文件 | 改动 | 说明 |
|------|------|------|
| `EtcdCenterConnector.hpp` | +include, +SetUpstreamTypes, +m_upstreamTypes | 存储关注类型集合 |
| `EtcdCenterConnector.cpp` | +2行过滤逻辑 | OnWatchAsync 构造快照时过滤 |
| `ManagerContext.hpp` | +include, +m_setUpstreamTypes | Manager 运行时存储 |
| `Manager.cpp` | +10行 | LoadConf 解析, CreateCenterConnector 注入 |
| `deploy/Interface/conf/Interface.json` | +1行 | Interface 声明上游类型 |
| `k8s/conf/Interface.json` | +1行 | k8s 下同上 |

---

## 4. 验证

### 4.1 k8s 冒烟测试

```
Interface → Logic  5/5 全部通过
  ✅ GenKey (POST)
  ✅ VerifyKey 有效 token → code:0
  ✅ GenKey (GET + JSON body)
  ✅ VerifyKey 非法 token → code:1
  ✅ VerifyKey 空 token → code:1

Hello 段            6/6 全部通过
etcd 注册中心       1/1 全部通过 (5节点在线)
```

### 4.2 路由表验证

Interface Worker 日志 (`grep AddNodeIdentify`):

```
# 旧行为（未配置 upstream_types）:
AddNodeIdentify(HELLO, 10.42.0.93:27007.0)
AddNodeIdentify(HELLO, 10.42.0.94:27444.0)
AddNodeIdentify(HELLO, 10.42.0.95:27011.0)
AddNodeIdentify(INTERFACE, 10.42.0.96:27009.0)
AddNodeIdentify(LOGIC, 10.42.0.99:16068.0)
                ↑ 5条, 4条无意义

# 新行为（"upstream_types": ["LOGIC"]）:
AddNodeIdentify(LOGIC, 10.42.0.99:16068.0)
                ↑ 1条, 精准
```

---

## 5. etcd Key 重构 — Watch 源头过滤

> 已实现 (2026-06-08)

### 5.1 问题

v1 版在 `OnWatchAsync` 构造 NodeNotice 时从 value 提取 `node_type` 做过滤——但 etcd Watch **仍拉取全量 registry 数据**，只在客户端丢弃不需要的类型。

### 5.2 方案

将 `node_type` 提升到 etcd key 路径中，过滤在 key 层面完成。

```
# 旧结构
/thunder/registry/{IP}:{PORT} → {"node_type":"LOGIC","node_id":52,...}

# 新结构
/thunder/registry/{TYPE}/{IP}:{PORT} → {"node_id":52,...}
```

```
/thunder/registry/
├── LOGIC/
│   └── 10.42.0.97:16068 → {"node_id":52,...}
├── HELLO/
│   ├── 10.42.0.93:27007 → {"node_id":44,...}
│   ├── 10.42.0.94:27444 → {"node_id":49,...}
│   └── 10.42.0.95:27011 → {"node_id":40,...}
└── INTERFACE/
    └── 10.42.0.96:27009 → {"node_id":48,...}
```

### 5.3 Watch 策略

| 配置 | Watch 前缀 | 效果 |
|------|-----------|------|
| `upstream_types: ["LOGIC"]` | `/thunder/registry/LOGIC/` | 仅收 LOGIC 事件 |
| `upstream_types: []` (缺省) | `/thunder/registry/` | 全量 (兼容) |

```cpp
// EtcdCenterConnector::StartWatch()
if (m_upstreamTypes.empty()) {
    m_watcher->Watch("/thunder/registry/");     // 全量
} else {
    for (const auto& t : m_upstreamTypes)
        m_watcher->Watch("/thunder/registry/" + t + "/");  // 按类型
}
```

### 5.4 优势对比

| | 应用层过滤 (v1) | etcd Watch 过滤 (v2) |
|---|---|---|
| 网络传输 | 全量拉取 | 仅关注类型 |
| JSON 解析 | 全量解析后丢弃 | 不需要的不会收到 |
| `m_nodeRegistry` | 含全量节点 | 仅含关注类型 |
| `OnWatchAsync` 过滤代码 | 需要 | 不需要 |

### 5.5 涉及改动

| 模块 | 改动 |
|------|------|
| `DoRegister` | key: `/thunder/registry/{IP}:{PORT}` → `/thunder/registry/{TYPE}/{IP}:{PORT}` |
| `DoWatchSnapshot` | range prefix 按 `m_upstreamTypes` 逐个查询 |
| `StartWatch` | watch prefix 同上 |
| `ProcessWatchLine` / `OnWatchAsync` | key 解析适配新层级; 去掉应用层过滤 |
| `SelfAuditRegistry` | 自检 key 更新 |
| `QueryRegistry` | 查询 key 更新 |
| `BuildSlotTxn` | slot txn 中 registry key 更新 |
| etcd 存量数据 | 需清理旧 key 或做迁移 |

### 5.6 实现摘要

**新 key 格式**：`/thunder/registry/{TYPE}/{IP}:{PORT}`

```cpp
// BuildRegistryPrefix / BuildMyRegistryKey (EtcdCenterConnector.hpp)
static std::string BuildRegistryPrefix(const std::string& nodeType) {
    return "/thunder/registry/" + nodeType + "/";
}
std::string BuildMyRegistryKey() const {
    return BuildRegistryPrefix(m_nodeType) + m_nodeIp + ":" + std::to_string(m_nodePort);
}
```

**key 层过滤** (`OnWatchAsync`)：旧格式 `{IP}:{PORT}` 兼容，新格式 `{TYPE}/{IP}:{PORT}` 直接按 type 前缀筛选，无需解析 JSON value。

**向后兼容**：`upstream_types` 缺省时不对 Watch 前缀做任何限制，行为与旧版完全一致。

### 5.7 验证

etcd key (`etcdctl get --prefix /thunder/registry/`)：
```
/thunder/registry/INTERFACE/10.42.0.103:27009    ← 新格式
/thunder/registry/LOGIC/10.42.0.102:16068        ← 新格式
/thunder/registry/10.42.0.93:27007               ← 旧格式 (兼容)
```

Interface Worker 路由表 (`grep AddNodeIdentify`)：
```
AddNodeIdentify(LOGIC, 10.42.0.102:16068.0)      ← 仅 LOGIC, 1条
```

冒烟测试：
- Hello HTTP/HTTPS/WS/Redis/MySQL    **6/6** ✅
- Interface→Logic GenKey/VerifyKey    **5/5** ✅
- etcd 注册中心                       **1/1** ✅
