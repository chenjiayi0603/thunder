# Thunder 核心架构 — 进程模型 · 事件循环 · C++20 协程

> 源码: `code/Net/src/labor/` (Manager/Worker), `code/Net/include/coro/` (协程)
> 续篇: [21-data-plane.md](21-data-plane.md) (网络I/O + 共享内存路由), [22-operations-internals.md](22-operations-internals.md) (连接管理 + 插件)

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

### 4.4 全局线程池

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
