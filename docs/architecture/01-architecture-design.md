# Thunder 集群异步服务框架 — 架构设计文档

> 版本: 1.0 | 日期: 2026-05-09 | 作者: cjy

---

## 目录

1. [项目概述](#1-项目概述)
2. [整体架构](#2-整体架构)
3. [进程模型与 IPC](#3-进程模型与-ipc)
4. [事件循环与并发模型](#4-事件循环与并发模型)
5. [C++20 协程系统](#5-c20-协程系统)
6. [网络 I/O 与编解码管道](#6-网络-io-与编解码管道)
7. [Raft 共识与分布式协调](#7-raft-共识与分布式协调)
8. [共享内存路由/配置同步](#8-共享内存路由配置同步)
9. [连接管理](#9-连接管理)
10. [插件系统](#10-插件系统)
11. [性能分析](#11-性能分析)
12. [优化建议](#12-优化建议)

---

## 1. 项目概述

Thunder 是一个 **C++20 分布式异步集群服务框架**，面向游戏服务器、即时通讯等高并发低延迟场景。支持多进程部署、多节点组网，内置 Raft 共识协议、C++20 协程、插件化扩展体系。

### 1.1 技术栈

| 层次 | 技术选型 |
|------|---------|
| 语言 | C++20 (coroutines, std::atomic) |
| 事件循环 | libev 4.33 (epoll 后端) |
| 序列化 | Protobuf (MsgHead + MsgBody 二进制协议) |
| 共识 | 自研 Raft (term-based, AppendEntries heartbeat) |
| 数据库 | MariaDB (异步)、MongoDB、Redis/RedisCluster (hiredis-vip 异步) |
| 压缩加密 | zlib + RC5 + AES-128/256 |
| 构建 | CMake 3.10+ |
| 部署 | 物理机 / K3s 容器 |

### 1.2 节点类型

| 类型 | 枚举值 | 职责 |
|------|--------|------|
| `NODE_TYPE_CENTER` | 0 | 集群中心：Raft 主选举、节点注册、路由镜像分发、node_id 分配 |
| `NODE_TYPE_LOGIC` | 1 | 逻辑服：业务逻辑处理 |
| `NODE_TYPE_INTERFACE` | 2 | 接入网关：客户端连接、协议解析、消息转发 |
| `NODE_TYPE_DATA` | 3 | 数据服：数据库代理 |

---

## 2. 整体架构

### 2.1 架构全景图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Cluster Level                                  │
│                                                                          │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐               │
│   │  Center-1 ★  │◄──►│  Center-2    │◄──►│  Center-3    │  Raft Cluster │
│   │  (LEADER)    │    │  (FOLLOWER)  │    │  (FOLLOWER)  │               │
│   └──────┬───────┘    └──────────────┘    └──────────────┘               │
│          │ register/report                                                │
│          │ + route snapshot                                               │
│          │ + custom config                                                │
│          ▼                                                                │
│   ┌─────────────────────────────────────────────────────┐                │
│   │              Business Node (e.g. Logic)              │                │
│   │                                                      │                │
│   │  ┌─────────────────────────────────────┐             │                │
│   │  │         Manager Process             │             │                │
│   │  │  • ReportToCenter (periodic)        │             │                │
│   │  │  • Route/Config → Shared Memory     │             │                │
│   │  │  • Worker lifecycle (fork/monitor)  │             │                │
│   │  │  • S2S listen + connect             │             │                │
│   │  └───┬──────────────┬──────────────────┘             │                │
│   │      │socketpair    │socketpair                       │                │
│   │      ▼              ▼                                 │                │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐            │                │
│   │  │ Worker-0 │  │ Worker-1 │  │ Worker-N │  ...       │                │
│   │  │ epoll    │  │ epoll    │  │ epoll    │            │                │
│   │  │ codec    │  │ codec    │  │ codec    │            │                │
│   │  │ steps    │  │ steps    │  │ steps    │            │                │
│   │  │ sessions │  │ sessions │  │ sessions │            │                │
│   │  └──────────┘  └──────────┘  └──────────┘            │                │
│   │                                                      │                │
│   │  ┌──────────┐                                       │                │
│   │  │  Loader  │  (optional config manager)             │                │
│   │  │ fork     │                                       │                │
│   │  └──────────┘                                       │                │
│   └─────────────────────────────────────────────────────┘                │
│                                                                          │
│   ┌──────────────────────────────┐                                       │
│   │     Interface Nodes          │  ← 客户端连接入口                      │
│   │  (same Manager/Worker arch)  │                                       │
│   └──────────────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 消息流转全景图

```
                         Client
                           │
                    ┌──────▼──────┐
                    │  Interface  │  gateway node
                    │  Worker     │  codec: WS/HTTP/Protobuf
                    └──────┬──────┘
                           │ internal Thunder protocol (MsgHead + MsgBody)
                    ┌──────▼──────┐
                    │   Logic     │  business node
                    │   Worker    │  Step state machine / C++20 coroutine
                    └──┬──────┬──┘
                       │      │
              ┌────────▼─┐  ┌─▼────────┐
              │   Data    │  │  Other   │
              │  (Redis/  │  │  Logic   │
              │   MySQL)  │  │  Nodes   │
              └───────────┘  └──────────┘
```

---

## 3. 进程模型与 IPC

### 3.1 进程树

```
main()
 ├─ Manager (父进程)
 │   ├─ Loader  (fork, optional)      ← 配置文件拉取/管理
 │   ├─ Worker-0 (fork + socketpair)  ← 业务处理
 │   ├─ Worker-1 (fork + socketpair)
 │   └─ Worker-N (fork + socketpair)
```

- **Manager**：单进程，管理整个节点的生命周期。负责与 Center 通信、Worker 进程管理、S2S 连接监听。
- **Loader**：可选进程（配置项 `loader_process`），负责从远端拉取配置文件写入共享内存。默认关闭。
- **Worker**：多进程（默认 CPU 核数），负责实际业务逻辑。每个 Worker 独立 epoll 事件循环，互不干扰。

### 3.2 Manager ↔ Worker IPC

每个 Worker 与 Manager 之间通过两对 **Unix domain socketpair** 通信：

| 通道 | 方向 | 用途 |
|------|------|------|
| `iControlFd` | 双向 | 控制消息：Worker 负载上报（`CMD_REQ_UPDATE_WORKER_LOAD`）、node_id 更新通知 |
| `iDataFd` | Manager→Worker | **文件描述符传递**（`sendmsg` + `SCM_RIGHTS`）：Manager 将 accept 的客户端 fd 传给 Worker |

```
Manager                      Worker
  │                            │
  │ socketpair(AF_UNIX)        │
  │◄──── control fd ──────────►│  heartbeat, load report, cmd dispatch
  │                            │
  │ socketpair(AF_UNIX)        │
  │───── data fd (fd pass) ───►│  client fds via SCM_RIGHTS
```

#### FD 传递流程

```
Manager IoRead:
  if fd == S2SListenFd → AcceptServerConn (other nodes / centers)
  else if fd from Center → DisposeDataFromCenter
  else → DisposeDataAndTransferFd:
     1. Parse ConnectWorkerReq protobuf
     2. accept() client connection
     3. send_fd_with_attr(dataFd, clientFd, remoteAddr, codecType) → Worker
     4. Worker FdTransfer() → recv_fd_with_attr → CreateAcceptFdAttr

Worker IoRead:
  if reuseport && fd == C2SListenFd → AcceptClientConn (direct accept)
  else if fd == iManagerDataFd → FdTransfer (receive fd from Manager)
  else → RecvDataAndDispose (normal I/O)
```

### 3.3 进程生命周期管理

```
Manager::IdleCallback / PeriodicTaskCallback:
  CheckWorker():
    for each worker:
      if now - lastBeat > m_iWorkerBeat (2×beat_interval + 1):
        kill(pid, SIGKILL)    // 超时未心跳则强杀
        RestartWorker(pid):
          1. close old socketpairs
          2. create new socketpairs
          3. fork new Worker (sleep 1s 防过快重启)
          4. ev_loop_fork(m_loop)  ← libev 事件循环 fork 修复
          5. pass shm pointers (LoaderConfig, RouteNotice, CustomConfig)

Worker::CheckParent():
  if getppid() == 1:  // Manager 已死
    exit(0)            // 孤儿进程退出
  SendToParent: CMD_REQ_UPDATE_WORKER_LOAD (负载上报)
```

---

## 4. 事件循环与并发模型

### 4.1 libev 事件循环

```
┌──────────────────────────────────────────────┐
│             libev Event Loop                 │
│  ev_loop_new(EVFLAG_FORKCHECK |              │
│              EVFLAG_SIGNALFD |               │
│              EVBACKEND_EPOLL)                │
│                                              │
│  ┌─────────┐ ┌──────────┐ ┌──────────────┐  │
│  │ ev_io   │ │ ev_timer │ │ ev_signal    │  │
│  │ (网络IO) │ │ (定时器)  │ │ (信号处理)   │  │
│  └─────────┘ └──────────┘ └──────────────┘  │
│  ┌─────────┐ ┌──────────┐                   │
│  │ ev_idle │ │ ev_async │                   │
│  │ (空闲)   │ │ (跨线程)  │                   │
│  └─────────┘ └──────────┘                   │
└──────────────────────────────────────────────┘
```

### 4.2 单线程事件循环模型（每个进程）

核心原则：**每个进程只有一个 libev 事件循环线程，所有 I/O 在该线程中非阻塞执行。**

```
ev_run(m_loop, 0)  ← 主循环永不退出

事件回调执行链（均在主线程）:
  PeriodicTaskCallback (NODE_BEAT seconds)
    ├─ ReportToCenter()
    ├─ CheckWorker()
    └─ RefreshServer()

  IdleCallback (空闲时)
    ├─ CheckWorker()
    └─ ReportToCenter()

  IoCallback (EV_READ|EV_WRITE|EV_ERROR)
    ├─ IoRead  → RecvDataAndDispose → codec::Decode → Dispose(Step/Cmd)
    ├─ IoWrite → send buffered data
    └─ IoError → DestroyConnect

  StepTimeoutCallback
    └─ Step::Timeout() → 业务超时处理

  SessionTimeoutCallback
    └─ Session::Timeout() → 会话超时清理
```

### 4.3 跨线程通信：PostToEventLoop

```
┌──────────────┐     ev_async      ┌──────────────┐
│  Other Thread │ ──── wakeup ────►│  Event Loop  │
│  (threadpool) │                  │  Thread      │
└──────┬───────┘                   └──────┬───────┘
       │                                  │
       │ PostToEventLoop(task)            │ AsyncCallback()
       │  mutex_.lock()                   │  mutex_.lock()
       │  pending_queue_.push(task)       │  while(!queue.empty())
       │  ev_async_send(wakeup_)          │    task()
       │  mutex_.unlock()                 │  mutex_.unlock()
```

关键数据结构（`Labor.hpp`）：
```cpp
std::mutex m_oPendingMutex;
std::deque<std::function<void()>> m_oPendingTask;
ev_async* m_pPendingAsyncWatcher;
```

- 其他线程（线程池）通过 `PostToEventLoop()` 将任务投递到事件循环线程执行
- 使用 `ev_async_send` 唤醒事件循环
- 线程安全由 mutex 保证

### 4.4 全局线程池（辅助）

```
Worker 进程内全局 std::threadpool
  ├─ InitThunderWorkerThreadPool(n)  // 幂等初始化，上限 16 线程
  ├─ ThunderWorkerThreadPool()       // 懒初始化（默认 4 线程）
  └─ commit(f, args...) → std::future  // 提交任务，获取返回值
```

线程池实现（`Util/src/thread/threadpool.h`）：
- 基于 `std::queue<Task>` + `std::mutex` + `std::condition_variable`
- 工作线程循环 wait → pop → execute
- 支持 `std::future` 获取返回值
- 可选自动扩容（`THREADPOOL_AUTO_GROW`，默认关闭）

使用场景：
- 协程卸载 CPU 密集型任务
- 数据库连接池管理
- 辅助计算（不涉及框架核心 I/O）

### 4.5 并发模型图

```
                         Manager Process
                    ┌─────────────────────┐
                    │   Event Loop Thread │
                    │   (libev epoll)     │
                    │                     │
                    │  ┌───────────────┐  │
                    │  │ S2S Listen FD │  │
                    │  │ Center Conn   │  │
                    │  │ Worker Ctrl   │  │
                    │  │ Worker Data   │  │
                    │  │ Signals       │  │
                    │  └───────────────┘  │
                    └─────────────────────┘

  Worker-0 Process            Worker-1 Process            Worker-N Process
┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐
│ Event Loop Thread   │  │ Event Loop Thread   │  │ Event Loop Thread   │
│ (libev epoll)       │  │ (libev epoll)       │  │ (libev epoll)       │
│                     │  │                     │  │                     │
│ ┌───────────────┐   │  │ ┌───────────────┐   │  │ ┌───────────────┐   │
│ │ Client FDs    │   │  │ │ Client FDs    │   │  │ │ Client FDs    │   │
│ │ Parent Ctrl   │   │  │ │ Parent Ctrl   │   │  │ │ Parent Ctrl   │   │
│ │ Parent Data   │   │  │ │ Parent Data   │   │  │ │ Parent Data   │   │
│ │ S2S Conns     │   │  │ │ S2S Conns     │   │  │ │ S2S Conns     │   │
│ │ Redis/MySQL   │   │  │ │ Redis/MySQL   │   │  │ │ Redis/MySQL   │   │
│ └───────────────┘   │  │ └───────────────┘   │  │ └───────────────┘   │
│                     │  │                     │  │                     │
│ ┌───────────────┐   │  │ ┌───────────────┐   │  │ ┌───────────────┐   │
│ │ ThreadPool    │   │  │ │ ThreadPool    │   │  │ │ ThreadPool    │   │
│ │ (4-16 threads)│   │  │ │ (4-16 threads)│   │  │ │ (4-16 threads)│   │
│ └──────┬────────┘   │  │ └──────┬────────┘   │  │ └──────┬────────┘   │
│        │PostToEL    │  │        │PostToEL    │  │        │PostToEL    │
│        ▼            │  │        ▼            │  │        ▼            │
│   ev_async wakeup   │  │   ev_async wakeup   │  │   ev_async wakeup   │
└─────────────────────┘  └─────────────────────┘  └─────────────────────┘
          │                        │                        │
          └────────────────────────┼────────────────────────┘
                                   │
                          Shared Memory
                   ┌─────────────────────────┐
                   │ LoaderConfigVersionMM    │  seq_config + config body
                   │ RouteNoticeVersionMM     │  seq + node_notice_blob
                   │ CustomConfigVersionMM    │  seq + custom_blob
                   └─────────────────────────┘
```

### 4.6 并发安全性总结

| 场景 | 策略 | 安全性保证 |
|------|------|-----------|
| 事件循环内部 | 单线程，无竞态 | 无需锁 |
| 线程池 → 事件循环 | PostToEventLoop + mutex | mutex 保护队列 |
| Manager ↔ Worker IPC | socketpair 内核缓冲区 | OS 保证原子性 |
| 进程间共享内存 | atomic + snapshot CRC | 版本号 + double-snapshot 读取 |
| 多处 SendTo 同一连接 | CBuffer 单写线程 | 事件循环单线程模型天然保证 |
| FD 序列号 ABA | ulSeq 单调递增 | 指针匹配失败即丢弃过期回调 |

---

## 5. C++20 协程系统

### 5.1 协程架构

```
                      StepCo20 (state machine base)
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        HttpGetAsync  SendToAsync  CoSleepAsync
              │            │            │
              ▼            ▼            ▼
        Awaitable<T>  IoBoolAwaitable  CoSleepAwaiter
              │            │            │
              └────────────┼────────────┘
                           │
                    co_await 暂停协程
                           │
                    异步操作完成
                           │
                    handle.resume() 恢复
```

### 5.2 核心类型

```
AsyncTask (Coroutine20.hpp)
  ├─ promise_type
  │   ├─ initial_suspend() → suspend_never  // 立即开始执行
  │   ├─ final_suspend() → suspend_always   // 保留协程帧
  │   └─ return_void() → NotifyEmitCoroutineSuccess
  └─ 只能移动，不可复制

StepCo20 (StepCo20.hpp)  继承自 Step
  ├─ StepAsync() → AsyncTask (纯虚函数，业务实现)
  ├─ IoBoolAwaitable    // I/O 操作等待器
  ├─ SendToAsync        // 异步发送 + 等待回调
  ├─ HttpGetAsync/HttpPostAsync
  └─ CoSleepAwaiter     // 协程内延时

Awaitable<T> (Awaitable.hpp)
  ├─ await_ready() → bool
  ├─ await_suspend(handle) → 保存句柄，启动异步操作
  ├─ await_resume() → T  // 获取结果
  └─ SetResult(T) / SetException()
```

### 5.3 协程执行流程

```
1. StepCo20::Emit()
   └─ AsyncTask task = StepAsync()     // 调用业务协程
      └─ promise_type::initial_suspend → suspend_never
         → 协程体开始执行

2. 协程体内遇到 co_await SomeAsyncOp()
   └─ await_ready() → false
   └─ await_suspend(handle)
      └─ 保存 coroutine_handle
      └─ 发起异步操作（如 SendTo、HTTP GET）
   └─ 协程挂起，控制权返回事件循环

3. 异步操作完成（网络回包到达）
   └─ handle.resume()
   └─ await_resume() → 获取结果
   └─ 协程继续执行

4. 协程体执行完毕
   └─ promise_type::return_void()
      └─ NotifyEmitCoroutineSuccess(step)
   └─ final_suspend → suspend_always
      └─ 协程帧保留，等待清理
```

### 5.4 协程与步骤（Step）的关系

```
传统 Step 模式（状态机）:           C++20 协程模式:
  StepA::Emit() →                    co_await SendToAsync(...)
    StepA::Callback() →              co_await HttpGetAsync(...)
      new StepB →                    co_await SendToAsync(...)
        StepB::Emit() →              // 线性代码，无需回调嵌套
          StepB::Callback() →
            ...
```

协程是 Step 的现代替代方案：
- **Step 模式**: 显式状态机，`Emit() → Callback() → new NextStep()`，适合复杂状态转换
- **协程模式**: 线性代码编写，`co_await` 表达异步等待，适合顺序异步流程

两者在框架中共存，可混用。

### 5.5 IoBoolAwaitable 实现细节

```cpp
// StepCo20.hpp 中的 IoBoolAwaitable
struct IoBoolAwaitable {
    bool await_ready() const noexcept { return false; }  // 永远挂起
    void await_suspend(std::coroutine_handle<> h) noexcept {
        // 保存协程句柄，由 I/O 回调恢复
        pStep_->CoroutineAttachPendingAwaitable(h, this);
    }
    bool await_resume() noexcept {
        return pStep_->CoroutineGetAwaitableBoolResult();
    }
};
```

---

## 6. 网络 I/O 与编解码管道

### 6.1 读数据流程

```
epoll_wait → EV_READ
  │
  ▼
IoCallback → IoRead → RecvDataAndDispose
  │
  ├─ pRecvBuff->Compact(8192)      ← 释放已读空间
  ├─ pRecvBuff->ReadFD(fd, err)    ← 非阻塞 read
  │
  ▼
  while (ReadableBytes >= MsgHeadSize):
    ├─ codec->Decode(pConn, oMsgHead, oMsgBody)
    │   ├─ CODEC_STATUS_OK:
    │   │   ├─ Protocol message (cmd > 0) → Dispose(Step/Cmd)
    │   │   └─ HTTP/WS message → Dispose(HttpMsg)
    │   │       └─ Encode response → pSendBuff->WriteFD → Compact(8192)
    │   ├─ CODEC_STATUS_PAUSE: break  ← 数据不完整，等下次 EV_READ
    │   └─ CODEC_STATUS_ERR: DestroyConnect
    │
    ▼
  return true;

错误处理:
  read == 0        → 对端关闭 → DestroyConnect
  errno == EAGAIN  → 正常（非阻塞读空）
  errno == EINTR   → goto read_again（被信号中断，重试）
  errno == other   → DestroyConnect
```

### 6.2 编解码器体系

```
ThunderCodec (抽象基类)
  ├─ Encode(MsgHead, MsgBody) → CBuffer
  ├─ Decode(CBuffer) → MsgHead, MsgBody
  ├─ Decode(ConnectionAttr) → MsgHead, MsgBody  // 连接感知解码
  │
  ├─ ProtoCodec          ← S2S / 内部通信（MsgHead+MsgBody 二进制）
  ├─ HttpCodec           ← HTTP 请求/响应解析
  ├─ HttpsCodec          ← HTTPS（OpenSSL 握手 + HTTP）
  ├─ CodecWebSocketJson  ← WebSocket JSON 帧
  ├─ CodecWebSocketPb    ← WebSocket Protobuf 帧
  ├─ CodecWebSocketPbApp ← WebSocket Protobuf App 帧（带用户会话）
  ├─ ClientMsgCodec      ← 客户端私有协议
  ├─ AppMsgCodec         ← 应用层协议（带 auth verify）
  └─ CodecCustom         ← 自定义编解码器扩展
```

### 6.3 发送数据流程

```
SendTo(msgShell, MsgHead, MsgBody):
  1. 查找连接 fd
  2. pSendBuff->Write(MsgHead.SerializeAsString())
  3. pSendBuff->Write(MsgBody.SerializeAsString())
  4. pSendBuff->WriteFD(fd, err)     ← 立即尝试 write
  5. pSendBuff->Compact(8192)        ← 压缩缓冲区
  6. if EAGAIN: RefreshEvent(EV_WRITE)  ← 注册写事件
     if error: DestroyConnect
```

**关键优化**: 先尝试直接 write，失败才注册 EV_WRITE 事件。这种 "try-write-first" 模式避免了不必要的 epoll_ctl 系统调用。

### 6.4 压缩/加密管道

```
ThunderCodec 支持透明的压缩和加密：

编码路径:
  MsgBody.body  → [可选: Zip/Gzip 压缩] → [可选: RC5/AES 加密] → 组包 → CBuffer

解码路径:
  CBuffer → 拆包 → [可选: AES/RC5 解密] → [可选: Gunzip/Unzip 解压] → MsgBody.body

控制位（MsgHead.cmd）:
  gc_uiGzipBit (0x10000000)  ← gzip 压缩
  gc_uiZipBit  (0x20000000)  ← zip 压缩
  gc_uiRc5Bit  (0x01000000)  ← RC5 加密
  gc_uiAesBit  (0x02000000)  ← AES-128 加密
```

### 6.5 CBuffer 缓冲区设计

```
       +-------------------+------------------+------------------+
       | readed bytes      |  readable bytes  |  writable bytes  |
       +-------------------+------------------+------------------+
       |                   |                  |                  |
       0      <=      readerIndex   <=   writerIndex    <=    capacity
```

| 操作 | 说明 |
|------|------|
| `Compact(8192)` | 当可写空间不足时，释放已读空间。若空闲仍不足，malloc 新 buffer |
| 扩容策略 | 容量不足时 ×2 扩容（`newCapacity <<= 1`） |
| `BUFFER_MAX_READ` | 8192 字节，单次最多读取量 |
| `DEFAULT_BUFFER_SIZE` | 32 字节初始容量 |

---

## 7. Raft 共识与分布式协调

### 7.1 Raft 状态机

```
                    timeout, start election
     ┌──────────┐ ──────────────────────────► ┌───────────┐
     │ Follower │                             │ Candidate │
     │          │ ◄────────────────────────── │           │
     └────┬─────┘  discover current leader    └─────┬─────┘
          │         or higher term                  │
          │                                         │ receives votes from
          │ AppendEntries from leader               │ majority of servers
          ▼                                         ▼
     ┌──────────┐                             ┌──────────┐
     │ Follower │ ◄───────────────────────────│  Leader  │
     │ (normal) │    AppendEntries heartbeat  │          │
     └──────────┘                             └──────────┘
```

### 7.2 关键时间参数（SessionRaftCluster.cpp）

```
Follower Lease:
  base = center_beat × mult  (mult 从配置读取，默认值取决于同数据中心/跨数据中心)
  extra = 1.0 + U(0,1) × 0.5
  lease = base + extra

Candidate 选举重试:
  retry = 0.08 + U(0,1) × 0.12  (80ms~200ms random jitter)

冷启动随机延迟:
  delay = 0.20 + U(0,1) × 0.30  (200ms~500ms，避免同时启动多个选举)

心跳间隔:
  center_beat (配置项，通常 1~3 秒)
```

### 7.3 Raft 消息流

```
                              Leader                           Follower
                                │                                 │
  RaftTick (periodic timer)     │                                 │
    Leader: skip (no action)    │                                 │
    Follower: check lease       │                                 │
      if expired:               │                                 │
        RaftStartElection()     │  ── RequestVote ──────────────► │
        term++, votedFor=self   │  ◄── RequestVoteRsp ─────────── │
                                │                                 │
  RaftSendAppendEntriesToAll()  │  ── AppendEntries ────────────► │
    (Leader periodic)           │     {term, leaderId,            │
    node_id cursor merge        │      node_id_alloc_cursor,      │
    online node snapshot        │      prevLogIndex, prevLogTerm, │
                                │      entries[], leaderCommit}   │
                                │                                 │
                                │  ◄── AppendEntriesRsp ─────────  │
                                │     {term, success,             │
                                │      matchIndex}                │
```

### 7.4 Node ID 分配（环形合并算法）

```
Node ID 范围: 1 ~ 254 (255 个节点，0 为保留值)

分布式分配算法 (MergeNodeIdAllocRing):
  ┌───────────────────────────────────────────┐
  │         Node ID Ring (mod 255)            │
  │                                           │
  │    cursor_A ──► [ assigned_A ] ──►        │
  │    cursor_B ──► [ assigned_B ] ──►        │
  │                                           │
  │  Merge 规则:                               │
  │    每个节点维护自己的 cursor 和已分配集合    │
  │    Leader 收集所有 node 的 cursor          │
  │    取 max(cursor, 看到的最远已分配位置)      │
  │    通过 AppendEntries 同步给所有 Follower   │
  └───────────────────────────────────────────┘
```

### 7.5 业务节点与 Center 交互

```
业务节点 Manager → Center:
  ┌─────────────────────────────────────────────────┐
  │ 1. ReportToCenter (periodic, NODE_BEAT 秒)       │
  │    ├─ 有 Raft Leader 缓存 → 只发给 Leader          │
  │    └─ 无 Leader 缓存 → fan-out 所有 Center        │
  │                                                   │
  │ 2. NodeReportRsp 处理 (DisposeDataFromCenter):    │
  │    ├─ err=2 (no stable leader) → 清空 leader 缓存 │
  │    ├─ err=0 + leader_identify → 缓存 leader       │
  │    ├─ err=0 + subscribed_route_snapshot:          │
  │    │   ├─ 与旧快照比较 (SerializeAsString 全量)    │
  │    │   ├─ 有变化 → GetRouteNoticeVersionData()    │
  │    │   │          .SetNodeNotice(oSnapshot)       │
  │    │   │          → 写入共享内存                   │
  │    │   └─ 无变化 → skip (避免无效写入)             │
  │    └─ node_id 变化 → CMD_REQ_REFRESH_NODE_ID      │
  │                      → SendToWorker               │
  └─────────────────────────────────────────────────┘
```

### 7.6 Leader 缓存与故障切换

```
业务节点 Manager:
  ┌────────────────────────────────────────┐
  │ m_strRaftLeaderCenterKey               │
  │                                        │
  │ if (!m_strRaftLeaderCenterKey.empty()) │
  │    SendTo(leader_conn, report)         │  ← 精准发送
  │ else                                   │
  │    for each center_conn:               │  ← fan-out
  │        SendTo(center_conn, report)     │
  └────────────────────────────────────────┘

Leader 缓存更新时机:
  1. NodeReportRsp 回调 (CMD_RSP_NODE_REGISTER / CMD_RSP_NODE_STATUS_REPORT)
  2. err=2 (no stable leader) → 清空缓存
  3. leader_identify 不在配置的 Center 列表中 → 清空缓存
```

---

## 8. 共享内存路由/配置同步

### 8.1 三块共享内存

```
Manager 进程创建 (MAP_SHARED | MAP_ANON):

┌──────────────────────────────────────────────────────────────────┐
│  RouteNoticeVersionMM       路由镜像共享内存          (≈164 KB)  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_node_notice (atomic<uint64>)  ← 业务版本号              │ │
│  │ seq_snapshot    (atomic<uint64>)  ← 奇:写中, 偶:稳定        │ │
│  │ node_id         (uint32)                                       │ │
│  │ node_notice_len (uint32)                                       │ │
│  │ node_notice_crc32 (uint32)                                     │ │
│  │ node_notice_blob[160*1024]  ← NodeNotice protobuf binary      │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│  CustomConfigVersionMM       自定义配置共享内存        (≈164 KB) │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_custom     (atomic<uint64>)  ← 业务版本号               │ │
│  │ seq_snapshot   (atomic<uint64>)                             │ │
│  │ custom_len     (uint32)                                     │ │
│  │ custom_crc32   (uint32)                                     │ │
│  │ custom_blob[160*1024]  ← JSON custom config                │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│  LoaderConfigVersionMM      配置文件共享内存         (≈16.1 KB)  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ seq_config     (atomic<uint64>)  ← 配置版本号               │ │
│  │ server_config_name[64]                                      │ │
│  │ server_config_body[16*1024]  ← JSON config content         │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 8.2 写入协议（Write Order: blob → len → version++）

```
Manager 写路由镜像 (SetNodeNotice):
  1. seq_snapshot.fetch_add(1)  ← 变奇数，标记"正在写"
  2. memcpy(node_notice_blob, data, data.size())
  3. node_notice_len = data.size()
  4. node_notice_crc32 = Crc32(blob, len)
  5. seq_node_notice.fetch_add(1)  ← 递增业务版本号
  6. seq_snapshot.fetch_add(1)  ← 变偶数，标记"稳定"

Worker 读路由镜像 (GetNodeNotice):
  Double-Snapshot Read (最多重试 3 次):
  1. snapA = seq_snapshot.load(acquire)
  2. if snapA & 1: continue  ← 写入进行中，重试
  3. len = node_notice_len, crc = node_notice_crc32
  4. memcpy(data, blob, len)
  5. snapB = seq_snapshot.load(acquire)
  6. if snapA != snapB || (snapB & 1): continue  ← 读写并发，重试
  7. if Crc32(data) != crc: return false  ← 数据损坏
  8. ParseFromArray(data) → 成功
```

### 8.3 Worker 定时轮询（CheckShareMem）

```
Worker::ShortPeriodicTaskCallback (每 1 秒):

CheckShareMem():
  ├─ [Loader Config]
  │   ├─ IsConfigVersionChange()? → UpdateLoaderConfigVersion()
  │   ├─ GetServerConfigFile() → 解析 JSON
  │   ├─ 比较 custom、log_level、so、module 字段
  │   └─ 有变化 → 重载对应组件
  │
  ├─ [Route Notice]
  │   ├─ IsNodeNoticeVersionChange()? → GetNodeNotice()
  │   ├─ 应用路由镜像:
  │   │   ├─ 构建 expected={node_type → {identify set}}
  │   │   ├─ 删除不在 expected 中的旧路由
  │   │   └─ 添加 expected 中的新路由
  │   └─ UpdateNodeNoticeVersion()
  │
  ├─ [Node ID]
  │   └─ node_id 变化 → SetNodeId()
  │
  └─ [Custom Config]
      ├─ IsCustomVersionChange()? → GetCustomConfig()
      ├─ 解析 customContent JSON
      └─ SetCustomConf() + UpdateCustomVersion()
```

### 8.4 版本通知路径

```
                              ┌─────────────────────────┐
                              │        Center ★          │
                              │  (Raft Leader)           │
                              └───────────┬─────────────┘
                                          │ NodeReportRsp
                                          │ + subscribed_route_snapshot
                              ┌───────────▼─────────────┐
                              │      Manager Process     │
                              │                          │
                              │  1. Compare(旧,新)       │
                              │  2. SetNodeNotice(shm)   │
                              │  3. if node_id changed:  │
                              │     SendToWorker(CMD_)   │
                              └───────────┬─────────────┘
                                          │ Shared Memory
                              ┌───────────▼─────────────┐
                              │      Worker Process      │
                              │                          │
                              │  ShortPeriodicTask (1s): │
                              │    CheckShareMem()       │
                              │    → 版本号变化即更新     │
                              │    → node_id主动通知也处理│
                              └──────────────────────────┘
```

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

