# etcd 设计文档

Thunder 用 etcd 替代自研 Center，承担服务注册、路由下发、配置存储三项职责。

---

## 一、整体架构

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

Manager 持有 `CenterConnector*`，`EtcdGrpcConnector` 是其 etcd 实现。业务逻辑只与接口交互，不感知 etcd 协议细节。

---

## 二、协议与依赖库

| 项 | 选型 |
|----|------|
| 客户端协议 | **gRPC**（etcd v3 API） |
| 客户端库 | `etcd-cpp-apiv3`（`code/3party/include/etcd/`） |
| 主要类 | `etcd::SyncClient`（阻塞 unary）、`etcd::Watcher`（server-side streaming） |
| etcd 版本 | v3.5.21 |
| 通信端口 | client: 2379/2381/2383，peer: 2380/2382/2384 |

**为什么用 gRPC 不用 HTTP**：Watch 是 server-side streaming RPC，HTTP/1.1 chunked 需要自己实现流解析；gRPC 库直接提供 `etcd::Watcher` 封装，可靠性更高。

---

## 三、etcd key 数据模型

```
/thunder/slot/{1..255}
    value: "ip:port"（绑定 lease）
    用途:  node_id 槽位占位。崩溃后 lease 过期自动释放，新节点可复用。

/thunder/registry/{node_type}/{ip:port}
    value: {"node_id":87,"node_type":"HELLO_HTTP","node_ip":"0.0.0.0",
            "node_port":27007,"worker_num":1}
    用途:  在线表。Watch 此前缀感知节点上下线。绑定同一 lease。

/thunder/config/module/{node_type}
    value: JSON 配置内容
    用途:  动态配置下发。5s 轮询感知变更（Watch 覆盖 registry，配置频率低）。
```

当前在线示例（5 节点）：

```
node_id  node_type     ip:port
87       HELLO_HTTP    0.0.0.0:27007
65       HELLO_WS      0.0.0.0:27011
70       HELLO_HTTPS   0.0.0.0:27444
209      INTERFACE     0.0.0.0:27009
155      LOGIC         0.0.0.0:16068
```

---

## 四、租约参数

```cpp
kLeaseTTL         = 30s   // 租约 TTL
kKeepAliveRefresh = 10s   // keepalive 检查间隔
kPollInterval     = 5s    // config 轮询间隔
kMaxSlot          = 255   // node_id 最大槽位数
```

keepalive 策略：每 10s 调用 `leasetimetolive`；remainTTL ≤ 15s（TTL/2）时 re-grant 新 lease，原 lease revoke，slot + registry key 重新绑定新 lease。

---

## 五、注册流程

```
ReportNodeStatus(node_report, is_register)   [libev 线程]
  → PostCmd({Register, ip, port, node_type, worker_num})
  → gRPC 线程 DoRegisterGrpc:

1. GET /thunder/registry/{node_type}/{ip:port}
   ├─ 存在且 node_id > 0
   │    → PUT slot/{nid} + registry key（绑新 lease）→ rebind，跳过 slot scan
   └─ 不存在或解析失败
        → slot scan（从 hash(ip:port)%255+1 起轮询，防惊群）：
             txn { compare: create_revision(slot/i)==0
                   success: PUT slot/i + PUT registry/...
                   failure: GET slot/i（查看占用者） }
          → 找到空槽 → m_nodeId = slot
          → 所有槽满 → 注册失败（PushEvent Registered with errcode=-1）

2. 注册成功 → PushEvent(Registered, node_id=slot)
           → DoInitialSnapshot → DoStartWatch
```

---

## 六、路由下发（Watch）

