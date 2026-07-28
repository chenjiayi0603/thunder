# node_id 分布式分配

> 源码: `code/Net/src/labor/EtcdCenterConnector.cpp` (AsyncTryClaimSlot, BuildSlotTxn)
> 每个 Thunder 节点需要一个唯一 node_id (1-255) 用于路由标识和槽位索引

---

## 分配流程

```
ReportNodeStatus()
  └→ DoRegister(ip, port, type)
       │
       ├─ L1 确保 lease
       │    └→ AsyncLeaseGrant → m_leaseId
       │
       ├─ L2 查 registry
       │    └→ AsyncQueryRegistry
       │         │
       │         ├─ Fresh (键存在 + lease 匹配)
       │         │    └→ m_nodeId=原值, OnRegDone(true)
       │         │
       │         ├─ Rebind (键存在 + lease 不匹配)
       │         │    └→ PUT slot+registry → OnRegDone
       │         │
       │         └─ Claim (键不存在) → L3
       │
       └─ L3 槽位扫描
            └→ AsyncTryClaimSlot(slot=N)
                 └→ BuildSlotTxn
                      ├─ OK    → m_nodeId=N
                      └─ 占用  → N++, 递归
```

---

## 槽位抢占 (etcd txn)

```
AsyncTryClaimSlot(N) → BuildSlotTxn(N, slotKey, registryKey, ipPort)
→ POST /v3/kv/txn {
    compare:   slot/N 的 create_revision == 0 (key 不存在)
    success:   PUT slot/N=ip:port  +  PUT registry/ip:port=JSON{node_id:N,...}
               (both with lease=m_leaseId)
    failure:   range slot/N
  }

原子性: etcd txn 保证 compare AND success 原子——两个节点不会抢到同一 slot。
```

---

## 分配算法

### 起始槽位计算

```cpp
uint32_t hashVal = 0;
for (unsigned char c : ipPort) hashVal += c;
const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]
```

字节累加哈希，打散不同 ip:port 的起始位，防惊群。

### 扫描策略

```cpp
for (uint32_t loop = 0; loop < kMaxSlot; ++loop) {
    const int slotIdx = ((startSlot - 1 + loop) % kMaxSlot) + 1;
    if TryClaimSlot(slotIdx) → found → return slotIdx;
}
return "所有槽位已满"
```

---

## 注册表数据结构

```
/thunder/slot/{1..255}    → value = "ip:port"
/thunder/registry/ip:port  → value = {"node_id":N, "node_type":"LOGIC", ...}
```

一致性约束: slot/N 和 registry/ip:port 的 lease 必须相同(同一个 txn)；slot/N 的 value = registry 的 key 后缀(ip:port)；registry 的 node_id = N。

---

## 关键边界

| 边界 | 处理 |
|------|------|
| 并发抢同一 slot | etcd txn atomic |
| 重启 lease 未过期 | Fresh 路径直接复用原 node_id |
| 重启 lease 已过期 | Claim 路径重新抢占 |
| 255 槽位全满 | OnRegDone(false) |
| m_regInProgress 死锁 | 30s 超时复位 |
| 绑定端口竞态 | SO_REUSEADDR |

---

## 实测分配

```
节点        ip:port              hash起始   实际nid  扫描步数
LOGIC       127.0.0.1:16068      247        247        1
INTERFACE   127.0.0.1:27009      244        244        1
HELLO       127.0.0.1:27011      237        237        1
```

空集群首次分配，hash 起始位恰好命中空闲槽，无冲突扫描。
