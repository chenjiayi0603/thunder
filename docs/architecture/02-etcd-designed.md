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

## etcd Key 空间全貌

```
/thunder/
├── slot/                              # 节点 ID 分配 (1-255, CAS 抢占)
│   ├── 1   → "10.244.0.59:16068"     # value = ip:port, 绑 lease
│   └── 2   → "10.244.0.60:16068"
│
├── registry/                          # 服务注册 (Watch 实时感知上下线)
│   └── {NODE_TYPE}/                   # HELLO / INTERFACE / LOGIC / ...
│       └── {IP}:{PORT}                # 如 10.42.0.10:16068
│
├── canary/                            # 灰度路由权重 (Watch 实时)
│   └── {NODE_TYPE}/weights            # 如 LOGIC/weights
│
└── config/                            # SO/Lua 热更新 (5s 轮询)
    └── module/
        └── {NODE_TYPE}                # 如 HELLO_HTTP
```

C++ 常量 (EtcdGrpcConnector.cpp):

```cpp
static constexpr const char* kRegistryPrefix = "/thunder/registry/";
static constexpr const char* kCanaryPrefix   = "/thunder/canary/";
static constexpr const char* kConfigPrefix   = "/thunder/config/";
```

---

## 注册 Key 完整结构

**路径**: `/thunder/registry/{NODE_TYPE}/{IP}:{PORT}`

```cpp
// BuildRegistryKey() — 第 1056 行
return "/thunder/registry/" + nodeType + "/" + ip + ":" + std::to_string(port);
```

**Value** (JSON, 由 `BuildRegistryValue()` 构造):

```json
{
    "node_id":       208,
    "node_type":     "LOGIC",
    "node_ip":       "10.244.0.59",
    "node_port":     16068,
    "worker_num":    4,
    "node_version":  "v1",
    "registered_at": 1753700000
}
```

| 字段 | 类型 | 说明 |
|:---|:---|:---|
| `node_id` | int | 全局唯一，从 `/thunder/slot/` CAS 抢占 |
| `node_type` | string | HELLO / INTERFACE / LOGIC |
| `node_ip` | string | Pod IP (inner_host, Manager S2S 端口) |
| `node_port` | int | Manager inner_port |
| `worker_num` | int | Worker 进程数 |
| `node_version` | string | 版本标签 (`NODE_VERSION` 环境变量), 灰度权重分组依据 |
| `registered_at` | int64 | Unix 时间戳, **僵尸节点检测**：age > 60s 则过滤不参与路由 |

> **生命周期**: Key 绑定 Lease (TTL=60s), Manager 每 10s 调用 `leasetimetolive` 续约。
> Pod 宕机后 ~60s Key 自动删除, Watch 通知全网节点剔除路由。

---

## 配置 Key 完整结构 — SO/Lua 热更新

### {TYPE} 是什么？

`{TYPE}` 是 `m_strNodeType`，即配置文件中的 `node_type` 字段值。**每个 Manager 只轮询自己类型的配置 Key**。

构造代码 (`DoPollConfig()` 第 741 行):

```cpp
const std::string cfgKey = kConfigPrefix + "module/" + m_myNodeType;
// 生成: /thunder/config/module/HELLO_HTTP
```

实际值来自各部署的 `node_type` 配置:

| 配置 `node_type` | etcd Key | 用途 |
|:---|:---|:---|
| `HELLO_HTTP` | `/thunder/config/module/HELLO_HTTP` | HTTP 网关模块 |
| `HELLO_HTTPS` | `/thunder/config/module/HELLO_HTTPS` | HTTPS 网关模块 |
| `HELLO_WS` | `/thunder/config/module/HELLO_WS` | WebSocket 网关模块 |
| `INTERFACE` | `/thunder/config/module/INTERFACE` | API 接入层模块 |
| `LOGIC` | `/thunder/config/module/LOGIC` | 业务逻辑层模块 |

### Value 格式 — 多模块数组

**路径**: `/thunder/config/module/{NODE_TYPE}`

