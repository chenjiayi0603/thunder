# Manager 重构设计文档

> 2026-06-02 | 基于 manager_optimization.md 分析，细化实施方案

---

## 一、现状

Manager.cpp 约 2940 行，单个文件混合了 7 个职责：

```
 1. 进程管理    (CreateWorker, RestartWorker, GracefulRestartWorker, OnChildTerminated)
 2. S2S 连接   (AcceptServerConn, send_fd, CMD_REQ_CONNECT_TO_WORKER)
 3. Center 通信 (ReportToCenter, OnCenterEvent, TcpCenterConnector)
 4. 共享内存    (SetNodeNotice, SetCustomConfig)
 5. IO 事件循环 (IoCallback, IoRead, IoWrite, IoTimeout)
 6. 消息分发    (DisposeDataFromWorker, DisposeDataAndTransferFd, DisposeDataFromCenter)
 7. 杂项       (日志、配置解析、Session管理)
```

---

## 二、具体冗余分析

### 2.1 消息解析循环重复（最明显）

`RecvDataAndDispose()` 和 `HandleIoReadComplete()` 各自包含一个 ~80 行的消息解析循环，逻辑**完全一致**：

```
while (pRecvBuff->ReadableBytes() >= gc_uiMsgHeadSize) {
    1. ParseFromArray(oInMsgHead)
    2. 等待/ParseFromArray(oInMsgBody)
    3. 根据 fd 来源分发 (m_mapWorkerFdPid → DisposeDataFromWorker
                         else → CenterConnector / DisposeDataAndTransferFd)
    4. SkipBytes + Compact(32784)
    5. 如果 !bContinue → DestroyConnect
}
```

两个版本唯一差异：
- `RecvDataAndDispose` 用 `watcher->fd` 判断来源
- `HandleIoReadComplete` 用 `pConn->iFd` 判断来源

提取后复杂度降 50%。

### 2.2 Worker 创建重复（代码量最大）

`CreateWorker()`、`RestartWorker()`、`GracefulRestartWorker()` 三个函数共享核心流程：

```
                              CreateWorker   RestartWorker   GracefulRestart
                              ────────────   ─────────────   ────────────────
socketpair (control + data)       ✅              ✅               ✅
ShmRingQueue::Create ×2          ✅              ✅               ✅
CreateEventFd ×2                 ✅              ✅               ✅
fork                             ✅              ✅               ✅
  子进程: close parent fds       ✅              ✅               ✅
  子进程: new Worker + Run       ✅              ✅               ✅
父进程: close child fds          ✅              ✅               ✅
父进程: 填充 tagWorkerAttr       ✅              ✅               ✅
父进程: m_mapWorker.insert       ✅              ✅               ✅
父进程: m_mapWorkerFdPid.insert  ✅              ✅               ✅
父进程: CreateReadFdAttr ×2      ✅              ✅               ✅
```

重复代码量：~180 行（每个函数约 60 行重复的 fork/IPC 逻辑）。

### 2.3 大函数

| 函数 | 行数 | 可拆分逻辑 |
|------|------|-----------|
| `AutoSend()` | ~100 | ParseIdentify + DoConnect + Send |
| `LoadConf()` | ~100 | 首次加载 + 热更新 |
| `OnCenterEvent()` | ~90 | ConfigUpdate → 版本检测/优雅重启 |

---

## 三、优化方案

### P0-1: 提取 `ProcessMessages()`

**目标**: 消除消息解析循环重复。

**接口设计**:
```cpp
// Manager.hpp
protected:
    /**
     * @brief 从连接接收缓冲区解析并分派完整消息
     * @param pConn     连接属性
     * @param conn_iter m_mapFdAttr 迭代器（用于 DestroyConnect）
     * @return true 继续，false 连接已销毁
     */
    bool ProcessMessages(tagConnectionAttr* pConn,
                         std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator conn_iter);
```

