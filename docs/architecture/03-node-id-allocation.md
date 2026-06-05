# node_id 分配设计

> 日期: 2026-06-05
> 涉及: `EtcdCenterConnector`, `BuildSlotTxn`, `[`AsyncTryClaimSlot`](../../code/Net/src/register/EtcdCenterConnector.cpp)`

---

## 1. 设计原理

### 为什么需要 node_id

Thunder 多节点集群中,每个节点(Logic/Hello/Interface)需要全局唯一的标识符。Worker 进程根据 node_id 建立 S2S 连接的 identity(`node_type + node_id = "LOGIC.247"`),路由表用 node_id 索引目标节点的 ip:port。

### 为什么用 etcd 槽位(1-255)

```
槽位 = node_id = 全局唯一序号, 1~255

好处:
  · 自增序号 → Worker identity 简短, 不需要 UUID
  · etcd 原子 txn → 并发抢 slot 不会碰撞
  · lease 绑定 → 崩溃自动回收(lease TTL=10s 过期,etcd 删 key)
  · 重启复用 → lease 未过期时直接用原 node_id
  · 无需中心发号器 → Center 可彻底下线
```

### 为什么用 hash 起始 + 顺序扫描

```
hash(ip:port) % 255 + 1 → 起始槽位

好处:
  · 不同 ip:port 散布到不同起始位 → 降低同时注册的碰撞概率
  · 简单字节累加 → 零内存, 纯计算
  · 顺序扫描 → 最坏 O(255), 但实际集群只有 3~10 个节点, 平均 O(1)
```

### 为什么用 lease 绑定而非永久占用

```
PUT slot/N + PUT registry/ip:port → 同时绑定 lease(10s TTL)
KeepAlive 每 3s 续租一次

好处:
  · 进程崩溃 → lease 不续 → etcd 10s 后自动删 key → slot 释放
  · 重启回来 → lease 还没过期 → Fresh路径 直接复用原 node_id
  · 重启晚了 → lease 已过期 → Claim路径 重新抢占(可能换号)
```

---

## 2. 实现过程

### 2.1 初始化: 异步申请租约

```
Manager::Run()
  └→ EtcdCenterConnector::Init()
       ├→ AsyncLeaseGrant(TTL=10s)   // fire-and-forget, 不阻塞循环
       ├→ StartKeepAliveTimer(3s)    // 启动续租定时器
       └→ StartWatch()               // 启动 etcd watch 长连接
```

节点启动时不等租约——`AsyncLeaseGrant` 发出 HTTP 请求后立即返回,callback 中设置 `m_leaseId`。若 etcd 暂不可达,`OnKeepAliveTimer` 会在 3s 后重试补领。

### 2.2 注册流程: 三层决策

```
Manager 定时心跳 → ReportNodeStatus(m_nodeIp, m_nodePort, m_nodeType)
  └→ DoRegister(ip, port, type)
       │
       ├─ L1 确保 lease ──────────────────► AsyncLeaseGrant → m_leaseId
       │   (OnRegEnsureLease)                 若 m_leaseId==0 → 补领
       │                                       成功则继续,L2; 失败则 OnRegDone(false)
       │
       ├─ L2 查 registry ─────────────────► AsyncQueryRegistry(/thunder/registry/ip:port)
       │   (OnRegQuery)                       etcd range 查询该 ip:port 是否已有注册键
       │    │
       │    ├─ Fresh: 键存在 + lease 匹配当前 → m_nodeId=原值, OnRegDone(true)
       │    │   场景: 重启且在 TTL 内回来
       │    │
       │    ├─ Rebind: 键存在 + lease 不匹配 → AsyncRebindRegistration
       │    │   场景: 重启, etcd 里残留旧进程的注册键(绑在旧 lease)
       │    │   动作: PUT slot/N + PUT registry/ip:port, 绑定【当前新 lease】
       │    │
       │    └─ Claim: 键不存在             → L3 槽位扫描
       │        场景: 全新节点或旧 lease 已过期被 etcd 删除
       │
       └─ L3 槽位扫描 ─────────────────────► 从 hash(ip:port)%255+1 开始,顺序找空槽
            (OnRegScan)                      每个槽位: AsyncTryClaimSlot(txn)
                 │
                 └→ etcd txn: compare slot/N 不存在 → PUT slot+registry → OK
                      OK  → m_nodeId=N, OnRegDone(true)
                      占用 → N++, 继续扫描(最坏 255 次)
```

### 2.3 续租: KeepAlive 定时器

