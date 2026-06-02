# Thunder Worker 优雅重启设计

> 2026-06-01 | 基于现有 RestartWorker 分析

---

## 一、现状: 崩溃重启 (不是优雅重启)

```cpp
// Manager.cpp:1577 — 当前只有这个
Manager::RestartWorker(int iDeathPid)
```

### 当前流程

```
Worker 崩溃 (SIGCHLD)
  → Manager::OnChildTerminated()
  → RestartWorker()
    → 销毁旧的 socketpair + shm ring queue
    → 重建 socketpair + shm ring queue
    → fork 新 Worker
    → sleep(1) ← hack, 等旧进程资源释放
    → 新 Worker::Run()
```

### 当前问题

| 问题 | 影响 |
|------|------|
| 只有崩溃重启，没有主动重启 | 改 so/module 配置后要走 in-place dlopen (RTLD_NODELETE 泄露) |
| 旧 Worker 的连接全部断开 | 客户端收到 RST |
| 新 Worker 重建 listen fd | SO_REUSEPORT 下内核分发会丢一个 accept 窗口 |
| `sleep(1)` | 不确定的等待 |
| 没有新旧协调 | 无 | 

---

## 二、优雅重启设计

### 核心思想

```
不是 "杀了再起"，是 "先起新的，等旧的优雅退出"

                    Manager
                       │
  Step 1: fork new Worker (共享旧 IPC)
                       │
  Step 2: new Worker init → 告知 Manager "就绪"
                       │
  Step 3: Manager → old Worker: SIGTERM
                       │
  Step 4: old Worker: 停止 accept, 排空连接, 退出
                       │
  Step 5: Manager 检测 old Worker 退出 → 清理旧 IPC
```

### 2.1 状态机

```
Worker 状态:
  RUNNING    → 正常服务中
  DRAINING   → 收到 SIGTERM, 停止 accept, 等待已有连接完成
  STOPPED    → 所有连接关闭, 准备退出

Manager 状态 (per Worker):
  RUNNING       → old: 服务中, new: 未启动
  STARTING      → old: 服务中, new: 初始化中
  DRAINING      → old: 排空中,   new: 接管完成
  NEW_ACTIVE    → old: 已退出,   new: 服务中
```

### 2.2 时间线

```
 old Worker            Manager              new Worker
 ─────────             ───────              ──────────
    │                     │                      │
    │  [RUNNING]           │ [RUNNING]             │
    │                     │                      │
    │                     │ 1.收到重启指令         │
    │                     │   (so变更/手动)       │
    │                     │                      │
    │                     │ 2.fork+setup IPC ──→ │
    │                     │                      │ [STARTING]
    │                     │                      │ InitClientListener
    │                     │                      │ LoadSo (新版本)
    │                     │                      │ InitIoBackend
    │                     │                      │
    │                     │ 3.← CMD_WORKER_READY │
    │                     │                      │ [RUNNING]
    │                     │                      │ 开始 accept 新连接
    │                     │ [DRAINING]            │
    │                     │                      │
    │ 4.← SIGTERM         │                      │
    │ [DRAINING]          │                      │
    │ 停止 accept          │                      │ accept 新连接
    │ 排空已有连接         │                      │
    │ 关闭所有 fd          │                      │
    │                     │                      │
    │ 5.→ SIGCHLD          │                      │
    │ [EXIT]              │                      │
    │                     │ [NEW_ACTIVE]          │
    │                     │ 清理旧 IPC             │
    │                     │ 回收旧 pid             │
```

### 2.3 连接迁移策略

```
access_port (客户端连接), SO_REUSEPORT 模式:

  old Worker: listen fd 还在, 但停止 accept
  new Worker: listen fd 已创建, 内核开始分发给它
  → 新连接自然切到新 Worker, 零中断

inner_port (S2S 连接):

  Manager 维持 listen_fd
  新的 S2S 连接 → Manager accept → send_fd → new Worker
  旧的 S2S 连接 → old Worker 排空 → close
  → 对端 S2S 重连机制 (AutoConnect) 自动切到新 Worker
```

### 2.4 排空超时

```
old Worker 收到 SIGTERM 后:

  Step 1: 停止 accept (m_bAccepting = false)
  Step 2: 等待已有连接完成 (最多 30s)
          - 遍历 mapFdAttr
          - 如果还有活跃 Step/未写完数据 → 继续等
  Step 3: 超时或全部完成 → 关闭所有 fd → exit(0)

  Manager 侧:
    如果 60s 后 old Worker 还没退出 → SIGKILL
```

---

## 三、代码改动

### 3.1 Manager 新增

