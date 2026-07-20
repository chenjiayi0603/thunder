# Thunder 运维内幕 — 连接管理 · 插件 · 性能 · 优化 · S2S 路由

> 原 [`01-architecture-design.md`](01-architecture-design.md) 第 9-13 章 + 附录独立拆分。适合需要运维 Thunder 集群、优化性能、或开发插件的读者。

---

## 9. 连接管理

### 9.1 连接属性

```cpp
struct tagConnectionAttr {
    std::unique_ptr<CBuffer> pRecvBuff;       // 接收缓冲
    std::unique_ptr<CBuffer> pSendBuff;       // 发送缓冲
    std::unique_ptr<CBuffer> pWaitForSendBuff; // 等待发送缓冲（S2S 握手期间）
    std::unique_ptr<CBuffer> pClientData;      // 客户端附加数据（auth token等）
    char   szRemoteAddr[32];                   // 对端 IP
    E_CODEC_TYPE eCodecType;                   // 编解码类型
    int    iFd;                                // 文件描述符
    uint32 ulSeq;                              // FD 序列号（防 ABA）
    uint32 ulForeignSeq;                       // 对端序列号
    std::string strIdentify;                   // 连接标识（如 "logic:192.168.1.1:8080.0"）
    ev_io* pIoWatcher;                         // I/O watcher
    ev_timer* pTimeWatcher;                    // 超时 watcher
    std::string strSessionKey;                 // 会话密钥
    // ... 流量统计字段
};
```

### 9.2 ABA 防护（FD 序列号）

```
问题: fd 被 close 后，新 accept 可能复用同一 fd 值
      旧的 ev_io watcher 回调会误操作新连接

方案: 每个 fd 在创建时分配单调递增的 ulSeq
      IoCallback 中验证 pData->ulSeq == pConn->ulSeq
      不匹配则 DelEvent（丢弃过期回调）

IoCallback:
  if (pData->ulSeq != pConn->ulSeq):
    DelEvent(watcher, pData)  ← 安全丢弃
    return

  IoRead(...):
    // IoRead 内部可能 DestroyConnect，销毁 pData
    auto iter = mapFdAttr.find(iFd);
    if (iter == mapFdAttr.end() || iter->second->ulSeq != ulSeq):
      return  ← 连接已被销毁，安全返回

  IoWrite(...):
    // 同理 re-validate
```

### 9.3 S2S 连接建立流程

```
Node-A Manager                Node-B Manager
      │                              │
      │ connect() ─────────────────► │
      │                              │ accept() → CreateAcceptFdAttr
      │◄────── connected ─────────── │
      │                              │
      │ CMD_REQ_CONNECT_TO_WORKER    │
      │   {worker_idx} ────────────► │
      │                              │ parse worker_idx
      │                              │ send_fd_to_worker(clientFd)
      │                              │
      │ CMD_REQ_TELL_WORKER ◄─────── │
      │   {node_type, identify}      │
      │                              │
      │ Drain waitForSendBuff ──────►│
      │                              │
      │◄══════ normal traffic ══════►│
```

### 9.4 负载均衡（发送策略）

```
SendTo 内部路由选择:

  SendTo(identify)        → 精确路由到指定 identify
  SendToNext(identify, cmd)→ 轮询同一 identify 的多个连接
  SendToNextByMod(uid)    → 按 uid % worker_num 分发（一致性 hash 变体）
  SendToNextByMinLoad()   → 选择负载最小的连接
```

---

## 10. 插件系统

### 10.1 动态加载

```
配置示例:
{
  "so": {
    "CmdLogic": {
      "path": "./libCmdLogic.so",
      "symbol": "CreateCmd"
    }
  },
  "module": {
    "ModuleAuth": {
      "path": "./libModuleAuth.so",
      "symbol": "CreateModule"
    }
  }
}

加载流程:
Worker::LoadSo(conf):
  for each so in conf:
    dlopen(path, RTLD_NOW)
    dlsym(handle, symbol) → CreateCmd function pointer
    Cmd* pCmd = CreateCmd()
    AddCmd(pCmd, cmd_id)  // 注册系统命令

Worker::LoadModule(conf):
  for each module in conf:
    dlopen(path, RTLD_NOW)
    dlsym(handle, symbol) → CreateModule function pointer
    Module* pModule = CreateModule()
    pModule->Init()  // 调用模块初始化
    pModule->Start() // 启动模块
```

### 10.2 Cmd vs Module

