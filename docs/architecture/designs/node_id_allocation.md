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
ReportNodeStatus(m_nodeIp, m_nodePort, m_nodeType)
  │
  └→ DoRegister(ip, port, type)
       │
       ├─ L1 确保 lease ──────────────────► AsyncLeaseGrant → m_leaseId
       │
       ├─ L2 查 registry ─────────────────► AsyncQueryRegistry(/thunder/registry/ip:port)
       │    │
       │    ├─ Fresh (键存在 + lease 匹配)  → m_nodeId = 原值, 完成
       │    ├─ Rebind (键存在 + lease 不匹配)→ AsyncRebind(slot+registry PUT 到新 lease)
       │    └─ Claim (键不存在)             → L3 槽位扫描
       │
       └─ L3 槽位扫描 ─────────────────────► 从 hash(ip:port)%255+1 开始,顺序找空槽
            │
            └→ AsyncTryClaimSlot(slot=N)
                 │
                 ├─ OK    → m_nodeId = N, 完成
                 └─ 占用  → N++, 重试
```

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

| 边界 | 处理 | 验证方式 |
|------|------|---------|
| **并发抢同一 slot** | etcd txn atomic → 只有1个成功 | E2E 多节点同时注册 |
| **重启 lease 未过期** | Fresh路径: 键存在+lease匹配 → 续租复用原 node_id | 杀进程→3s内重启→node_id不变 |
| **重启 lease 已过期** | Claim路径: 键被 etcd 删除 → 重新抢占,可能换 node_id | 杀进程→等>10s→重启 |
| **255 槽位全满** | OnRegDone(false, "所有槽位已满") → emit error | 模拟255节点注册 |
| **node_id 唯一性** | txn 原子 compare → 同一 slot 只能被一个节点占有 | smoke test 验证 nids 无重复 |
| **node_id 范围** | 1-255, 从 hash(ip:port)%255+1 开始 | smoke test 验证所有 nid ∈ [1,255] |
| **slot↔registry 对应** | txn 同时写 slot 和 registry,同 lease | smoke test slot_ips==reg_ips |
| **m_regInProgress 死锁** | 30s 超时自动复位 (#27) | 模拟注册链异常→验证恢复 |

## 5. 分配算法细节

### 5.1 起始槽位计算

```cpp
uint32_t hashVal = 0;
for (unsigned char c : ipPort) hashVal += c;
const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]
```

简单字节累加哈希,打散不同 ip:port 的起始位,降低碰撞概率。

### 5.2 扫描策略

```cpp
for (uint32_t loop = 0; loop < kMaxSlot; ++loop) {
    const int slotIdx = ((startSlot - 1 + loop) % kMaxSlot) + 1;
    if TryClaimSlot(slotIdx) → found → return slotIdx
    // else → next slot
}
return "所有槽位已满"
```

保证每个槽位只尝试一次,255次内必然找到空槽或确定全满。

### 5.3 Lease 绑定

每个 PUT 操作绑定 `m_leaseId`(异步申请,TTL=10s)。续租间隔 3s,失败累计超过阈值触发 reconnect。

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