```cpp
// Manager.hpp
class Manager {
    // 现有: 崩溃重启
    bool RestartWorker(int iDeathPid);

    // 新增: 主动优雅重启
    bool GracefulRestartWorker(int iWorkerIndex);

    enum class WorkerState {
        RUNNING,     // old worker serving
        STARTING,    // new worker initializing  
        DRAINING,    // old worker draining, new worker active
        NEW_ACTIVE,  // old worker exited, new worker serving
    };
    struct WorkerLifecycle {
        WorkerState  state = RUNNING;
        int          oldPid  = -1;
        int          newPid  = -1;
        time_t       drainStartTime = 0;
        static constexpr int DRAIN_TIMEOUT = 60;  // seconds before SIGKILL
    };
    std::unordered_map<int, WorkerLifecycle> m_workerLifecycle;
};
```

### 3.2 Worker 新增

```cpp
// Worker.hpp
class Worker {
    // 新增: 排空模式
    void EnterDrainMode();

    // 新增: 检查排空是否完成
    bool IsDrainComplete() const;

private:
    std::atomic<bool> m_bDraining{false};
    std::atomic<bool> m_bAccepting{true};
};
```

### 3.3 Worker::EnterDrainMode()

```cpp
void Worker::EnterDrainMode() {
    m_bDraining = true;
    m_bAccepting = false;

    LOG4_INFO("Worker %d entering drain mode", iWorkerIndex);

    // 停止 accept (IO 回调里检查 m_bAccepting)
    // 客户端 listen fd: 让内核停止分发新连接到本 Worker
    //   → SO_REUSEPORT 下内核自动把新连接分给其他 Worker

    // 通知 Manager: 我在排空
    SendToParent(CMD_WORKER_DRAINING, seq, "");
}
```

### 3.4 Worker::Run() 改动

```cpp
void Worker::Run() {
    // ... 现有初始化 ...

    while (true) {
        ev_run(m_loop, EVRUN_ONCE);

        if (m_bDraining && IsDrainComplete()) {
            LOG4_INFO("Worker drain complete, exiting");
            break;
        }

        // 定期检查排空超时
        if (m_bDraining && (time(nullptr) - m_drainStartTime > 30)) {
            LOG4_WARN("Worker drain timeout, force exit");
            break;
        }
    }

    // 清理
    Destroy();
}
```

### 3.5 AcceptClientConn 改动

```cpp
bool Worker::AcceptClientConn(int iFd) {
    // 排空模式: 不 accept 新连接
    if (m_bDraining || !m_bAccepting) {
        return true;  // 让内核分发给其他 Worker
    }
    // ... 现有 accept 逻辑 ...
}
```

### 3.6 触发方式

```
触发优雅重启的场景:

  1. so 配置变更 (热重载 .so 插件)
     Manager::OnCenterEvent(ConfigUpdated):
       if (so 字段变了) → GracefulRestartWorker()

  2. module 配置变更
     Manager::OnCenterEvent(ConfigUpdated):
       if (module 字段变了) → GracefulRestartWorker()

  3. 手动触发 (运维)
     Center Admin API:
       POST /admin {"cmd": "restart", "args": ["worker", "0"]}
     → CMD_RESTART_WORKER → Manager → GracefulRestartWorker(0)
```

---

## 四、So/Module 热重载流程对比

### 现在 (in-place dlopen)

```
配置变更 → Worker::CheckShareMem()
  → LoadSo(new, force=true)
  → dlopen(RTLD_NODELETE)  ← 旧 .so 泄漏
  → mapSo[cmd] = new
  ❌ 旧 .so 内存永不释放
```

### 优雅重启后

```
配置变更 → Manager::OnCenterEvent()
  → GracefulRestartWorker(0)
  → fork new Worker (加载新 .so)
  → old Worker SIGTERM → 排空 → exit
  → 旧进程退出 → OS 回收全部内存
  ✅ .so 干净卸载
```

---

## 五、改动量估算

| 改动 | 文件 | 行数 |
|------|------|------|
| Manager: GracefulRestartWorker | Manager.cpp | +60 |
| Manager: WorkerLifecycle 状态机 | Manager.hpp | +30 |
| Worker: EnterDrainMode | Worker.cpp | +30 |
| Worker: IsDrainComplete | Worker.cpp | +20 |
| Worker: AcceptClientConn 加 `m_bAccepting` 检查 | Worker.cpp | +5 |
| Worker: Run 加排空循环 | Worker.cpp | +15 |
| Manager: OnCenterEvent 触发优雅重启 | Manager.cpp | +20 |
| 全新消息: CMD_WORKER_READY / CMD_WORKER_DRAINING | Proto | +10 |
| **合计** | | **~190 行** |