| 类型 | 基类 | 用途 | 生命周期 |
|------|------|------|---------|
| `Cmd` | 命令处理器 | 处理特定 `cmd` 的消息分发（`AnyMessage`） | 与进程同生命周期 |
| `Module` | 业务模块 | 独立业务逻辑单元，有完整 Init/Start/Stop 生命周期 | 可热加载/卸载 |

---

## 11. 性能分析

### 11.1 吞吐量关键路径

```
关键路径分析（以单次请求-响应为例）:

1. 网络收包
   epoll_wait → EV_READ                     ~1-10 μs
   ReadFD (recv syscall)                    ~1-5 μs
   Protobuf ParseFromArray                  ~5-20 μs (取决于消息大小)

2. 业务处理
   Step::Callback (逻辑处理)                 ~10-100 μs (业务相关)
   Step::RegisterCallback (新 Step)          ~1-5 μs
   Session 查找 (unordered_map)              ~0.1-1 μs

3. 网络发包
   Protobuf SerializeAsString               ~5-20 μs
   WriteFD (send syscall)                   ~1-5 μs
   Compact (buffer 压缩)                     ~0-50 μs (取决于是否需要 malloc)

4. 全链路预估延迟 (空载): ~30-170 μs per request
```

### 11.2 性能优势设计

| 设计决策 | 性能优势 |
|---------|---------|
| 单线程事件循环 | 无锁竞争，无上下文切换开销 |
| 非阻塞 I/O | 单线程可处理数万连接 |
| CBuffer 紧凑型缓冲 | 减少内存分配，malloc 零拷贝策略 |
| `try-write-first` 模式 | 避免不必要的 epoll_ctl 调用 |
| FD 传递 (SCM_RIGHTS) | Worker 零拷贝接收客户端连接 |
| 共享内存配置同步 | 避免 IPC 消息开销 |
| Protobuf 二进制 | 比 JSON 快 3-10 倍，体积小 3-10 倍 |
| 多进程模型 | 利用多核，进程间故障隔离 |

### 11.3 已知性能瓶颈

| 瓶颈 | 影响 | 现状 |
|------|------|------|
| Protobuf ParseFromArray | 每次消息解析有 heap 分配 | 可考虑 Arena 分配器 |
| CBuffer Compact 中 malloc/free | 高频消息时碎片化 | 可改用内存池 |
| 周期任务轮询共享内存 | 每秒一次，但 O(n) 比较 | 版本号驱动，已优化 |
| 多 Worker 竞争 accept | SO_REUSEPORT 存在惊群 | 已启用 reuseport |
| 事件循环单线程 | CPU 密集任务阻塞 I/O | 可卸载到 ThreadPool |

### 11.4 内存占用估算

```
单 Worker 进程内存:
  - 事件循环: ~1 MB
  - 每连接: ~50-200 KB (CBuffers + ConnectionAttr + codec state)
  - 1000 连接: ~100 MB
  - 10000 连接: ~1 GB
  - Step/Session: 按业务量动态增长
```

---

## 12. 优化建议

### 12.1 短期优化（低风险，高收益）

#### (1) Protobuf Arena 分配器
```
问题: ParseFromArray 内部频繁 new/delete
方案: 使用 google::protobuf::Arena
      Arena arena;
      auto* msg = Arena::CreateMessage<MsgHead>(&arena);
      msg->ParseFromArray(...);
      // 整个 Arena 一次性释放
预估: 减少 30-50% 解析耗时的堆分配
```

#### (2) CBuffer 内存池
```
问题: Compact 在容量不足时 malloc/free
方案: 实现简单的 per-thread buffer pool
      - 预分配 64KB/1MB 的 buffer 池
      - Compact 时从池中取，释放时归还
预估: 高吞吐场景下减少 50% malloc 调用
```

#### (3) Worker 延迟重启退化
```
问题: 当前 sleep(1) 固定等待，不够灵活
方案: 指数退避重启，max 30s，成功上报后重置
```

### 12.2 中期优化（todo.md 规划）

#### (1) DPDK 加速
```
目标: 将网络数据面从内核协议栈迁移到 DPDK
方案: 
  - 单独 DPDK 工作线程做包收发
  - 通过无锁队列与事件循环交互
  - 保留 libev 用于定时器/信号管理
收益: 10x+ 小包吞吐提升，延迟降至 10μs 级
```