Value 是一个 `{"module": [...]}` 包裹的 **JSON 数组**，每个数组元素是一个独立模块（SO 或 Lua），**一个 Key 可同时管理多个模块**。

以 `HELLO_HTTP` 为例 (`deploy/HelloHttp/conf/Hello.json`)：

```json
{
  "module": [
    {
      "url_path":         "/hello/hello",
      "so_path":          "plugins/HelloHttp_ModuleHello.so",
      "entrance_symbol":  "create",
      "load":             true,
      "version":          6
    },
    {
      "url_path":         "/hello/lua_echo",
      "so_path":          "plugins/HelloHttp_ModuleLua.so",
      "entrance_symbol":  "create",
      "load":             true,
      "version":          12,
      "script_content":   "function handle_request(msg)\n  ...\nend"
    },
    {
      "url_path":         "/hello/raw",
      "so_path":          "plugins/HelloHttp_ModuleRaw.so",
      "entrance_symbol":  "create",
      "load":             true,
      "version":          1
    }
  ]
}
```

一个 Key 同时管理了 3 个模块: `ModuleHello.so` (SO) + `ModuleLua.so` (Lua 宿主) + `ModuleRaw.so` (SO)。

### 模块数组元素字段

| 字段 | 类型 | SO 模块 | Lua 脚本 | 说明 |
|:---|:---|:---:|:---:|:---|
| `url_path` | string | ✅ | ✅ | URL 路由前缀，请求匹配用 |
| `so_path` | string | ✅ | ✅ (指向宿主) | 插件在 `/app/plugins/` 下的路径 |
| `entrance_symbol` | string | ✅ | — | SO 导出入口函数名 (`create`) |
| `load` | bool | ✅ | ✅ | true=加载, false=卸载 |
| `version` | int | ✅ | ✅ | **单调递增**，Manager 逐模块对比触发更新 |
| `so_url` | string | ✅ | — | MinIO 下载地址 (Pull 模式) |
| `size` | int | ✅ | — | SO 文件大小 (字节) |
| `md5` | string | ✅ | — | 文件校验和 |
| `script_content` | string | — | ✅ | Lua 源码, 直接嵌入 etcd value |
| `script_url` | string | — | ✅ | MinIO 下载地址 (大 Lua 脚本 Pull 模式) |

### 多模块独立更新机制

Manager 收到 `ConfigUpdated` 后，**逐模块对比新旧版本** (第 2830-2865 行):

```
遍历 module 数组每个元素:
    ├─ so_path 不变 + version 不变  → 跳过
    │
    ├─ so_path 不变 + version 变大 + 有 script_content
    │    → Lua 脚本变更
    │    → 收集到 luaChangedIdx 列表
    │    → 不重启 Worker！SendToWorker(CMD_REQ_RELOAD_LUA)
    │
    ├─ so_path 不变 + version 变大 + 无 script_content
    │    → SO 模块变更
    │    → 标记 soOrModuleChanged = true
    │    → HTTP GET so_url 下载新 .so
    │    → GracefulRestartWorker (逐个 Worker Drain + 重启)
    │
    └─ so_path 变化
         → 模块增删，同上走 SO 重启路径
```

关键区别:

| 变更类型 | Worker 操作 | 连接保持 |
|:---|:---|:---|
| 纯 Lua 版本变更 | `ReloadScript()` — 重建 Lua VM | 零中断 |
| SO 版本变更 | `GracefulRestartWorker` — Drain + fork + dlopen | Drain 期间保持 |
| 模块增删 | 同上 SO 路径 | 同上 |

> **结论**: 一个 `PUT /thunder/config/module/HELLO_HTTP` 可以原子地同时更新 N 个模块，但只有 SO 变更的模块才会触发 Worker 重启；
> 纯 Lua 脚本变更仅重建 Lua VM，不触及进程。

### 写入保护 — 仅做种子写入

首次注册成功后，Manager 将本地 `module` 数组播种到 etcd（仅当 Key 不存在时写入），防止 admin 配置被覆盖:

```cpp
// Manager.cpp 第 2775 行: 首次种子写入
auto& modArr = m_oCurrentConf["module"];
std::string cfgVal = "{\"module\":" + modArr.ToString() + "}";
m_pCenterConnector->PutConfig(cfgKey, cfgVal);

// EtcdGrpcConnector.cpp 第 288 行: etcd CAS 保护
etcdv3::Transaction txn;
txn.add_compare_create(cmd.configKey, EQUAL, 0);      // 仅当 key 不存在
txn.add_success_put(cmd.configKey, cmd.configValue);   // 才 PUT
txn.add_failure_range(cmd.configKey);                  // 已存在 → 跳过
```

### 为什么配置走 5s 轮询而不是 Watch?

```
注册/路由:  Watch (实时) — 节点上下线需秒级感知
配置:       轮询 (5s)     — 版本发布级低频变更; 内容可能数 KB;
                         Watch 断流重建代价高, 低频数据轮询更简单可靠
```

---

## Canary 灰度 Key

**路径**: `/thunder/canary/{NODE_TYPE}/weights`

**Value**: `{"v1": 80, "v2": 20}`

按 `node_version` 分组分配权重。Manager Watch 此 Key 后展开为 ip:port 级权重，推送 Worker 做加权分流。

展开逻辑 (`AssembleAndPushRouteUpdated()`):
```
canary weights: {"v1": 80, "v2": 20}
    ↓
registry 查同 node_type 下各 ip:port 的 node_version
    ↓
同 version 均分权重: 2 个 v1 节点 → 各 40, 1 个 v2 节点 → 20
    ↓
填入 NodeNotice.canary_weights = {"10.0.0.1:16068": 40, "10.0.0.2:16068": 40, "10.0.0.3:16068": 20}
```

### NodeID 分配

slot 和 registry 通过 **同一个 etcd txn** 原子写入，绑定同一个 lease：

```
┌─ /thunder/slot/208 ─────────────┐  ┌─ /thunder/registry/LOGIC/10.244.0.59:16068 ─┐
│ value: "10.244.0.59:16068"      │  │ value: {"node_id":208,"node_type":"LOGIC",   │
│ lease: 9219606906552152395      │  │         "node_ip":"10.244.0.59",              │
│                                 │  │         "node_port":16068,"worker_num":1}     │
│                                 │  │ lease: 9219606906552152395                    │
└─────────────────────────────────┘  └──────────────────────────────────────────────┘
         ↑ 同一个 txn 原子写入 ↑              ↑ 绑定同一个 lease ↑
```

**一致性约束**：slot/N 的 value = registry 的 key 后缀 (ip:port)；registry 的 node_id 必须 = N；两个 key 的 lease 必须相同。

**分配算法**：`hash(ip:port) % 255 + 1` 计算起始槽位，从该位置轮询扫描，用 etcd txn `compare create_revision==0` 抢占空槽。重启时若 lease 未过期则直接复用原 node_id（Fresh 路径），过期则重新抢占（Claim 路径）。

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

## 通知结构 — NodeNotice / NodeReport (Protobuf)

etcd Watch 事件驱动 `NodeNotice` 在 Worker 进程间同步路由表。定义在 `code/Net/src/protocol/oss_sys.proto`:

```protobuf
message NodeNotice {
    repeated NodeReport node_arry_reg  = 1;   // 在线节点列表
    repeated NodeReport node_arry_exit = 2;   // 下线节点列表 (保留字段)
    map<string, int32>  canary_weights = 3;   // ip:port → weight
}

message NodeReport {
    string  node_type     = 1;    // HELLO / INTERFACE / LOGIC
    uint32  node_id       = 2;    // 全局唯一
    string  node_ip       = 3;    // inner_host
    uint32  node_port     = 4;    // inner_port
    string  access_ip     = 5;    // 对外 IP (无对外则空)
    uint32  access_port   = 6;    // 对外端口
    uint32  worker_num    = 7;    // Worker 进程数
    double  active_time   = 8;    // 最后活跃时间
    Node    node          = 9;    // Manager 级负载统计
    repeated Worker workers = 10; // 各 Worker 负载统计
    string  node_version  = 11;   // 灰度版本标签

    message Node {
        uint32 load      = 1;     // 负载指数 0-100
        uint32 connect   = 2;     // 连接数
        uint32 recv_num  = 3;
        uint32 recv_byte = 4;
        uint32 send_num  = 5;
        uint32 send_byte = 6;
        uint32 client    = 7;
    }
    message Worker { /* 字段同 Node */ }
}
```