---

## 六、验证

```bash
# 1. 正常重启
curl POST /admin {"cmd":"restart","args":["worker","0"]}
# → 旧 Worker 排空, 新 Worker 接管, 客户端无感知

# 2. so 热重载
# 修改 Hello.json 的 "so" 字段
Center 推送 → Manager GracefulRestartWorker → 新 Worker 加载新 .so

# 3. 连接不断
wrk -t4 -c100 -d60s http://127.0.0.1:27006/hello/hello &
# 同时触发重启 → QPS 短暂下降 → 恢复, 无 RST
```


---

## 七、socket fd 转移 (关键: 客户端零感知)

### 7.1 为什么不能用重连

```
重连方案:
  old Worker close(fd) → client 收到 RST
  → curl: "Connection reset by peer"
  → wrk: socket errors 飙升
  → 不是优雅重启, 是断线重连

fd 转移方案:
  old Worker send_fd → Manager → new Worker
  → 同一个 TCP 连接继续用
  → 客户端完全不知道后端换了进程
  → 真正的零停机
```

### 7.2 fd 转移通道

```
已经有的基础设施: Manager ↔ Worker 之间有 dataFd (socketpair), 
已经实现了 send_fd_with_attr / recv_fd_with_attr

现有的 S2S fd 转移:
  Manager accept S2S 连接 → send_fd_with_attr(dataFd, fd) → Worker recv_fd

优雅重启的 fd 转移 (反向):
  old Worker → send_fd_to_manager(fd) → Manager → send_fd_to_worker(fd) → new Worker
```

### 7.3 转移流程

```
 old Worker                      Manager                       new Worker
 ─────────                       ───────                       ──────────
     │                               │                              │
     │ [收到 SIGTERM]                 │                              │
     │ EnterDrainMode()              │                              │
     │ 停止 accept                    │                              │
     │                               │                              │
     │ for each client fd:            │                              │
     │   send_fd_with_attr(           │                              │
     │     iManagerDataFd, fd,        │                              │
     │     IP, codec) ────────────→  │                              │
     │   close(fd)                    │ 收到 fd + attr               │
     │                               │                              │
     │                               │ send_fd_with_attr(            │
     │                               │   newDataFd, fd,              │
     │                               │   IP, codec) ────────────→  │
     │                               │                              │ RecvFdAndReg()
     │                               │                              │ 从 dataFd 读 fd
     │                               │                              │ CreateAcceptFdAttr
     │                               │                              │ AddIoReadEvent
     │                               │                              │ 开始收发
     │                               │                              │
     │ CMD_WORKER_DRAIN_DONE ──────→ │                              │
     │ [EXIT]                        │                              │
```

### 7.4 关键时序保证

```
TCP 连接在转移期间:
  old Worker send_fd 之后 → close(fd)
  new Worker recv_fd 之前 → fd 还没到

  问: 中间这段时间 fd 被谁持有?
  答: Manager 持有 (dataFd 的对端)

  问: 数据会丢吗?
  答: 不会。TCP 接收缓冲区在内核, 不被 close 影响。
      close 只是减少引用计数, 内核在最后一个引用关闭后才发 FIN。
      Manager 收到 fd → dup 增加引用计数 → send_fd 给 new Worker
      → 整个过程中 fd 引用计数 ≥1 → 内核不发 FIN → 连接存活
```

### 7.5 转移期间的新数据

```
转移期间客户端可能发来新数据:

  old Worker send_fd 之后:
    → 已经 close(fd), 不再读这个 fd
    → TCP 接收缓冲在内核里堆积

  new Worker recv_fd 之后:
    → 收到 fd
    → AddIoReadEvent → libev 发现 EV_READ
    → RecvDataAndDispose → 读出之前堆积的数据
    → 正常处理
```

### 7.6 排空 vs 转移的判断

```
哪些 fd 转移, 哪些 fd 等排空:

  转移:
    · 客户端连接 (access_port accept 的)
    · 状态: 在途请求未完成, 需要继续用
    → send_fd 给 new Worker

  排空 (不转移):
    · S2S 连接 (inner_port, CODEC_PB_INTERNAL)
    · 状态: Manager 维持着对端连接
    → old Worker 不再收发, Manager 的 fd 继续用
    → 对端重连时会走新路径到 new Worker

  丢弃:
    · 已完成/空闲的客户端连接
    → 直接 close, client 下次请求走新连接
```

### 7.7 新增代码