```
OnKeepAliveTimer() — 每 3s 触发一次
  ├─ m_leaseId==0         → 补领 lease → DoRegister  # etcd 恢复后自愈
  ├─ !m_registered        → 续租 → 成功恢复; 连续失败 10 次 → 重新注册
  └─ 正常                  → 续租 → 失败 → ConnectionLost → 重新注册
```

### 2.4 完成: 路由下发

```
OnRegDone(true)
  → Emit(CenterEventType::Registered, node_id=m_nodeId)
  → Manager::OnCenterEvent
       → m_uiNodeId = node_id         // Worker 建 identity
       → route mirror → shm 写入      // 跨节点 S2S 路由表更新
       → Worker 读取 shm → 建立 S2S 连接
```

---

## 2. 分配流程

```
ReportNodeStatus()                                          [EtcdCenterConnector.cpp#L220](../../code/Net/src/register/EtcdCenterConnector.cpp#L220)
  └→ DoRegister(ip, port, type)                             [EtcdCenterConnector.cpp#L425](../../code/Net/src/register/EtcdCenterConnector.cpp#L425)
       │
       ├─ L1 确保 lease                                      [EtcdCenterConnector.cpp#L443](../../code/Net/src/register/EtcdCenterConnector.cpp#L443)
       │    └→ [`AsyncLeaseGrant`](../../code/Net/src/register/EtcdCenterConnector.cpp) → m_leaseId                  [EtcdCenterConnector.cpp#L261](../../code/Net/src/register/EtcdCenterConnector.cpp#L261)
       │
       ├─ L2 查 registry                                     [EtcdCenterConnector.cpp#L454](../../code/Net/src/register/EtcdCenterConnector.cpp#L454)
       │    └→ [`AsyncQueryRegistry`](../../code/Net/src/register/EtcdCenterConnector.cpp)                            [EtcdCenterConnector.cpp#L306](../../code/Net/src/register/EtcdCenterConnector.cpp#L306)
       │         │
       │         ├─ Fresh (键存在 + lease 匹配)              [EtcdCenterConnector.cpp#L464](../../code/Net/src/register/EtcdCenterConnector.cpp#L464)
       │         │    └→ m_nodeId=原值, OnRegDone(true)     [EtcdCenterConnector.cpp#L514](../../code/Net/src/register/EtcdCenterConnector.cpp#L514)
       │         │
       │         ├─ Rebind (键存在 + lease 不匹配)           [EtcdCenterConnector.cpp#L476](../../code/Net/src/register/EtcdCenterConnector.cpp#L476)
       │         │    └→ [`AsyncRebindRegistration`](../../code/Net/src/register/EtcdCenterConnector.cpp)             [EtcdCenterConnector.cpp#L533](../../code/Net/src/register/EtcdCenterConnector.cpp#L533)
       │         │         └→ PUT slot+registry → OnRegDone
       │         │
       │         └─ Claim (键不存在) → L3                    [EtcdCenterConnector.cpp#L488](../../code/Net/src/register/EtcdCenterConnector.cpp#L488)
       │
       └─ L3 槽位扫描                                        [EtcdCenterConnector.cpp#L488](../../code/Net/src/register/EtcdCenterConnector.cpp#L488)
            └→ [`AsyncTryClaimSlot`](../../code/Net/src/register/EtcdCenterConnector.cpp)(slot=N)                    [EtcdCenterConnector.cpp#L405](../../code/Net/src/register/EtcdCenterConnector.cpp#L405)
                 └→ BuildSlotTxn                            [EtcdCenterConnector.cpp#L351](../../code/Net/src/register/EtcdCenterConnector.cpp#L351)
                      │
                      ├─ OK    → m_nodeId=N, OnRegDone
                      └─ 占用  → N++, OnRegScan 递归
```

**关键成员**: `m_regInProgress`(`[EtcdCenterConnector.hpp#L260](../../code/Net/src/register/EtcdCenterConnector.hpp#L260)`), `m_regSlot`(`:261`), `m_regStuckTicks`(`:262`)

## 3. 槽位抢占原理 — CAS + 双键原子写入

### 要解决的问题

多个节点同时启动,都需要分配 node_id。如何保证:

1. **不重复** — 任意两个节点不会拿到同一个 node_id
2. **不遗漏** — slot 和 registry 双向记录一致,不会出现"slot 指向的节点查不到 registry"

### 方案: 以一个 etcd txn 做 CAS

**CAS(Compare-And-Swap)**: 先检查"slot 是否空闲",如果空闲就占为己有。检查和抢占在同一个原子操作中完成。

### CAS 怎么实现的

etcd txn 请求就是一个三段的 JSON。以 Logic 节点(`127.0.0.1:16068`)抢 slot/247 为例,实际发出的请求:

```json
{
  "compare": [{
    "key":              "L3RodW5kZXIvc2xvdC8yNDc=",
    "target":           "CREATE",
    "result":           "EQUAL",
    "create_revision":  "0"
  }],
  "success": [
    {
      "request_put": {
        "key":   "L3RodW5kZXIvc2xvdC8yNDc=",
        "value": "MTI3LjAuMC4xOjE2MDY4",
        "lease": "7587895322829403153"
      }
    },
    {
      "request_put": {
        "key":   "L3RodW5kZXIvcmVnaXN0cnkvMTI3LjAuMC4xOjE2MDY4",
        "value": "eyJub2RlX2lkIjoyNDcsIm5vZGVfdHlwZSI6IkxPR0lDIiwibm...",
        "lease": "7587895322829403153"
      }
    }
  ],
  "failure": [{
    "request_range": {
      "key": "L3RodW5kZXIvc2xvdC8yNDc="
    }
  }]
}
```

解码后:

| 段 | base64 值 | 解码后 | 含义 |
|----|-----------|--------|------|
| compare.key | `L3RodW5kZXIvc2xvdC8yNDc=` | `/thunder/slot/247` | 检查这个 slot |
| compare.create_revision | `"0"` | 0 | key 从未创建过? |
| success[0].value | `MTI3LjAuMC4xOjE2MDY4` | `127.0.0.1:16068` | slot→IP 映射 |
| success[1].key | `...cmVnaXN0cnkvMTI3...` | `/thunder/registry/127.0.0.1:16068` | registry key |
| success[1].value | `eyJub2RlX2lkIjoyNDcs...` | `{"node_id":247,"node_type":"LOGIC","node_ip":"127.0.0.1","node_port":16068}` | 节点完整信息 |
| success[*].lease | `758789...` | 相同值 | 两个 key 绑同一个租约 |
| failure.key | `L3RodW5kZXIvc2xvdC8yNDc=` | `/thunder/slot/247` | 失败时空查(仅满足协议) |

**执行流程**:

```
① curl POST /v3/kv/txn 发送上述 JSON
② etcd 原子执行:
   - 读 /thunder/slot/247 的 create_revision
   - 等于 0? → 执行 success: 创建 slot/247 + registry/127.0.0.1:16068(同 lease)
   - 不等于 0? → 执行 failure: 仅 range(结果忽略)
③ 返回 {"succeeded": true}  或  {"succeeded": false}
```

如果另一个 Interface 节点(`127.0.0.1:27009`)正好也发 `/thunder/slot/244`,两个 JSON 的 key 不同,互相不冲突——Raft 日志中先后执行,各拿各的 slot。

关键字段:

| 字段 | 含义 | 值 |
|------|------|----|
| `compare.key` | 检查哪个 key | `/thunder/slot/247` |
| `compare.target` | 检查什么属性 | `CREATE` — 该 key 的创建 revision |
| `compare.result` | 比较方式 | `EQUAL` — 等于 |
| `compare.create_revision` | 期望值 | `0` — key 从未被创建过 |

翻译成人话: **"如果 slot/247 的 create_revision 等于 0(即 key 不存在),则执行 success 中的 PUT 操作;否则执行 failure"**。

### etcd 内部怎么保证原子性

```
客户端 → POST /v3/kv/txn  (一段 JSON)
         │
         ▼
    etcd Raft 层:
    把整个 txn JSON 打包成一个 Raft log entry ──► 复制到多数节点
         │
         ▼
    Raft 状态机 apply:
    ① 读取 slot/247 的 create_revision
    ② 和 compare.create_revision("0") 比较
    ③ 如果相等 → 执行 success 里的所有 PUT
    ④ 如果不相等 → 执行 failure
    ⑤ 返回 {succeeded: true/false}
```

**原子性的来源**: 第①~④步在 etcd 的 Raft 状态机中作为一个**不可分割的 apply 单元**执行。Raft 保证:

- 同一时刻只有一个 txn 在 apply(串行)
- 所有 PUT 要么全做,要么全不做
- 中间不会被其他请求打断

所以这个 JSON 就是一个**无锁的分布式 CAS**——不需要应用层加锁,Raft 的串行 apply 本身就是锁。

```
┌──── 一次 txn 请求 ────┐
│                        │
│  ① Compare:            │
│     slot/247 从来没被   │
│     创建过吗?           │
│         │              │
│    ┌────┴────┐         │
│    │ YES     │ NO      │
│    ▼         ▼         │
│  ② Write    ③ Skip    │
│  PUT slot   (空操作)    │
│  PUT reg               │
│                        │
└────────────────────────┘
```