**组装链路** (`AssembleAndPushRouteUpdated()`):

```
m_nodeRegistry (ip:port → JSON value)
    │
    ├─ 遍历 → 过滤僵尸节点 (registered_at > 60s) → 构造 NodeReport
    ├─ 按 canary_weights 展开 version 权重 → ip:port 权重
    │
    ▼
notice.SerializeAsString()
    │
    ▼
CenterEvent{ RouteUpdated, route_snapshot }
    │
    ▼
ev_async_send → Manager::OnCenterEvent() → ShmRingQueue → Worker
```

---

## 配置变更通知 — ConfigUpdated 事件

与路由不同，SO/Lua 配置变更通过 `ConfigUpdated` 事件传递，携带完整配置 JSON 字符串。

```cpp
// CenterConnector.hpp
enum class CenterEventType : uint8_t {
    ConfigUpdated,       // SO/Lua 配置变更
    ...
};

struct CenterEvent {
    CenterEventType type;
    uint32_t        node_id          = 0;
    std::string     route_snapshot;    // RouteUpdated 携带 (NodeNotice 序列化)
    std::string     config_content;    // ConfigUpdated 携带 (JSON 字符串)
    int             errcode          = 0;
    std::string     errmsg;
};
```

### SO 热更新流程

```
admin-web / CLI
    │
    ├─ ① PUT .so 文件 → 双写:
    │      - MinIO: artifacts/{TYPE}/{filename}  (主存储)
    │      - 本地:  /app/data/artifacts/{TYPE}/{filename}  (MinIO 不可用时的备份)
    │
    └─ ② POST deploy → 只写 etcd (不移动文件):
           PUT /thunder/config/module/{TYPE}
           { ..., "version": 42,
             "so_url": "http://{minio_host}/{bucket}/{TYPE}/{filename}",
             "so_path": "plugins/{filename}", "md5": "...", "load": true }
              │
              ▼  so_url 由 MinIO.GetObjectURL() 构造:
              │    优先: http://{endpoint}/artifacts/Logic/CmdGetToken.so
              │    降级: http://{admin_host}:8090/api/artifacts/Logic/CmdGetToken.so
              ▼
          etcd ──── 5s 轮询 ────► Manager
                                      │
                              DoPollConfig() 检测 version 变化
                                      │
                                      ▼
                              PushEvent(ConfigUpdated)
                                      │
                                      ▼
                              Manager: ① HTTP GET MinIO 下载 .so
                                       ② 校验 MD5
                                       ③ GracefulRestart Worker:
                                           Drain 旧 Worker (保连接)
                                           → fork 新 Worker
                                           → dlopen 新 .so
                                       ④ 热更新完成 (连接不丢)
```

### Lua 热更新流程 (与 SO 的关键区别)

```
etcd PUT /thunder/config/module/{TYPE}
  { ..., "version": 99, "script_content": "function handle_request(...) ... end" }
      │
      ▼
Manager 收到 ConfigUpdated → 解析 script_content
      │
      ▼
ShmRingQueue → Worker::ReloadScript()
      │
      ├─ 不重启进程！
      ├─ 不 dlopen
      └─ 直接重新解析 Lua 代码 → 热更新完成 (零中断)
```

### SO vs Lua 通知对比

| 维度 | SO 变更 | Lua 变更 |
|:---|:---|:---|
| 触发字段 | `version++` + `so_url` | `version++` + `script_content` 非空 |
| 制品传输 | MinIO HTTP GET (Pull) | etcd value 内联 |
| Worker 级操作 | GracefulRestart + dlopen | ReloadScript() — 无重启 |
| 连接保持 | Drain 保持 → 新 Worker 接管 | 零中断 |
| 生效延迟 | 5s (轮询) + Drain 时间 | 5s (轮询) |
| 适用场景 | 高性能路径, 复杂协议 | 业务逻辑, 频繁变更 |

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
kLeaseTTL         = 60s   // 租约 TTL (崩溃后 ~75s 自动剔除)
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