```cpp
// Worker.hpp 新增
struct PendingFdTransfer {
    int   fd;
    char  remoteAddr[32];
    int   codec;
    uint32 seq;
};

// Worker: 排空时收集要转移的 fd
void Worker::CollectActiveFds(std::vector<PendingFdTransfer>& fds) {
    for (auto& [fd, conn] : mapFdAttr) {
        if (conn->eCodecType != CODEC_PB_INTERNAL     // 不是 S2S
            && conn->pRecvBuff->ReadableBytes() > 0   // 有未处理数据
            && !conn->IsVerify()) {                     // 未完成校验
            PendingFdTransfer t;
            t.fd = fd;
            snprintf(t.remoteAddr, sizeof(t.remoteAddr), "%s", conn->szRemoteAddr);
            t.codec = conn->eCodecType;
            t.seq = conn->ulSeq;
            fds.push_back(t);
        }
    }
}

// Manager 新增: 接收 old Worker 的 fd, 转发给 new Worker
void Manager::ForwardFdToNewWorker(int oldPid, int newPid, int fd, 
                                    const char* ip, int codec) {
    auto it = m_mapWorker.find(newPid);
    if (it != m_mapWorker.end()) {
        send_fd_with_attr(it->second.iDataFd, fd, ip, codec);
        close(fd);  // Manager 这边的引用
    }
}
```


---

## 八、排空策略 (Drain Strategy)

### 8.1 核心原则

```
排空 = 停止收新请求 + 等旧请求处理完 + 超时兜底

不是"等所有连接关闭" (keepalive 连接永远不关)
而是"等所有在途请求完成" (Step 跑完, 响应发完)
```

### 8.2 什么算"在途请求"

Worker 遍历所有连接，判断是否需要等待：

```
每个连接的状态:

  ① 空闲 (keepalive, 无未处理数据, 无活跃 Step)
    → 直接 close, 不等待
    → 客户端下次请求走新 Worker (SO_REUSEPORT 分发)

  ② 有未读数据 (pRecvBuff 里有字节)
    → 读完 → 解码 → 处理完 → 发响应 → 再 close
    → 最多等 30s

  ③ 有活跃 Step (mapCallbackStep 里有这个连接的 seq)
    → Step 正在执行 (等 DB/Cache/S2S 响应)
    → 等 Step::Callback 完成 → 发响应 → 再 close
    → 最多等 30s

  ④ 有未写完数据 (pSendBuff 里有字节)
    → 写完 (IoWrite) → 再 close
    → 最多等 5s (写操作本身很快)

  ⑤ 监听 fd (m_iC2SListenFd)
    → 不 close, 但停止 accept (m_bAccepting=false)
    → Manager::RestartWorker 里会重建

  ⑥ S2S 连接 (CODEC_PB_INTERNAL)
    → 不转移, 让 Manager 维持
    → 新的 S2S 请求走新 Worker

  ⑦ 正在 HTTP 解析中 (http_parser 状态机中间态)
    → 递归 buffer 里拿到完整 HTTP 请求 → 解码 → 处理完 → close
    → 最多等 30s
```

### 8.3 排空循环

```cpp
bool Worker::IsDrainComplete() {
    // 检查是否还有"在途请求"
    for (auto& [fd, conn] : mapFdAttr) {
        // 跳过监听 fd
        if (fd == m_iC2SListenFd) continue;
        // 跳过 S2S 连接
        if (conn->eCodecType == CODEC_PB_INTERNAL) continue;

        // 有未读数据 → 还没处理完
        if (conn->pRecvBuff->ReadableBytes() > 0) return false;

        // 有未写完数据 → 还没发完响应
        if (conn->pSendBuff->ReadableBytes() > 0) return false;
    }

    // 检查是否有活跃 Step
    if (!mapCallbackStep.empty()) return false;

    // 检查是否有活跃 HttpStep  
    for (auto& [fd, steps] : mapHttpAttr) {
        if (!steps.empty()) return false;
    }

    return true;  // 所有在途请求已完成
}
```

### 8.4 排空完整流程

```
Worker 收到 SIGTERM:

  Step 1: 标记排空
    m_bDraining = true
    m_bAccepting = false
    m_drainStartTime = time(nullptr)
    记录: 当前有 X 个连接, Y 个活跃 Step

  Step 2: 立即关闭空闲连接
    for each conn in mapFdAttr:
      if 无未读数据 && 无未写数据 && 无活跃 Step && 非 S2S:
        close(fd)  ← 立即关闭, 不等
        mapFdAttr.erase(fd)
    记录: 关闭了 Z 个空闲连接

  Step 3: 等在途请求完成
    while (!IsDrainComplete() && !timeout):
      ev_run(m_loop, EVRUN_ONCE)  // 继续处理 IO
      // 每个请求处理完 → 发响应 → close fd
      // Step 完成后 → mapCallbackStep.erase(seq)

  Step 4: 超时兜底
    if timeout:
      强制关闭剩余连接 (可能丢掉几个请求)
      记录: 超时强制关闭了 N 个连接

  Step 5: 清理退出
    Destroy()
    SendToParent(CMD_WORKER_DRAIN_DONE)
    exit(0)
```

