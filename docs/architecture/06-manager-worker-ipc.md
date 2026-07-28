# Manager-Worker IPC 机制

> 源码: `code/Net/src/labor/Manager.cpp:1460` (fork), `code/Net/src/labor/Worker.cpp` (Run)

---

## 进程模型

```
Manager (父进程)
  │
  ├── fork() → Worker-0 (子进程)
  ├── fork() → Worker-1 (子进程)
  │   ...
  └── fork() → Worker-N (子进程)

每个服务: 1 Manager + N Worker
Logic/Interface/Hello: 1 Manager + 1 Worker (默认)
```

---

## fork 时建立的 5 条 IPC 通道

```
Manager                         Worker
  │                               │
  ├─ controlFd ─────────────────→ iManagerControlFd    (控制消息)
  ├─ dataFd ────────────────────→ iManagerDataFd       (传 fd)
  ├─ pMgrToWorker (shm ring) ──→ 共享内存队列          (通知: eventfd)
  ├─ pWorkerToMgr (shm ring) ──→ 共享内存队列          (通知: eventfd)
  └─ RouteNoticeVersionData ────→ CheckShareMem()      (路由: version++检测)
```

```cpp
for (unsigned int i = 0; i < m_uiWorkerNum; ++i) {
    // 1. 两对 socketpair
    socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds);  // 控制消息
    socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds);     // 传 fd (SCM_RIGHTS)

    // 2. 共享内存环形队列 (双向)
    ShmRingQueue* pMgrToWorker = ShmRingQueue::Create(128, 4096);
    ShmRingQueue* pWorkerToMgr = ShmRingQueue::Create(128, 4096);
    int iMgrToWorkerEfd = ShmRingQueue::CreateEventFd();
    int iWorkerToMgrEfd = ShmRingQueue::CreateEventFd();

    // 3. 共享内存版本数据
    LoaderConfigVersionData    // 配置热更新
    RouteNoticeVersionData     // 路由表
    CustomConfigVersionData    // 自定义配置

    if (fork() == 0) {  // Worker
        ev_loop_destroy(m_loop);
        CloseSocket(m_iS2SListenFd);
        close(iControlFds[0]); close(iDataFds[0]);
        pWorker->Run();
    } else {  // Manager
        close(iControlFds[1]); close(iDataFds[1]);
        CreateReadFdAttr(iControlFds[0]);
        CreateReadFdAttr(iDataFds[0]);
    }
}
```

---

## S2S 连接: Manager accept → 传 fd 给 Worker

### 为什么 Manager accept

S2S 连接不走 reuseport，由 Manager 统一 accept 再分发——Manager 可以做连接数控制、权限校验。

### 流程

```
Manager IO 回调 (watcher->fd == m_iS2SListenFd):
  1. accept() → iAcceptFd
  2. send_fd_with_attr(iDataFd, iAcceptFd, ip, codec)
     → Unix domain socket + SCM_RIGHTS 把 fd 传给 Worker
  3. Manager close(iAcceptFd)  ← 不再管这个连接

Worker IO 回调 (watcher->fd == iManagerDataFd):
  FdTransfer():
    1. recv_fd_with_attr(iManagerDataFd) → iAcceptFd + IP + codec
    2. CreateAcceptFdAttr(iAcceptFd, seq, codec)
       → tagConnectionAttr (recvBuff, sendBuff, ...)
       → mapFdAttr 注册
       → AddIoReadEvent → libev 监听 EV_READ
    3. 后续 IO 与客户端连接完全相同
```

---

## 进程生命周期

```
Manager::IdleCallback / PeriodicTaskCallback:
  CheckWorker():
    for each worker:
      if now - lastBeat > m_iWorkerBeat (2×beat_interval + 1):
        kill(pid, SIGKILL)    // 超时未心跳则强杀
        RestartWorker(pid):
          1. close old socketpairs
          2. create new socketpairs + shm ring queues
          3. fork new Worker
          4. ev_loop_fork(m_loop)
          5. pass shm pointers

Worker::CheckParent():
  if getppid() == 1:  // Manager 已死
    exit(0)            // 孤儿进程退出
  SendToParent: CMD_REQ_UPDATE_WORKER_LOAD (负载上报)
```