**流程**:
```
ProcessMessages(pConn, conn_iter)
  while (ReadableBytes >= MsgHeadSize)
    ├── Parse MsgHead → 失败则 DestroyConnect, return false
    ├── body 不完整 → break（等待下次读）
    ├── Parse MsgBody → 失败则 DestroyConnect, return false
    ├── 判断来源 (m_mapWorkerFdPid.find(pConn->iFd))
    │     ├── Worker → DisposeDataFromWorker()
    │     └── 其他 → CenterConnector::TryConsumeMessage()
    │                → 未消费则 DisposeDataAndTransferFd()
    ├── SkipBytes + Compact(32784) ×2
    └── bContinue == false → DestroyConnect, return false
  return true
```

**调用方变化**:

```cpp
// RecvDataAndDispose — 原 ~80 行 while 循环替换为 1 行：
if (iReadLen > 0) {
    return ProcessMessages(pConn, conn_iter);
}

// HandleIoReadComplete — 原 ~80 行 while 循环替换为 3 行：
if (!ProcessMessages(pConn, conn_iter)) {
    return false;
}
// (后续 IoBackend resubmit 逻辑保留不变)
```

**影响范围**:
- `Manager.hpp`: +2 行（方法声明）
- `Manager.cpp`: +60 行（新方法）, -160 行（两处重复删除）
- 净减少 ~100 行

**风险**: 极低。纯机械提取，逻辑不变，两个调用方的语义完全等价。

---

### P0-2: 提取 `SpawnSingleWorker()`

**目标**: 消除 Worker fork 流程的三处重复。

**接口设计**:
```cpp
// Manager.hpp
protected:
    /**
     * @brief 创建 IPC 通道并 fork Worker 子进程
     * @param workerIndex Worker 编号
     * @param outAttr     [出参] 填充完整的 tagWorkerAttr（含 fd、shm queue、eventfd）
     * @return >0 父进程得到的子进程 PID，子进程不会返回
     */
    pid_t SpawnSingleWorker(int workerIndex, tagWorkerAttr& outAttr);
```

**流程图**:
```
SpawnSingleWorker(workerIndex, outAttr)
  1. socketpair(PF_UNIX, SOCK_STREAM) ×2 → iControlFds, iDataFds
  2. ShmRingQueue::Create(128, 4096) ×2 → pMgrToWorker, pWorkerToMgr
  3. ShmRingQueue::CreateEventFd() ×2    → iMgrToEfd, iWorkerToEfd
  4. pid = fork()
     ├── pid == 0 (子进程):
     │     close(iControlFds[0]), close(iDataFds[0])
     │     CloseEventFd(iWorkerToMgrEfd)
     │     x_sock_set_block(fds[1], 0)
     │     new Worker(workPath, fds[1], ..., m_oCurrentConf, shm...)
     │     pWorker->SetLoaderConfigVersionMM(...)
     │     pWorker->SetRouteNoticeVersionMM(...)
     │     pWorker->SetCustomConfigVersionMM(...)
     │     pWorker->Run()
     │     exit(-2)   // 不会到达这里
     └── pid > 0 (父进程):
           close(iControlFds[1]), close(iDataFds[1])
           CloseEventFd(iMgrToWorkerEfd)
           x_sock_set_block(fds[0], 0)
           填充 outAttr (workerIndex, controlFd, dataFd, shm queues, eventfds)
           return pid
```

**调用方变化**:

```cpp
// CreateWorker — 调用 SpawnSingleWorker 替代内联 fork
void Manager::CreateWorker() {
    for (unsigned int i = 0; i < m_uiWorkerNum; ++i) {
        tagWorkerAttr attr;
        pid_t pid = SpawnSingleWorker(i, attr);
        if (pid > 0) {
            m_mapWorker[pid] = attr;
            m_mapWorkerFdPid[attr.iControlFd] = pid;
            m_mapWorkerFdPid[attr.iDataFd] = pid;
            CreateReadFdAttr(attr.iControlFd, GetFdSequence());
            CreateReadFdAttr(attr.iDataFd, GetFdSequence());
        }
    }
}

// RestartWorker — 清理旧资源后调用 SpawnSingleWorker
bool Manager::RestartWorker(int iDeathPid) {
    // ... 清理旧 worker（不变）
    tagWorkerAttr attr;
    pid_t pid = SpawnSingleWorker(iWorkerIndex, attr);
    if (pid > 0) {
        ev_loop_fork(m_loop);          // RestartWorker 特有
        m_mapWorker[pid] = attr;
        m_mapWorkerFdPid[attr.iControlFd] = pid;
        m_mapWorkerFdPid[attr.iDataFd] = pid;
        CreateReadFdAttr(attr.iControlFd, GetFdSequence());
        CreateReadFdAttr(attr.iDataFd, GetFdSequence());
        m_mapWorkerRestartNum[iWorkerIndex]++;
        ReportToCenter();
    }
    return (pid > 0);
}

// GracefulRestartWorker — 同上
bool Manager::GracefulRestartWorker(int iWorkerIndex) {
    tagWorkerAttr attr;
    pid_t pid = SpawnSingleWorker(iWorkerIndex, attr);
    if (pid > 0) {
        m_mapWorker[pid] = attr;
        m_mapWorkerFdPid[attr.iControlFd] = pid;
        m_mapWorkerFdPid[attr.iDataFd] = pid;
        CreateReadFdAttr(attr.iControlFd, GetFdSequence());
        CreateReadFdAttr(attr.iDataFd, GetFdSequence());
        // GracefulRestartWorker 特有
        lc.state  = WorkerLifecycle::STARTING;
        lc.oldPid = oldPid;
        lc.newPid = pid;
    }
    return (pid > 0);
}
```

**影响范围**:
- `Manager.hpp`: +3 行
- `Manager.cpp`: +40 行（新方法）, -180 行（三处重复）
- 净减少 ~140 行

**风险**: 低。纯机械提取，fork 行为完全不变。三个调用方各自的差异逻辑（ev_loop_fork、restart计数、lifecycle状态）保留在调用方。

**关于 RestartWorker 的 sleep(1)**: 当前仅 RestartWorker 的子进程在构造 Worker 前有 `sleep(1)`。SpawnSingleWorker 默认不带 sleep，通过在调用前设置标志或 RestartWorker 调用方自行 sleep 处理（在 fork 之前 sleep 效果等价于子进程 sleep，且不阻塞父进程）。

---

### P0-3: 拆分 `AutoSend()`

**目标**: 将 100 行函数拆为可理解的小块。

**接口设计**:
```cpp
// Manager.hpp
protected:
    struct AutoSendTarget {
        std::string host;
        int port = 0;
        int workerIndex = 0;
    };

    bool ParseAutoSendTarget(const std::string& strIdentify, AutoSendTarget& target);
    bool DoAutoConnect(const AutoSendTarget& target, const MsgHead& oMsgHead, const MsgBody& oMsgBody);
```

**AutoSend 新流程**:
```cpp
bool Manager::AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody) {
    AutoSendTarget target;
    if (!ParseAutoSendTarget(strIdentify, target)) return false;
    return DoAutoConnect(target, oMsgHead, oMsgBody);
}
```

**影响范围**: 不减少行数，但提高可读性和可测试性。

**风险**: 低。

---

## 四、不做（待后续独立 PR）

| 项目 | 原因 |
|------|------|
| P1: IO 基类提取（Manager/Worker 13 个重复函数） | 涉及模板化 `tagIoWatcherData` + Labor 继承结构改动 |
| P2: WorkerProcessManager 独立类（新文件） | 需要新文件 + 完整测试 |
| P3: S2SConnectionBroker 独立类（新文件） | 同上 |
| 统一 SendTo 重载 | 3 个重载各有使用场景，统一后语义不清 |
| Loader 删除 | 虽然默认关闭但有使用场景（手动改配置文件热加载） |

---

## 五、影响汇总

```
当前 Manager.cpp:  ~2940 行

P0-1 后 (ProcessMessages):  ~2840 行  (-100)
P0-2 后 (SpawnSingleWorker): ~2700 行  (-140)
P0-3 后 (AutoSend 拆分):    ~2700 行  (不减少行数，但拆为 3 个 <40 行的小函数)

净减少: ~240 行重复代码
```

---

## 六、实施顺序

1. **P0-1** `ProcessMessages` — 最简单，最安全，先做
2. **P0-2** `SpawnSingleWorker` — 代码量减少最大
3. **P0-3** `AutoSend` 拆分 — 纯可读性改进

每步完成后编译验证，再继续下一步。