### 8.5 超时配置

```
等待在途请求完成:      30s   (WORKER_DRAIN_GRACE_PERIOD)
等待数据写完:          5s    (WORKER_DRAIN_WRITE_TIMEOUT)
Manager 等 old Worker: 60s   (超时 → SIGKILL)

可配置:
  "worker": {
    "drain_grace_period": 30,
    "drain_write_timeout": 5
  }
```

### 8.6 排空期间的事件循环

```
排空期间的 ev_run 只处理:
  ✅ EV_READ  → RecvDataAndDispose → 处理完 → SendToClient
  ✅ EV_WRITE → IoWrite → 把响应发出去
  ✅ Step 回调 → Callback → 业务逻辑完成
  ✅ Timer  → Step/Session 超时

  ❌ AcceptClientConn → m_bAccepting=false, 直接跳过
  ❌ 新连接 → 内核分发给其他 Worker (SO_REUSEPORT)
```

### 8.7 监控指标

```
优雅重启期间可观测:

  thunder_worker_drain_active_connections  当前排空中连接数
  thunder_worker_drain_idle_closed         立即关闭的空闲连接数
  thunder_worker_drain_completed           正常完成的请求数
  thunder_worker_drain_timeout_forced      超时强制关闭的连接数
  thunder_worker_drain_duration_seconds    排空总耗时
```


---

## 九、Manager 端完整设计

### 9.1 数据结构

```cpp
// Manager.hpp 新增

struct WorkerLifecycle {
    enum State {
        RUNNING,       // old worker 正常服务
        STARTING,      // 旧worker运行中, 新worker初始化
        DRAINING,      // 旧worker排空中, 新worker接管完成
        NEW_ACTIVE,    // 旧worker已退出, 新worker正常服务
    };
    State  state = RUNNING;
    int    oldPid  = -1;   // 正在排空的旧进程
    int    newPid  = -1;   // 新启动的进程
    time_t drainStartTime = 0;

    static constexpr int DRAIN_GRACE_PERIOD = 60;  // Manager 等待 old Worker 退出的超时
};

std::unordered_map<int, WorkerLifecycle> m_workerLifecycle;
```

### 9.2 状态转换

```
  RUNNING ──(收到重启指令)──→ STARTING
                                 │
                      fork new Worker + 初始化
                                 │
                      new Worker CMD_WORKER_READY
                                 │
                                 ▼
                             DRAINING ──(old SIGCHLD)──→ NEW_ACTIVE
                                 │
                          超时60s → SIGKILL old
                                 │
                                 ▼
                             NEW_ACTIVE
```

### 9.3 GracefulRestartWorker 实现

