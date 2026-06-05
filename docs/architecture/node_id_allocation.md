# node_id 分配设计

> 日期: 2026-06-05
> 涉及: `EtcdCenterConnector`, `BuildSlotTxn`, `AsyncTryClaimSlot`

---

## 1. 设计目标

每个 Thunder 节点需要一个 **唯一 node_id (1-255)** 用于:
- 路由标识(Worker 根据 node_id 建立 connection identity)
- 槽位索引(slot/N 指向该节点的 ip:port)
- 重启后尽量复用原 node_id(lease 未过期)

---

## 2. 分配流程

```
ReportNodeStatus()                                          EtcdCenterConnector.cpp:220
  └→ DoRegister(ip, port, type)                             EtcdCenterConnector.cpp:425
       │
       ├─ L1 确保 lease                                      EtcdCenterConnector.cpp:443
       │    └→ AsyncLeaseGrant → m_leaseId                  EtcdCenterConnector.cpp:261
       │
       ├─ L2 查 registry                                     EtcdCenterConnector.cpp:454
       │    └→ AsyncQueryRegistry                            EtcdCenterConnector.cpp:306
       │         │
       │         ├─ Fresh (键存在 + lease 匹配)              EtcdCenterConnector.cpp:464
       │         │    └→ m_nodeId=原值, OnRegDone(true)     EtcdCenterConnector.cpp:514
       │         │
       │         ├─ Rebind (键存在 + lease 不匹配)           EtcdCenterConnector.cpp:476
       │         │    └→ AsyncRebindRegistration             EtcdCenterConnector.cpp:533
       │         │         └→ PUT slot+registry → OnRegDone
       │         │
       │         └─ Claim (键不存在) → L3                    EtcdCenterConnector.cpp:488
       │
       └─ L3 槽位扫描                                        EtcdCenterConnector.cpp:488
            └→ AsyncTryClaimSlot(slot=N)                    EtcdCenterConnector.cpp:405
                 └→ BuildSlotTxn                            EtcdCenterConnector.cpp:351
                      │
                      ├─ OK    → m_nodeId=N, OnRegDone
                      └─ 占用  → N++, OnRegScan 递归
```

**关键成员**: `m_regInProgress`(`EtcdCenterConnector.hpp:260`), `m_regSlot`(`:261`), `m_regStuckTicks`(`:262`)

## 3. 槽位抢占(txn)原理

```
AsyncTryClaimSlot(N) → BuildSlotTxn(N, slotKey, registryKey, ipPort)
→ POST /v3/kv/txn {
    compare:   slot/N 的 create_revision == 0 (key 不存在)
    success:   PUT slot/N=ip:port  +  PUT registry/ip:port=JSON{node_id:N,...}
               (both with lease=m_leaseId)
    failure:   range slot/N (协议格式,结果不用)
  }

原子性: etcd txn 保证 compare AND success 原子——两个节点不会抢到同一 slot。
```

## 4. 关键边界