#### (2) 并行库引入（tbb / openmp / 线程池）
```
目标: 对 CPU 密集型计算（加密/压缩/序列化）并行化
方案:
  - 评估 tbb::parallel_for 适合数据并行
  - openmp 适合简单的 #pragma omp parallel for
  - 现有线程池适合任务并行
决策建议: 优先用现有线程池，避免引入新的依赖
```

#### (3) Center Web 管理界面
```
目标: 通过 Web 页面管理集群状态
方案:
  - Center 内嵌 HTTP server
  - 展示: 节点列表、Raft 状态、路由表、节点负载
  - 操作: 节点启停、配置下发
```

#### (4) Center 只发主节点路由同步优化
```
问题: 当前 NodeReportRsp 包含全量路由快照
方案: 增量同步 - 只下发变更的节点路由
      Manager 本地合并增量到全量快照
收益: 减少大集群下的网络带宽
```

### 12.3 长期规划

#### (1) 多线程事件循环（one loop per thread）
```
当前: 每个进程一个事件循环
优化: 每个 Worker 内 N 个事件循环线程
      + 每个线程独立的 epoll fd
      + SO_REUSEPORT 分发连接
      + 共享 Step/Session 通过 PostToEventLoop 跨线程通信
挑战: Session 跨线程共享需要更精细的锁策略
```

#### (2) 协程调度器优化
```
当前: 协程在 StepCo20 中管理，每个协程独立
优化: 统一的协程调度器
      - 协程池 (coroutine pool) 复用协程帧
      - Work-stealing 调度
      - I/O 多路复用与协程调度融合
```

#### (3) 零拷贝网络栈
```
sendfile / splice / io_uring:
  - 静态文件服务用 sendfile
  - 大消息转发用 splice
  - io_uring 替代 epoll (Linux 5.1+)
```

---

## 附录 A: 关键配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `worker_num` | CPU 核数 | Worker 进程数量 |
| `node_type` | - | 节点类型 (0=center, 1=logic, 2=interface, 3=data) |
| `node_beat` | 3.0 | 心跳间隔 (秒) |
| `center_beat` | 2.0 | Center 心跳间隔 |
| `worker_beat` | - | Worker 心跳超时 (2×beat_interval + 1) |
| `centers` | - | Center 地址列表 (JSON 数组或逗号分隔) |
| `loader_process` | false | 是否启用 Loader 进程 |
| `custom` | {} | 自定义配置 (通过 Center Web 下发) |
| `log_level` | INFO | 日志级别 |
| `so` | {} | 动态库配置 |
| `module` | {} | 业务模块配置 |

## 附录 B: 关键命令码

| 命令码 | 方向 | 说明 |
|--------|------|------|
| `CMD_REQ_NODE_REGISTER` | Manager→Center | 节点注册 |
| `CMD_RSP_NODE_REGISTER` | Center→Manager | 注册响应 (含路由快照) |
| `CMD_REQ_NODE_STATUS_REPORT` | Manager→Center | 节点状态上报 |
| `CMD_RSP_NODE_STATUS_REPORT` | Center→Manager | 状态上报响应 |
| `CMD_REQ_UPDATE_WORKER_LOAD` | Worker→Manager | Worker 负载上报 |
| `CMD_REQ_REFRESH_NODE_ID` | Manager→Worker | node_id 更新通知 |
| `CMD_REQ_SET_NODE_CUSTOM_CONFIG` | Center→Manager | 自定义配置下发 |
| `CMD_REQ_CONNECT_TO_WORKER` | Manager→Manager | S2S 请求连接指定 Worker |
| `CMD_REQ_TELL_WORKER` | Manager→Manager | S2S 连接建立完成通知 |
| `CMD_REQ_BEAT` | - | 心跳 |
| `CMD_REQ_NODE_STOP` | Center→Node | 节点停机 |
| `CMD_REQ_NODE_RESTART_WORKERS` | Center→Node | 重启所有 Worker |
| `CMD_REQ_RELOAD_LOGIC_CONFIG` | - | 热加载逻辑配置 |
| `CMD_REQ_SERVER_CONFIG` | - | 服务器配置更新 |

## 附录 C: 文件组织