```cpp
bool Manager::GracefulRestartWorker(int iWorkerIndex)
{
    LOG4_INFO("Graceful restart worker %d start", iWorkerIndex);

    auto& lc = m_workerLifecycle[iWorkerIndex];

    // === Step 1: 找到 old Worker ===
    int oldPid = -1;
    for (auto& [pid, attr] : m_mapWorker) {
        if (attr.iWorkerIndex == iWorkerIndex)
        {
            oldPid = pid;
            break;
        }
    }
    if (oldPid < 0)
    {
        LOG4_ERROR("worker %d not found", iWorkerIndex);
        return false;
    }

    // === Step 2: fork new Worker (复用 CreateWorker 的 IPC 创建逻辑) ===
    int iControlFds[2], iDataFds[2];
    socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds);
    socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds);
    ShmRingQueue* pMgrToWorker = ShmRingQueue::Create(128, 4096);
    ShmRingQueue* pWorkerToMgr = ShmRingQueue::Create(128, 4096);
    int iMgrToWorkerEfd = ShmRingQueue::CreateEventFd();
    int iWorkerToMgrEfd = ShmRingQueue::CreateEventFd();

    LoaderConfigVersionMM* pLoaderMM = GetLoaderConfigVersionData().GetLoaderConfigVersionMM();
    RouteNoticeVersionMM*  pRouteMM  = GetRouteNoticeVersionData().GetRouteNoticeVersionMM();
    CustomConfigVersionMM* pCustMM   = GetCustomConfigVersionData().GetCustomConfigVersionMM();

    int newPid = fork();
    if (newPid == 0)   // new Worker
    {
        close(iControlFds[0]);
        close(iDataFds[0]);
        ShmRingQueue::CloseEventFd(iWorkerToMgrEfd);
        Worker* p = new Worker(m_strWorkPath,
            iControlFds[1], iDataFds[1], iWorkerIndex,
            m_oCurrentConf, pMgrToWorker, pWorkerToMgr,
            iMgrToWorkerEfd, iWorkerToMgrEfd);
        p->GetLoaderConfigVersionData().SetLoaderConfigVersionMM(pLoaderMM);
        p->GetRouteNoticeVersionData().SetRouteNoticeVersionMM(pRouteMM);
        p->GetCustomConfigVersionData().SetCustomConfigVersionMM(pCustMM);
        p->Run();
        delete p;
        exit(-2);
    }

    // === Step 3: Manager 记录 new Worker ===
    close(iControlFds[1]);
    close(iDataFds[1]);
    ShmRingQueue::CloseEventFd(iMgrToWorkerEfd);

    tagWorkerAttr newAttr;
    newAttr.iWorkerIndex = iWorkerIndex;
    newAttr.iControlFd = iControlFds[0];
    newAttr.iDataFd = iDataFds[0];
    newAttr.pMgrToWorkerQueue = pMgrToWorker;
    newAttr.pWorkerToMgrQueue = pWorkerToMgr;
    newAttr.iMgrToWorkerEventFd = iMgrToWorkerEfd;
    newAttr.iWorkerToMgrEventFd = iWorkerToMgrEfd;

    m_mapWorker[newPid] = newAttr;
    CreateReadFdAttr(iControlFds[0], GetFdSequence());
    CreateReadFdAttr(iDataFds[0], GetFdSequence());

    lc.state = STARTING;
    lc.oldPid = oldPid;
    lc.newPid = newPid;

    // === Step 4: 等 new Worker 就绪 ===
    // new Worker 初始化完成后发送 CMD_WORKER_READY
    // → Manager::DisposeDataFromWorker 处理 → 进入 DRAINING

    LOG4_INFO("Graceful restart worker %d: new pid=%d, old pid=%d (waiting ready)",
              iWorkerIndex, newPid, oldPid);
    return true;
}
```

### 9.4 Manager 接收 new Worker 就绪通知

```cpp
// Manager::DisposeDataFromWorker 新增处理

case CMD_WORKER_READY: {
    int workerIndex = /* parse from body */;
    auto& lc = m_workerLifecycle[workerIndex];
    if (lc.state != STARTING) {
        LOG4_WARN("unexpected WORKER_READY in state %d", (int)lc.state);
        break;
    }

    lc.state = DRAINING;
    lc.drainStartTime = time(nullptr);

    // 发送 SIGTERM 给 old Worker
    LOG4_INFO("new worker %d ready, signaling old worker %d to drain",
              lc.newPid, lc.oldPid);
    kill(lc.oldPid, SIGTERM);

    // 同时发 CMD_WORKER_DRAIN 消息 (更友好的方式)
    SendToWorker(CMD_WORKER_DRAIN, GetSequence(), "");
    break;
}
```

### 9.5 Manager 处理 old Worker 退出

```cpp
// Manager::OnChildTerminated 新增处理

// 检查是否在优雅重启中
for (auto& [idx, lc] : m_workerLifecycle) {
    if (lc.state == DRAINING && iDeathPid == lc.oldPid) {
        LOG4_INFO("old worker %d exited, graceful restart complete for worker %d",
                  lc.oldPid, idx);
        lc.state = NEW_ACTIVE;
        // 清理 old Worker 的 IPC (socketpair, shm queue)
        CleanupOldWorkerIPC(lc.oldPid);
        return;  // 不触发 RestartWorker (不是崩溃)
    }
}

// 不是优雅重启 → 可能是崩溃 → 走现有 RestartWorker 逻辑
RestartWorker(iDeathPid);
```

---

## 十、Proto 新增消息

```protobuf
// coor.proto 新增

// Manager → Worker: 开始排空
// cmd = CMD_WORKER_DRAIN
message WorkerDrain {
    uint32 grace_period_sec = 30;  // 排空等多久
}

// Worker → Manager: 新 Worker 初始化完成
// cmd = CMD_WORKER_READY
message WorkerReady {
    uint32 worker_index = 1;
}

// Worker → Manager: 排空完成, 即将退出
// cmd = CMD_WORKER_DRAIN_DONE
message WorkerDrainDone {
    uint32 worker_index = 1;
    uint32 connections_closed = 2;  // 关闭了多少连接
    uint32 connections_transferred = 3;  // 转移了多少连接
}
```