| 边界 | 处理 | 代码 |
|------|------|------|
| **并发抢同一 slot** | etcd txn atomic | `EtcdCenterConnector.cpp:405` |
| **重启 lease 未过期** | Fresh 路径直接复用 | `EtcdCenterConnector.cpp:464` |
| **重启 lease 已过期** | Claim 路径重新抢占 | `EtcdCenterConnector.cpp:488` |
| **255 槽位全满** | OnRegDone(false) | `EtcdCenterConnector.cpp:514` |
| **m_regInProgress 死锁** | 30s 超时复位(#27) | `EtcdCenterConnector.cpp:425` |
| **dangling ref crash** | [&]→按值捕获(#19) | `EtcdCenterConnector.cpp:545` |
| **绑定端口竞态** | SO_REUSEADDR(#30) | `Manager.cpp:1199` |
| **管道数据错位** | SkipBytes 重试(#33) | `Manager.cpp:2325` |

## 5. 分配算法细节

### 5.1 起始槽位计算

```cpp
uint32_t hashVal = 0;
for (unsigned char c : ipPort) hashVal += c;
const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]
```
`EtcdCenterConnector.cpp:488` — 简单字节累加哈希,打散不同 ip:port 的起始位。

### 5.2 扫描策略

```cpp
for (uint32_t loop = 0; loop < kMaxSlot; ++loop) {
    const int slotIdx = ((startSlot - 1 + loop) % kMaxSlot) + 1;
    if TryClaimSlot(slotIdx) → found → return slotIdx;   // else → next
}
return "所有槽位已满"
```
`EtcdCenterConnector.cpp:488-510` — 保证每个槽位只尝试一次,255次内必然找到空槽。

### 5.3 Lease 绑定

每个 PUT 操作绑定 `m_leaseId`(异步申请,`kLeaseTTL=10s`)。续租间隔 `kKeepAliveInterval=3s`,失败累计超过阈值触发 reconnect。
参见 `EtcdCenterConnector.cpp:261`(AsyncLeaseGrant), `EtcdCenterConnector.cpp:278`(AsyncKeepAlive)。

## 6. 注册表数据结构

```
/thunder/slot/{1..255}    → value = "ip:port"              (slot 索引)
/thunder/registry/ip:port  → value = {"node_id":N,           (节点信息)
                                        "node_type":"LOGIC",
                                        "node_ip":"127.0.0.1",
                                        "node_port":16068,
                                        "worker_num":1}
```

**一致性约束**: slot/N 和 registry/ip:port 的 lease 必须相同(同一个 txn 写入); slot/N 的 value = registry 的 key 后缀(ip:port); registry 的 node_id = N。

---

## 7. 验证清单

| 测试项 | 检查点 | 脚本 |
|--------|--------|------|
| 注册键完整性 | count≥3, lease≠0 | smoke test |
| node_id 唯一 | `len(nids) == len(set(nids))` | smoke test + `smoke_etcd_parse.py` |
| node_id 范围 | 1 ≤ nid ≤ 255 | smoke test |
| slot↔registry 一致 | slot_ips == reg_ips | smoke test |
| 重启不换号 | lease 未过期 → node_id 不变 | E2E: kill→restart→check |
| 过期回收 | lease 过期 → 新节点可抢占 | E2E: kill→wait TTL→start new node |
| 并发不碰撞 | 多节点同时注册 → nid 无重复 | E2E: multi-container up |
| 255 全满 | OnRegDone 报 "所有槽位已满" | 单元测试模拟 |

---

## 8. 实测分配 (2026-06-05 冒烟集群)

```
节点        ip:port              hash起始   实际nid  扫描步数
LOGIC       127.0.0.1:16068      247        247        1
INTERFACE   127.0.0.1:27009      244        244        1
HELLO       127.0.0.1:27011      237        237        1
```

空集群首次分配,hash 起始位恰好命中空闲槽,无冲突扫描。

---

## 9. 分配链路代码分析

```
Init()
  └→ AsyncLeaseGrant("/v3/lease/grant")                     # L1: 申请租约
      └→ callback: m_leaseId = id → DoRegister(ip,port,type)

DoRegister()
  └→ OnRegEnsureLease()                                     # 确保有 lease(无则补领)
      └→ OnRegQuery()                                       # L2: 查 registry/ip:port
          ├─ Fresh: 键存在 + lease 匹配 → AsyncKeepAlive → OnRegDone
          ├─ Rebind: 键存在 + lease 不匹配 → AsyncRebindRegistration(PUT slot+registry) → OnRegDone
          └─ Claim: 键不存在 → OnRegScan()
                └→ AsyncTryClaimSlot(txn: compare+PUT)       # L3: 槽位抢占
                    ├─ ok → OnRegDone(node_id=slot)
                    └─ fail → next slot → OnRegScan(递归)

OnRegDone()
  → m_regInProgress=false, m_regStuckTicks=0
  → Emit(CenterEventType::Registered, node_id=m_nodeId)
  → Manager::OnCenterEvent → route mirror shm 写入
```

**关键文件**:
- `EtcdCenterConnector.cpp:425-530` — 注册延续链
- `EtcdCenterConnector.cpp:533-557` — AsyncRebindRegistration
- `EtcdCenterConnector.cpp:398-414` — AsyncTryClaimSlot
- `EtcdCenterConnector.cpp:492-498` — BuildSlotTxn(txn JSON)

**关键保护**:
- `m_regInProgress` — 防 re-entrancy(定时器+心跳同时触发注册)
- `m_regStuckTicks` — 30s 超时复位(异步链异常时死锁恢复, #27)
- `[&]` 按值捕获 — 防 async 回调 dangling ref crash (a27c83d)
- `SO_REUSEADDR` — 防重启端口竞态 (#30)