```
code/
├── Net/                    ← 框架核心
│   ├── include/            ← 头文件
│   │   ├── labor/          ← 进程模型 (Labor/Manager/Worker/Loader)
│   │   ├── cmd/            ← 命令处理器
│   │   ├── codec/          ← 编解码器 (10种协议)
│   │   ├── coro/           ← C++20 协程基础设施
│   │   ├── step/           ← 异步步骤/状态机
│   │   ├── session/        ← 会话管理
│   │   ├── storage/        ← 存储 (Redis/MySQL/MongoDB)
│   │   └── protocol/       ← Protobuf 协议定义
│   └── src/labor/          ← 核心实现
├── Center/                 ← 中心节点实现 (Raft)
├── Util/                   ← 通用工具库
│   └── src/
│       ├── util/           ← CBuffer, CJsonObject, StreamCodec
│       ├── thread/         ← 线程池
│       └── ...
├── PlugIn/                 ← 插件 (.so)
├── cmake/                  ← CMake 模块
├── deploy/                 ← 部署配置
├── docs/                   ← 文档
└── tests/                  ← 测试
```

---

## 附录 D: 文档结构总览

| 章节 | 内容 |
|------|------|
| 1. 项目概述 | 技术栈表格、4 种节点类型定义 |
| 2. 整体架构 | ASCII 架构全景图（Cluster→Node→Process 三层）+ 消息流转图 |
| 3. 进程模型与 IPC | 进程树、Manager↔Worker socketpair 通信、FD 传递流程、生命周期管理（CheckWorker/RestartWorker/CheckParent） |
| 4. 事件循环与并发模型 | libev epoll 事件循环结构、单线程模型、PostToEventLoop 跨线程通信机制、全局线程池、并发模型全景图、6 种安全性保证策略 |
| 5. C++20 协程系统 | 协程架构图、AsyncTask/Awaitable/StepCo20 核心类型、协程执行 4 步流程、协程与 Step 状态机对比、IoBoolAwaitable 实现细节 |
| 6. 网络 I/O 与编解码 | 读数据完整流程（epoll→codec→Dispose→Encode→WriteFD）、10 种编解码器继承树、发送 try-write-first 优化、压缩加密管道、CBuffer 环形缓冲区设计 |
| 7. Raft 共识与分布式 | 状态机图、关键时间参数（Follower Lease / Candidate Retry / 冷启动 jitter）、消息流图、Node ID 环形合并算法、Leader 缓存与故障切换 |
| 8. 共享内存路由/配置同步 | 三块 SHM 结构体详细字段、写入协议（blob→len→version++ double-snapshot）、Worker 定时轮询 CheckShareMem 4 条分支、完整版本通知路径图 |
| 9. 连接管理 | ConnectionAttr 结构体、FD 序列号 ABA 防护、S2S 连接建立流程、负载均衡策略 |
| 10. 插件系统 | .so 动态加载流程、Cmd vs Module 对比 |
| 11. 性能分析 | 关键路径延迟估算（30-170μs）、5 项性能优势设计、5 个已知瓶颈、单 Worker 内存估算 |
| 12. 优化建议 | 短期（Arena/内存池/退避重启）、中期（DPDK/并行库/Center Web/增量路由）、长期（多线程事件循环/协程调度器/零拷贝） |

---

> 本文档基于 Thunder 项目源码分析生成，版本对应 dev 分支 commit cc1fe18。

---

## 13. 连接监听与 S2S 路由 — 完整链路

### 13.1 每个节点两个端口

```
每个 Thunder 节点监听两个端口:

  inner_port (16068):    Server 间通信 (S2S)
                        · Logic/Interface 互连
                        · Center 的 Raft 心跳
                        · 编解码: CODEC_PB_INTERNAL (ProtoCodec)

  access_port (27006):   客户端接入 (C2S)
                        · HTTP/HTTPS/WebSocket
                        · 编解码: CODEC_HTTP/HTTPS/WS
```

配置:
```json
{
  "inner_host": "127.0.0.1",
  "inner_port": 16068,
  "access_host": "127.0.0.1", 
  "access_port": 27006,
  "access_codec": 1
}
```

### 13.2 节点注册流程

```
                 Manager                          Center
                    │                                │
  Step 1: 启动      │  CMD_REQ_NODE_REGISTER          │
                    │  (node_type, node_ip, node_port) │
                    │ ──────────────────────────────→ │
                    │                                  │ 写入 SessionOnlineNodes
                    │  CMD_RSP_NODE_REGISTER           │
                    │  (node_id + route_snapshot)      │
                    │ ←────────────────────────────── │
                    │                                  │
  Step 2: Manager   │  OnCenterEvent():                │
          收到响应  │    node_id → SendToWorker        │
                    │    route_snapshot → shm (共享内存)│
                    │                                  │
  Step 3: Worker   │  CheckShareMem():                 │
          定时轮询  │    version 变化?                  │
                    │    是 → 读 NodeNotice             │
                    │         → 更新路由表              │
```