对应的 cmd 编号:
```
CMD_WORKER_DRAIN      = 0x00001001  (Manager → Worker)
CMD_WORKER_READY      = 0x00001002  (Worker → Manager)
CMD_WORKER_DRAIN_DONE = 0x00001003  (Worker → Manager)
```

---

## 十一、触发入口

### 11.1 Center Admin API

```
POST /admin
{
  "cmd": "restart",
  "args": ["worker", "0"]     // 重启 worker-0
}

→ Center → CMD_RESTART_WORKER → Manager → GracefulRestartWorker(0)
```

### 11.2 配置变更自动触发

```cpp
// Manager::OnCenterEvent 新增

case CenterEventType::ConfigUpdated: {
    // ... 现有写入 shm 逻辑 ...

    // 检查是否需要重启 Worker (so/module 变更)
    util::CJsonObject newConf;
    newConf.Parse(ev.config_content);
    auto oldCustom = m_oCurrentConf["custom"];

    if (newConf["so"] != oldCustom["so"] ||
        newConf["module"] != oldCustom["module"])
    {
        LOG4_INFO("so/module changed, trigger graceful restart");
        for (unsigned int i = 0; i < m_uiWorkerNum; ++i)
        {
            GracefulRestartWorker(i);
        }
    }
    break;
}
```

### 11.3 信号触发

```
kill -SIGUSR1 <manager_pid>  → Manager::OnSigUsr1()
  → 遍历所有 Worker → GracefulRestartWorker(i)
```

---

## 十二、边界情况处理

| 场景 | 处理 |
|------|------|
| new Worker fork 失败 | 不发送 SIGTERM, 保持 old Worker 运行, 记录错误, 重试 |
| old Worker 收到 SIGTERM 时已有大量连接 | 正常排空, 最多等 30s, 超时 SIGKILL |
| 排空期间 new Worker 崩溃 | old Worker 还在运行 → 用 old 继续服务 → 下次再试 |
| 连续重启 (短时间内多次触发) | 状态机检查: 如果已经是 STARTING/DRAINING → 拒绝新请求 |
| 只有一个 Worker (process_num=1) | 排空期间新连接无法接受 → 客户端重试 → 等 new Worker 就绪后继续 |
| 多个 Worker (process_num>1) | 逐个重启 → 同一时间只有 1 个 Worker 在排空 → 其他正常服务 |

---

## 十三、测试计划

### 13.1 单元测试

```
1. Worker::IsDrainComplete()
   - 无连接 → true
   - 有未读数据 → false
   - 有活跃 Step → false
   - 全部完成 → true

2. Manager::GracefulRestartWorker()
   - old Worker 不存在 → false
   - fork 成功 → 状态 STARTING
   - fork 失败 → 不改变状态

3. 状态机转换
   - RUNNING → STARTING → DRAINING → NEW_ACTIVE ✓
   - 非法转换: DRAINING → GracefulRestartWorker → 拒绝
```

### 13.2 集成测试

```bash
# 1. 重启期间客户端无感知
wrk -t4 -c100 -d60s http://127.0.0.1:27006/hello/hello &
sleep 10
curl POST /admin {"cmd":"restart","args":["worker","0"]}
# 预期: wrk 输出无 socket errors, QPS 短暂下降后恢复

# 2. so 热重载
# 修改 Hello.json 的 so 字段
# Center 推送 → 自动触发优雅重启
# 新 Worker 加载新 .so

# 3. 排空超时
# 模拟: Worker 有一个请求一直不返回
# 30s 后强制退出
```


---

## 十四、fd 转移的实现细节

### 14.1 原理: SCM_RIGHTS (内核机制, 非应用层模拟)

```
send_fd_with_attr(sock_fd, clientFd, ip, codec):
  通过 Unix domain socket 发送一个文件描述符
  内核通过 SCM_RIGHTS 控制消息复制 fd 表项
  接收方拿到的是一个新的 fd 编号, 但指向同一个 TCP 连接
```

**不是传 fd 编号, 是内核在接收方进程里创建新的 fd 表项, 指向同一个内核文件对象。**

```
  进程A               内核               进程B
  fd=42 ──→ 文件表[42]                  fd=啥?
                │
                ├──→ TCP socket ←── 内核文件对象 (引用计数+1)
                │
  fd=42 ──→ send_fd_with_attr() ──── SCM_RIGHTS ────→ fd=17 ──→ 同一个 TCP socket
```