etcd 的 Raft 日志保证: 所有 txn 请求串行执行。节点 A 和 B 同时抢 slot/247:

```
Raft 日志顺序:
  entry_1: A 的 txn → compare 通过 → slot/247 被 A 创建
  entry_2: B 的 txn → compare 失败 → slot/247 已存在 → B 返回 false

结果: A=247, B 自动扫描下一个槽位(248)
```

**不需要分布式锁,不需要协调者**——Raft 的串行 apply 本身就是天然的互斥。

### 为什么一次写两个 key

slot 和 registry 必须在同一 txn 中创建:

```
slot/247    → "127.0.0.1:16068"               ← 反向索引: slot号→IP
registry/127.0.0.1:16068 → {node_id:247,...}   ← 正向索引: IP→node_id

如果分两次 PUT:
  slot 写成功 + registry 写失败 → slot 永久指向一个"幽灵节点"
  
一次 txn 保证: 两个 key 要么都创建, 要么都不创建。
```

---

## 4. 关键边界

| 边界 | 处理 | 代码 |
|------|------|------|
| **并发抢同一 slot** | etcd txn atomic | `[EtcdCenterConnector.cpp#L405](../../code/Net/src/register/EtcdCenterConnector.cpp#L405)` |
| **重启 lease 未过期** | Fresh 路径直接复用 | `[EtcdCenterConnector.cpp#L464](../../code/Net/src/register/EtcdCenterConnector.cpp#L464)` |
| **重启 lease 已过期** | Claim 路径重新抢占 | `[EtcdCenterConnector.cpp#L488](../../code/Net/src/register/EtcdCenterConnector.cpp#L488)` |
| **255 槽位全满** | OnRegDone(false) | `[EtcdCenterConnector.cpp#L514](../../code/Net/src/register/EtcdCenterConnector.cpp#L514)` |
| **m_regInProgress 死锁** | 30s 超时复位(#27) | `[EtcdCenterConnector.cpp#L425](../../code/Net/src/register/EtcdCenterConnector.cpp#L425)` |
| **dangling ref crash** | [&]→按值捕获(#19) | `[EtcdCenterConnector.cpp#L545](../../code/Net/src/register/EtcdCenterConnector.cpp#L545)` |
| **绑定端口竞态** | SO_REUSEADDR(#30) | `[Manager.cpp#L1199](../../code/Net/src/labor/Manager.cpp#L1199)` |
| **管道数据错位** | SkipBytes 重试(#33) | `[Manager.cpp#L2325](../../code/Net/src/labor/Manager.cpp#L2325)` |

## 5. 分配算法细节

### 5.1 起始槽位计算

```cpp
uint32_t hashVal = 0;
for (unsigned char c : ipPort) hashVal += c;
const int startSlot = static_cast<int>(hashVal % kMaxSlot) + 1;  // [1..255]
```
`[EtcdCenterConnector.cpp#L488](../../code/Net/src/register/EtcdCenterConnector.cpp#L488)` — 简单字节累加哈希,打散不同 ip:port 的起始位。

### 5.2 扫描策略

```cpp
for (uint32_t loop = 0; loop < kMaxSlot; ++loop) {
    const int slotIdx = ((startSlot - 1 + loop) % kMaxSlot) + 1;
    if TryClaimSlot(slotIdx) → found → return slotIdx;   // else → next
}
return "所有槽位已满"
```
`[EtcdCenterConnector.cpp#L488](../../code/Net/src/register/EtcdCenterConnector.cpp#L488)-510` — 保证每个槽位只尝试一次,255次内必然找到空槽。

### 5.3 Lease 绑定

每个 PUT 操作绑定 `m_leaseId`(异步申请,`kLeaseTTL=10s`)。续租间隔 `kKeepAliveInterval=3s`,失败累计超过阈值触发 reconnect。
参见 `[EtcdCenterConnector.cpp#L261](../../code/Net/src/register/EtcdCenterConnector.cpp#L261)`([`AsyncLeaseGrant`](../../code/Net/src/register/EtcdCenterConnector.cpp)), `[EtcdCenterConnector.cpp#L278](../../code/Net/src/register/EtcdCenterConnector.cpp#L278)`([`AsyncKeepAlive`](../../code/Net/src/register/EtcdCenterConnector.cpp))。

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

### 6.1 客户端查询指令

```bash
# ① 查看所有在线节点(node_id/type/ip/port/lease)
python3 deploy/scripts/admin.py nodes

# ② 查看路由表(哪个 type 对应哪个 node_id)
python3 deploy/scripts/admin.py routes

# ③ 查看 etcd 注册表原始数据
./tests/logs.sh --etcd

# ④ 从业务日志查看 node_id 分配日志
./tests/logs.sh --logic 1 "node_id 分配"
# 输出: <<< node_id 分配完成: 247 (type=LOGIC addr=127.0.0.1:16068 lease=...) >>>

# ⑤ 冒烟测试一键验证
./tests/test_smoke.sh --etcd          # 只看 etcd 段
./tests/test_smoke.sh                 # 全量 12 项
```

### 6.2 操作指令(底层 curl)

```bash
# 查看全量注册表
curl http://127.0.0.1:2379/v3/kv/range \
  -d '{"key":"L3RodW5kZXIv","range_end":"L3RodW5kZXIw"}'

# 只看 registry(kv JSON 解码)
etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/registry/ -w simple

# 只看 slot
etcdctl --endpoints=http://127.0.0.1:2379 get --prefix /thunder/slot/ -w simple

# Thunder 封装 → 一行查看所有
python3 deploy/scripts/admin.py nodes
./tests/logs.sh --etcd
```

### 6.3 代码位置

| 操作 | 文件 | 行 |
|------|------|----|
| slot PUT | [BuildSlotTxn](../../code/Net/src/register/EtcdCenterConnector.cpp#L351) | 构造 txn JSON |
| registry value 构造 | [BuildRegistryValueJson](../../code/Net/src/register/EtcdCenterConnector.cpp#L460) | JSON 拼接 |
| 异步 PUT 执行 | [AsyncTryClaimSlot](../../code/Net/src/register/EtcdCenterConnector.cpp#L405) | POST /v3/kv/txn |
| 重绑 PUT | [AsyncRebindRegistration](../../code/Net/src/register/EtcdCenterConnector.cpp#L533) | PUT slot+registry |
| 租约绑定 | [AsyncLeaseGrant](../../code/Net/src/register/EtcdCenterConnector.cpp#L261) | kLeaseTTL=10s |

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
  └→ [`AsyncLeaseGrant`](../../code/Net/src/register/EtcdCenterConnector.cpp)("/v3/lease/grant")                     # L1: 申请租约
      └→ callback: m_leaseId = id → DoRegister(ip,port,type)

DoRegister()
  └→ OnRegEnsureLease()                                     # 确保有 lease(无则补领)
      └→ OnRegQuery()                                       # L2: 查 registry/ip:port
          ├─ Fresh: 键存在 + lease 匹配 → [`AsyncKeepAlive`](../../code/Net/src/register/EtcdCenterConnector.cpp) → OnRegDone
          ├─ Rebind: 键存在 + lease 不匹配 → [`AsyncRebindRegistration`](../../code/Net/src/register/EtcdCenterConnector.cpp)(PUT slot+registry) → OnRegDone
          └─ Claim: 键不存在 → OnRegScan()
                └→ [`AsyncTryClaimSlot`](../../code/Net/src/register/EtcdCenterConnector.cpp)(txn: compare+PUT)       # L3: 槽位抢占
                    ├─ ok → OnRegDone(node_id=slot)
                    └─ fail → next slot → OnRegScan(递归)

OnRegDone()
  → m_regInProgress=false, m_regStuckTicks=0
  → Emit(CenterEventType::Registered, node_id=m_nodeId)
  → Manager::OnCenterEvent → route mirror shm 写入
```

**关键文件**:
- `[EtcdCenterConnector.cpp#L425](../../code/Net/src/register/EtcdCenterConnector.cpp#L425)-530` — 注册延续链
- `[EtcdCenterConnector.cpp#L533](../../code/Net/src/register/EtcdCenterConnector.cpp#L533)-557` — [`AsyncRebindRegistration`](../../code/Net/src/register/EtcdCenterConnector.cpp)
- `[EtcdCenterConnector.cpp#L398](../../code/Net/src/register/EtcdCenterConnector.cpp#L398)-414` — [`AsyncTryClaimSlot`](../../code/Net/src/register/EtcdCenterConnector.cpp)
- `[EtcdCenterConnector.cpp#L492](../../code/Net/src/register/EtcdCenterConnector.cpp#L492)-498` — BuildSlotTxn(txn JSON)

**关键保护**:
- `m_regInProgress` — 防 re-entrancy(定时器+心跳同时触发注册)
- `m_regStuckTicks` — 30s 超时复位(异步链异常时死锁恢复, #27)
- `[&]` 按值捕获 — 防 async 回调 dangling ref crash (a27c83d)
- `SO_REUSEADDR` — 防重启端口竞态 (#30)