---

## etcd 注册发现 vs CoreDNS 注册发现

| 维度 | etcd | CoreDNS (K8s Service) |
|------|------|------|
| **原理** | KV 存储 + Watch 实时推送 | DNS 解析 + TTL 轮询 |
| **变更延迟** | Watch 毫秒级 | TTL 默认 30s, 最短 1s |
| **下线感知** | lease 过期 (~75s) → Watch 即时剔除 | DNS TTL 过期 → 下次解析才消失 |
| **权重灰度** | ✅ canary/weights key | ❌ 不支持 |
| **元数据** | ✅ node_id, version, worker_num | ❌ 只能 IP:Port |
| **故障隔离** | 独立 etcd 集群, 不依赖 K8s | 依赖 K8s API Server + CoreDNS |
| **跨集群** | 天然支持 (TCP 直连) | K8s 内建, 跨集群需额外配置 |
| **运维** | 需维护 etcd 集群 (3 节点) | K8s 自带, 零运维 |
| **一致性** | Raft 强一致 | 最终一致 (DNS 缓存) |

### Thunder 为什么选 etcd

1. **Watch 实时推送** — 路由变更毫秒级生效。CoreDNS 依赖 TTL 轮询, 最小 1s 但有缓存穿透
2. **权重灰度** — `/thunder/canary/LOGIC/weights` 直接控制流量比例, DNS 做不到
3. **元数据丰富** — node_id, version, worker_num 全部存 etcd, Worker 直接用
4. **不绑定 K8s** — Docker Compose / 物理机 / K8s 同一套代码

### 什么时候 CoreDNS 就够了

- 只需要"服务名 → IP"的简单映射
- 不关心权重、灰度、版本
- 可以接受 30s 级别的下线延迟
- 不想维护额外组件

Thunder 的场景（游戏网关、IoT Broker）需要**秒级路由更新 + 权重分流**, etcd 是必需项, CoreDNS 无法替代。

---

## 完整 etcd 数据流汇总

```
┌────────────────────────────────────────────────────────────────────┐
│                        etcd Key 空间                               │
├──────────────┬──────────┬──────────────────────────────────────────┤
│ /thunder/    │ 感知方式  │ 触发动作                                  │
├──────────────┼──────────┼──────────────────────────────────────────┤
│   registry/  │ Watch    │ → OnWatchEvent → 更新 m_nodeRegistry      │
│              │ (实时)   │ → AssembleAndPushRouteUpdated()          │
│              │          │ → NodeNotice.SerializeAsString()          │
│              │          │ → PushEvent(RouteUpdated)                │
│              │          │ → ev_async → Manager → ShmRingQueue       │
│              │          │ → Worker 更新路由表                        │
├──────────────┼──────────┼──────────────────────────────────────────┤
│   slot/      │ CAS Txn  │ → etcd txn compare_create==0             │
│              │ (注册时)  │ → 分配全局唯一 node_id (1-255)            │
│              │          │ → 与 registry key 原子写入同一 lease       │
├──────────────┼──────────┼──────────────────────────────────────────┤
│   canary/    │ Watch    │ → OnWatchEvent → 更新 m_canaryWeights     │
│              │ (实时)   │ → AssembleAndPushRouteUpdated() 重载权重  │
│              │          │ → 展开 version → ip:port 权重             │
├──────────────┼──────────┼──────────────────────────────────────────┤
│   config/    │ Unary    │ → DoPollConfig() 每 5s 轮询               │
│              │ GET 轮询  │ → 对比 m_lastConfigValue                 │
│              │ (5s)     │ → version 变化 → PushEvent(ConfigUpdated)│
│              │          │ → SO: Manager HTTP GET MinIO             │
│              │          │       → GracefulRestart + dlopen          │
│              │          │ → Lua: ShmRingQueue → ReloadScript()     │
└──────────────┴──────────┴──────────────────────────────────────────┘
```