### 14.2 Manager 中转

```
Manager 不持有客户端 fd (只做中转, 不读写):

  old Worker                       Manager                         new Worker
  ──────────                       ───────                         ──────────

  for each client fd:
    send_fd_with_attr(             OnRecvDataFd():                   OnRecvDataFd():
      iManagerDataFd,    ──────→     recv_fd_with_attr()              recv_fd_with_attr()
      clientFd,              =      收到 fd + ip + codec            收到 fd + ip + codec
      ip, codec)                     │                               │
    close(clientFd)                  │ 查 m_workerLifecycle:         CreateAcceptFdAttr
                                     │   workerIndex → newPid        AddIoReadEvent
                                     │   → 找到 newDataFd            开始正常收发
                                     │                               │
                                     │ send_fd_with_attr(            │
                                     │   newDataFd,        ──────→   │
                                     │   clientFd,                    │
                                     │   ip, codec)                   │
                                     │ close(clientFd)  ← Manager    │
                                     │                  不持有fd     │
```

### 14.3 Manager 只需新增: 接收 Worker 发来的 fd

当前 Manager::RecvDataAndDispose 处理 dataFd 上的消息。

现有: Manager 从 dataFd 读到的是 Worker 发来的业务消息（文本数据）
新增: Manager 从 dataFd 读到的如果是 SCM_RIGHTS 控制消息 → 这是 Worker 发来的 fd

```cpp
// Manager::RecvDataAndDispose 新增分支

// 检查是否有附带的 fd (SCM_RIGHTS)
int transferredFd = -1;
char remoteIp[32] = {0};
int codecType = 0;

// 尝试接收附带 fd 的消息
int ret = recv_fd_with_attr(pConn->iFd, remoteIp, sizeof(remoteIp), &codecType);
if (ret > 0)
{
    // Worker 发来了一个客户端 fd → 需要转发给 new Worker
    transferredFd = ret;

    // 找到是哪个 Worker 发的
    int workerIndex = GetWorkerIndexByFd(pConn->iFd);

    if (workerIndex >= 0)
    {
        auto& lc = m_workerLifecycle[workerIndex];
        if (lc.state == DRAINING)
        {
            // 转发给 new Worker
            int newDataFd = m_mapWorker[lc.newPid].iDataFd;
            send_fd_with_attr(newDataFd, transferredFd, remoteIp, 32, codecType);
            close(transferredFd);  // Manager 不持有
            LOG4_TRACE("forwarded client fd %d from old worker %d to new worker %d",
                       transferredFd, lc.oldPid, lc.newPid);
        }
    }
    return true;
}
```

### 14.4 Worker 端: 排空时发送客户端 fd

```cpp
// Worker::EnterDrainMode 新增

// 遍历活跃连接, 有在途请求的发送 fd 给 Manager
for (auto it = mapFdAttr.begin(); it != mapFdAttr.end(); )
{
    auto* pConn = it->second.get();

    // 跳过监听 fd 和 S2S 连接
    if (pConn->iFd == m_iC2SListenFd) { ++it; continue; }
    if (pConn->eCodecType == util::CODEC_PB_INTERNAL) { ++it; continue; }

    // 判断是否有在途请求需要转移
    bool hasPending = (pConn->pRecvBuff->ReadableBytes() > 0)  // 有未读数据
                   || (pConn->pSendBuff->ReadableBytes() > 0)  // 有未写数据
                   || HasActiveStep(pConn->iFd);               // 有活跃 Step

    if (hasPending)
    {
        // 发送 fd 给 Manager (通过 dataFd)
        send_fd_with_attr(iManagerDataFd, pConn->iFd,
                          pConn->szRemoteAddr, 32,
                          static_cast<int>(pConn->eCodecType));
        // Worker 关闭自己的 fd 引用 (Manager 持有新引用)
        close(pConn->iFd);
        it = mapFdAttr.erase(it);
    }
    else
    {
        // 空闲连接, 直接关闭
        m_pIoBackend->CloseFd(pConn->iFd);
        it = mapFdAttr.erase(it);
    }
}
```

### 14.5 为什么 Manager 做中转而不是 Worker 直连

```
Worker 之间没有 socketpair:

  old Worker ↔ Manager (有 dataFd)
  new Worker ↔ Manager (有 dataFd)
  old Worker ↔ new Worker (没有通道)

如果 Worker 直连, 需要:
  - 每对 Worker 之间建 socketpair
  - N 个 Worker = N×(N-1)/2 对
  - 启动/重启时要管理这些连接

Manager 中转:
  - 已有数据通道, 无需新增
  - 新增 ~40 行代码
```