```
DoInitialSnapshot（gRPC 线程，注册成功 / Watch 重建时调用）:
  client.ls("/thunder/registry/")
  → 全量填充 m_nodeRegistry（map<ip:port, JSON>）
  → m_watchRevision = resp.index()    // Watch 从此 revision+1 开始，不漏事件
  → AssembleAndPushRouteUpdated()

DoStartWatch（gRPC 线程）:
  etcd::Watcher(client, "/thunder/registry/", revision+1, OnWatchEvent, OnWatchEnded, recursive)
  → 启动 Watcher::task_ 线程，监听 server-side streaming

OnWatchEvent（Watcher 内部线程）:
  for each event:
    PUT   → m_nodeRegistry[ipPort] = new_value
    DELETE → m_nodeRegistry.erase(ipPort)
  → AssembleAndPushRouteUpdated() → PushEvent(RouteUpdated)

AssembleAndPushRouteUpdated:
  遍历 m_nodeRegistry → 组装 NodeNotice（protobuf）→ serialize → CenterEvent.route_snapshot
  → PushEvent → ev_async_send → libev 线程回调 → Manager 更新路由 shm
```

Watch 断流处理：

```
OnWatchEnded(cancelled=false)  [非主动 Cancel，etcd 断连等]
  → m_watchEnded = true → cmdCv.notify_one()

GrpcThreadMain tick:
  m_watchEnded == true
    → m_watcher.reset()         // join task_，确保 OnWatchEvent 不再并发
    → DoInitialSnapshot()       // 全量重建快照（从新 revision 开始）
    → DoStartWatch()            // 重新建立 Watch 流
```

---

## 七、线程安全

| 数据 | 保护方式 |
|------|----------|
| `m_nodeRegistry` | `m_registryMutex`（gRPC 线程写快照，Watcher 线程写事件） |
| `m_eventQueue` | `m_eventMutex` |
| `m_cmdQueue` | `m_cmdMutex` + `m_cmdCv` |
| `m_registered` | `std::atomic<bool>` |
| `m_watchEnded` | `std::atomic<bool>` |
| `ev_async_send` | libev 保证线程安全 |
| `etcd::SyncClient` | 仅 gRPC 线程访问，无锁 |

---

## 八、优雅关闭（leaserevoke）

```
Destroy()  [libev 线程]
  → PostCmd(Stop)
  → m_grpcThread.join()

GrpcThreadMain 收到 Stop:
  1. etcdClient.leaserevoke(m_leaseId)   // 立即删除 slot + registry key（<1s）
  2. cancelWatcher()                      // Cancel + reset m_watcher（join task_）
  3. return                               // etcdClient 析构（比 m_watcher 后析构）

leaserevoke 顺序说明：
  revoke 优先于 cancelWatcher，因为 gRPC Watch stream Cancel 可能耗时 >10s。
  若 SIGKILL 在 cancelWatcher 期间到来，leaserevoke 已完成，key 已删除。
```

**容器层信号传递**（`docker/docker-compose.yml`）：

```
docker stop → SIGTERM → docker-init(PID1) → bash(PID7, SESS=1, tini 直接子进程)
  → bash 的 wait $! 被中断
  → trap './node.sh stop' 触发
      → node.sh 找到 Manager PID → kill（SIGTERM）
      → node.sh kill -0 轮询最多 25s（等 Manager 真正退出）
  → bash 退出

配置：stop_grace_period: 30s（防 docker 在 10s 后提前 SIGKILL）
```

修复前：`exec tail` 替换 bash，trap 无处注册，Manager 收不到 SIGTERM，key 靠 TTL 过期（~19s）。  
修复后：key 在 **1.2s** 内通过 leaserevoke 立即删除。

---

## 九、配置管理

```
DoPollConfig（gRPC 线程，每 5s）:
  GET /thunder/config/module/{node_type}
  → value 变化 → PushEvent(ConfigUpdated, config_content)
  → Manager 更新配置 shm

PutConfig(key, value)  [libev 线程]:
  → PostCmd(PutConfig)
  → gRPC 线程: etcdClient.put(key, value, m_leaseId)
```

config 用轮询而非 Watch，原因：配置变更频率极低，5s 延迟可接受，不值得额外维护 Watch 流。

---

## 十、3 节点 etcd 集群

```
节点      client 地址            peer 地址
etcd1     http://127.0.0.1:2379  http://127.0.0.1:2380
etcd2     http://127.0.0.1:2381  http://127.0.0.1:2382
etcd3     http://127.0.0.1:2383  http://127.0.0.1:2384

cluster token: thunder-etcd-cluster
auto compaction: periodic, 1h
```