### 13.3 路由表同步机制

```
            Manager                  Worker
               │                        │
  Center → route_snapshot               │
               │                        │
               ▼                        │
  ┌─────────────────────────┐           │
  │  共享内存 (shm)          │           │
  │  ┌───────────────────┐  │           │
  │  │ blob (protobuf)    │  │  ev_idle│
  │  │ len                │  │  回调   │
  │  │ version++          │  │    │    │
  │  └───────────────────┘  │    │    │
  └──────────┬──────────────┘    │    │
             │                   ▼    │
             │    CheckShareMem()     │
             │    比较 version        │
             │    version 不同?       │
             │    → 读共享内存        │
             │    → 解析 NodeNotice   │
             │    → 更新 mapNodeId    │
```

原子性保证:
- Manager 先写 blob, 再写 len, 最后 version++ (原子递增)
- Worker 先读 version, 再读 len, 再读 blob
- 如果中途 Manager 在写, Worker 读到旧 version → 下次重试

### 13.4 S2S 连接 — Interface 如何找到 Logic

```
Interface Worker 收到 HTTP 请求:
  POST /Interface/gentoken {"option":"GenKey"}

  ModuleInterface::AnyMessage():
    1. 解析 JSON → option=GenKey
    2. 查路由表: GetNodeIdentify("LOGIC") → "127.0.0.1:16068.0"
    3. 检查是否已连接: m_mapInnerFd 里有 127.0.0.1:16068 的连接吗?
       没有 → AutoConnect("127.0.0.1:16068.0")
            → socket() + connect() + CreateFdAttr(iFd, CODEC_PB_INTERNAL)
            → 注册到 mapFdAttr, 加 libev EV_READ 监听
       有 → 直接用已有 fd

    4. SendTo(identify, CMD_REQ_GEN_KEY, seq, body)
       → ProtoCodec::Encode() → WriteFD → 发到 Logic

  Logic Worker 收到:
    RecvDataAndDispose → ProtoCodec::Decode → Dispose(MsgHead)
    → mapSo[CMD_REQ_GEN_KEY] → CmdGetToken::AnyMessage()
    → 生成 token+key → SendToClient → 发回 Interface

  Interface 收到响应:
    Dispose(MsgHead) → mapCallbackStep.find(seq)
    → Step::Callback() → 协程恢复
    → 构造 HTTP 200 JSON → SendToClient
```

### 13.5 连接管理

```
Worker 维护两套连接表:

  mapFdAttr (所有连接):
    key = fd, value = tagConnectionAttr (recv/send buffer, codec, seq)
    用途: IO 事件到达时找到对应的连接属性

  m_mapInnerFd (S2S 内部连接):
    用途: 判断 "identify" 是否已有 TCP 连接
          避免重复 connect

  连接类型判断:
    pConn->eCodecType == CODEC_PB_INTERNAL  → 内部 S2S (心跳保活, 不超时断开)
    否则                                      → 外部客户端 (超时回收)

  tagMsgShell:
    {iFd, ulSeq} — 连接的"地址"
    seq 是防重用的: fd 关闭后可能被新连接复用同一个 fd 号
    seq 不匹配 → 丢弃旧事件
```

### 13.6 心跳与 Keepalive

```
内部连接 (CODEC_PB_INTERNAL):
  dKeepAlive == 0 (长连接)
  → 定时发送 CMD_REQ_BEAT
  → 收不到响应 → CheckHeartBeat 标记断线
  → 超时 → DestroyConnect

外部连接 (客户端):
  dKeepAlive > 0
  → 超时未活动 → 回收连接
  → dActiveTime 每次 IO 刷新
```

### 13.7 跟 CLB 的路由对比

```
CLB 路由:
  请求 → 查后端表 → 选一个 → 转发
  路由表: hash 表, O(1)
  后端发现: 配置中心推送 (etcd/consul)
  连接: 连接池复用

Thunder 路由:
  请求 → 查路由表 → AutoConnect → 转发
  路由表: shm (Manager 写, Worker 读)
  节点发现: Center Raft 集群
  连接: S2S 长连接 + 心跳保活

核心区别:
  CLB 不做业务逻辑 (只转发)
  Thunder S2S 是业务调用 (Interface → Logic 执行 GenKey)
```



---

> 📖 上一篇: [21 数据面](21-data-plane.md)  
> 📖 回到全景: [00 架构全景](00-overview.md)
