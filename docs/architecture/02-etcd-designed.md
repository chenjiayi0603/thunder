# etcd 服务发现与配置同步

> 源码: `code/Net/src/register/EtcdGrpcConnector.{hpp,cpp}`, 接口: `code/Net/include/labor/CenterConnector.hpp`

Thunder 用 etcd 替代自研 Center，承担服务注册、路由下发、配置存储三项职责。

---

## 线程架构

```
Manager 进程
  │
  ├── libev 主线程（事件循环）
  │     ├── Init() → 启动 gRPC 线程
  │     ├── ReportNodeStatus() → PostCmd(Register)
  │     ├── PutConfig()        → PostCmd(PutConfig)
  │     ├── Destroy()          → PostCmd(Stop) → join gRPC 线程
  │     └── ev_async 回调 → 消费事件队列 → CenterEventCallback
  │
  ├── gRPC 专属线程（GrpcThreadMain）
  │     ├── LeaseGrant（unary）
  │     ├── DoRegisterGrpc → SlotTxn（unary CAS）
  │     ├── DoInitialSnapshot → ls /thunder/registry/（unary）
  │     ├── DoStartWatch → 启动 etcd::Watcher
  │     ├── DoKeepalive（每 10s，unary leasetimetolive + re-grant）
  │     ├── DoPollConfig（每 5s，unary get /thunder/config/...）
  │     └── Watch 断流 → reset + DoInitialSnapshot + DoStartWatch
  │
  └── Watcher 内部线程（etcd::Watcher::task_）
        ├── OnWatchEvent → 更新 m_nodeRegistry → PushEvent(RouteUpdated)
        └── OnWatchEnded → m_watchEnded=true → 通知 gRPC 线程重建
```

---

## etcd key 数据模型

```
/thunder/slot/{1..255}
    value: "ip:port"（绑定 lease）
    用途:  node_id 槽位占位。崩溃后 lease 过期自动释放。

/thunder/registry/{node_type}/{ip:port}
    value: {"node_id":87,"node_type":"HELLO_HTTP",...}
    用途:  在线表。Watch 前缀感知节点上下线。绑定同一 lease。

/thunder/config/module/{node_type}
    value: JSON 配置内容
    用途:  动态配置下发。5s 轮询感知变更。
```

---

## 注册流程

```
ReportNodeStatus(node_report, is_register)   [libev 线程]
  → PostCmd({Register, ip, port, node_type, worker_num})
  → gRPC 线程 DoRegisterGrpc:

1. GET /thunder/registry/{node_type}/{ip:port}
   ├─ 存在且 node_id > 0
   │    → PUT slot/{nid} + registry key（绑新 lease）→ rebind
   └─ 不存在或解析失败
        → slot scan（从 hash(ip:port)%255+1 起轮询，防惊群）：
             txn { compare: create_revision(slot/i)==0
                   success: PUT slot/i + PUT registry/...
                   failure: GET slot/i }
          → 找到空槽 → m_nodeId = slot
          → 所有槽满 → 注册失败

2. 注册成功 → PushEvent(Registered) → DoInitialSnapshot → DoStartWatch
```

---

## 路由下发（Watch）

```
DoInitialSnapshot:
  client.ls("/thunder/registry/")
  → 全量填充 m_nodeRegistry
  → m_watchRevision = resp.index()
  → AssembleAndPushRouteUpdated()

DoStartWatch:
  etcd::Watcher(client, "/thunder/registry/", revision+1, OnWatchEvent, OnWatchEnded, recursive)

OnWatchEvent (Watcher 线程):
  PUT   → m_nodeRegistry[ipPort] = new_value
  DELETE → m_nodeRegistry.erase(ipPort)
  → AssembleAndPushRouteUpdated() → PushEvent(RouteUpdated)

Watch 断流:
  OnWatchEnded → m_watchEnded=true
  → m_watcher.reset() → DoInitialSnapshot() → DoStartWatch()
```

---

## 租约参数

```
kLeaseTTL         = 30s   // 租约 TTL
kKeepAliveRefresh = 10s   // keepalive 检查间隔
kPollInterval     = 5s    // config 轮询间隔
kMaxSlot          = 255   // node_id 最大槽位数
```

keepalive 策略：每 10s 调用 `leasetimetolive`；remainTTL ≤ 15s（TTL/2）时 re-grant 新 lease。

---

## 线程安全

| 数据 | 保护方式 |
|------|----------|
| `m_nodeRegistry` | `m_registryMutex` |
| `m_eventQueue` | `m_eventMutex` |
| `m_cmdQueue` | `m_cmdMutex` + `m_cmdCv` |
| `m_registered` | `std::atomic<bool>` |
| `m_watchEnded` | `std::atomic<bool>` |
| `ev_async_send` | libev 保证线程安全 |
| `etcd::SyncClient` | 仅 gRPC 线程访问，无锁 |

---

## 优雅关闭

```
Destroy() → PostCmd(Stop) → m_grpcThread.join()

GrpcThreadMain 收到 Stop:
  1. etcdClient.leaserevoke(m_leaseId)  // 立即删除 slot + registry key
  2. cancelWatcher()
  3. return
```

revoke 优先于 cancelWatcher，确保 key 在 ~1s 内删除。

---

## etcd vs Nacos

| 维度 | etcd | Nacos |
|------|------|-------|
| 语言/运行时 | Go，单二进制 10MB | Java，需 JDK，512MB-1GB |
| 内存占用 | 空闲 ~20MB，10K key ~50MB | JVM 堆 512MB+ |
| 写性能 | ~10K ops/s (Raft) | ~3K ops/s (CP 模式) |
| 读性能 | 线性读 ~50K/s，串行读 ~200K/s | ~10K/s |
| 一致性 | Raft (强一致) | CP 模式用 Raft |
| Watch | 原生增量推送，低延迟 | 2.x 支持 |
| 运维 | 单二进制，零依赖 | JDK + GC 调优 + DB |

选择 etcd：Thunder 只需 KV + Watch 原语；内存/CPU 开销低；Watch 增量推送是热更新基石。