业务节点配置（`conf/*.json`）：

```json
"center": {
    "connector": "etcd-grpc",
    "etcd_endpoints": "http://127.0.0.1:2379,http://127.0.0.1:2381,http://127.0.0.1:2383"
}
```

`EtcdGrpcConnector::Init` 当前只取第一个 endpoint（逗号分割取首项）；多 endpoint 故障转移留待后续实现。

---

## 十一、管理工具

```bash
deploy/scripts/
  admin.py         # 查在线节点（node_id / node_type / ip:port / lease）
  admin_nodes.py   # 详细节点信息
  admin_config.py  # 查/改 /thunder/config/ 下的配置 key
  admin_status.sh  # etcd 集群健康状态

# 常用
python3 deploy/scripts/admin.py nodes
python3 deploy/scripts/admin_config.py get /thunder/config/module/HELLO_HTTP
```

---

## 十二、关键常数速查

| 常数 | 值 | 位置 |
|------|----|------|
| `kLeaseTTL` | 30s | `EtcdGrpcConnector.cpp` |
| `kKeepAliveRefresh` | 10s | 同上 |
| `kPollInterval` | 5s | 同上 |
| `kMaxSlot` | 255 | 同上 |
| `kRegistryPrefix` | `/thunder/registry/` | 同上 |
| `kConfigPrefix` | `/thunder/config/` | 同上 |

---

## 十三、相关文件

| 文件 | 说明 |
|------|------|
| `code/Net/include/labor/CenterConnector.hpp` | 接口定义（CenterEvent / CenterEventType / CenterEventCallback） |
| `code/Net/src/register/EtcdGrpcConnector.hpp` | 实现头文件（线程模型注释） |
| `code/Net/src/register/EtcdGrpcConnector.cpp` | 完整实现 |
| `docker/docker-compose.yml` | 3 节点 etcd + 5 业务服务 trap 模式 |
| `deploy/*/conf/*.json` | 各服务 `center.connector=etcd-grpc` 配置 |
| `deploy/scripts/admin*.{py,sh}` | 管理工具 |
| `tests/e2e/test_etcd_watch.py` | Watch 事件一致性 E2E 测试 |
| `tests/e2e/test_etcd_stability.py` | 全链路稳定性测试（S1–S6） |

## 八、etcd vs Nacos

> 为什么选 etcd 而非 Nacos

| 维度 | etcd | Nacos |
|------|------|-------|
| 语言/运行时 | Go，单二进制 10MB | Java，需 JDK，起步 512MB-1GB |
| 内存占用 | 空闲 ~20MB，10K key ~50MB | JVM 堆内存 512MB+，GC 暂停 |
| 写性能 | ~10K ops/s (Raft) | ~3K ops/s (CP 模式) |
| 读性能 | 线性读 ~50K/s，串行读 ~200K/s | ~10K/s (有缓存) |
| 一致性 | Raft (强一致) | CP 模式用 Raft，AP 模式用 Distro |
| 服务发现 | 通过 KV + Watch 模拟 | 原生支持 |
| 配置中心 | 通过 KV + Watch 模拟 | 原生支持 |
| Watch | 原生增量推送，低延迟 | 2.x 支持，成熟度不如 etcd |
| 运维 | 单二进制，零依赖 | JDK + GC 调优 + DB (集群模式) |
| 适用场景 | 注册中心 + 配置中心（需二次开发） | 一站式服务治理平台 |

**选择 etcd 的原因**：

- Thunder 已有自研服务发现框架，只需 KV + Watch 原语，不需要 Nacos 的全套
- etcd 内存和 CPU 开销远低于 Nacos，适合容器化部署（每节点 <100MB）
- etcd 的 Watch 增量推送是 Thunder 热更新（Lua/SO/路由）的基石，Nacos 的推送延迟更高
- Go 免 GC 内存，无 JVM 调优负担，运维成本低
