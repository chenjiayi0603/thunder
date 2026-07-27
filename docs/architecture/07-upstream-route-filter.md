# 路由按需下发 — 前缀过滤

> 源码: `code/Net/src/labor/EtcdCenterConnector.cpp` (OnWatchAsync)

---

## 问题

每个 Worker 收到 etcd 全量路由，但实际只用一部分：

```
Manager(Interface)                  Worker(Interface)
  │                                     │
  │  Watch /thunder/registry/           │
  │  ────────────┐                      │
  │              │ 全量 NodeNotice (5条) │
  │  ◄───────────┘                      │
  │                                     │
  │  SetNodeNotice → shm ──────────────►│ NodesMgr: HELLO×3 + INTERFACE + LOGIC
```

| 节点 | 需要 | 收到 | 冗余 |
|------|------|------|------|
| Interface | LOGIC | LOGIC + HELLO×3 + INTERFACE | 4条 |
| Logic | (无) | 全部 5条 | 5条 |
| Hello | (无) | 全部 5条 | 5条 |

---

## 设计

在配置层声明每个节点的上游类型，构造 NodeNotice 时按声明过滤：

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
      if node.type ∉ upstreamTypes → skip
  → NodeNotice 仅含 LOGIC
       │
       ▼
共享内存 (RouteNoticeVersionData)
       │
       ▼
Worker::ReadRouteMirror()
  NodesMgr 仅含 LOGIC
```

### 配置声明

```json
{
    "node_type": "INTERFACE",
    "upstream_types": ["LOGIC"]
}
```

### 过滤逻辑

```cpp
// EtcdCenterConnector.cpp:OnWatchAsync()
if (!m_upstreamTypes.empty() && m_upstreamTypes.find(ntype) == m_upstreamTypes.end())
    continue;  // 仅当配置了 upstream_types 且非空时才过滤
```

`upstream_types` 缺省/空数组时行为不变（全量下发），无需改配置。
