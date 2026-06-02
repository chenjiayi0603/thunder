# Thunder Manager-Worker 多进程交互机制

> 2026-06-01 | 基于 Worker.cpp + Manager.cpp 源码分析

---

## 一、进程模型

```
Manager (父进程)
  │
  ├── fork() → Worker-0 (子进程)
  ├── fork() → Worker-1 (子进程)
  │   ...
  └── fork() → Worker-N (子进程)

每个服务: 1 Manager + N Worker
Center:  3 节点 Raft 集群, 每节点 1 Manager + 1 Worker
Logic/Interface/Hello: 1 Manager + 1 Worker
```

## 二、fork 时建立的 IPC 通道 (Manager.cpp:1460)

```cpp
for (unsigned int i = 0; i < m_uiWorkerNum; ++i)
{
    // 1. 两对 socketpair (全双工)
    socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds);  // 控制消息
    socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds);     // 数据通道 (传fd)

    // 2. 共享内存环形队列 (双向)
    ShmRingQueue* pMgrToWorker = ShmRingQueue::Create(128, 4096);
    ShmRingQueue* pWorkerToMgr = ShmRingQueue::Create(128, 4096);
    int iMgrToWorkerEfd = ShmRingQueue::CreateEventFd();  // 通知机制
    int iWorkerToMgrEfd = ShmRingQueue::CreateEventFd();

    // 3. 共享内存版本数据 (路由 + 配置)
    LoaderConfigVersionData    // 配置热更新
    RouteNoticeVersionData     // 路由表
    CustomConfigVersionData    // 自定义配置

    int iPid = fork();

    if (iPid == 0)   // === 子进程 (Worker) ===
    {
        ev_loop_destroy(m_loop);       // 销毁 Manager 的事件循环
        CloseSocket(m_iS2SListenFd);    // 关闭 Manager 的监听 fd
        close(iControlFds[0]);         // 关掉 Manager 端
        close(iDataFds[0]);
        // Worker 持有: iControlFds[1], iDataFds[1], pMgrToWorker, pWorkerToMgr
        pWorker->Run();                 // 进入 Worker 事件循环
    }
    else            // === 父进程 (Manager) ===
    {
        close(iControlFds[1]);         // 关掉 Worker 端
        close(iDataFds[1]);
        // Manager 持有: iControlFds[0], iDataFds[0]
        // 注册 controlFd + dataFd 到 libev, 监听可读事件
        CreateReadFdAttr(iControlFds[0]);
        CreateReadFdAttr(iDataFds[0]);
    }
}
```

每个 Worker 有 **5 条 IPC 通道**:

```
Manager                         Worker
  │                               │
  ├─ controlFd ─────────────────→ iManagerControlFd    (控制消息)
  ├─ dataFd ────────────────────→ iManagerDataFd       (传 fd)
  ├─ pMgrToWorker (shm ring) ──→ 共享内存队列          (通知: eventfd)
  ├─ pWorkerToMgr (shm ring) ──→ 共享内存队列          (通知: eventfd)
  └─ RouteNoticeVersionData ────→ CheckShareMem()      (路由: version++检测)
```

## 三、监听: S2S 连接由 Manager accept, 传给 Worker

### 3.1 Manager 监听 inner_port

```
Manager::Run():
  socket() → bind(inner_port) → listen()
  m_iS2SListenFd = fd
  CreateReadFdAttr(m_iS2SListenFd)  → 注册 EV_READ
```

### 3.2 Manager accept → 传 fd 给 Worker

```
Manager IO 回调:
  watcher->fd == m_iS2SListenFd   → S2S 新连接

  1. accept(m_iS2SListenFd) → iAcceptFd
  2. send_fd_with_attr(iDataFd, iAcceptFd, ip, codec)
     → 通过 Unix domain socket 把 fd 传给 Worker
     → 附带上对端 IP 和 codec 类型
  3. Manager 自己 close(iAcceptFd)  → 不再管这个连接
```

### 3.3 Worker 收到 fd

```
Worker IO 回调:
  watcher->fd == iManagerDataFd    → Manager 传来的 fd

  FdTransfer():
    1. recv_fd_with_attr(iManagerDataFd) → iAcceptFd + IP + codec
    2. CreateAcceptFdAttr(iAcceptFd, seq, codec)
       → 创建 tagConnectionAttr (recvBuff, sendBuff, ...)
       → 注册到 mapFdAttr
       → AddIoReadEvent → libev 监听 EV_READ
    3. 后续 IO 跟客户端连接完全一样
```

**为什么 Manager accept 而不是 Worker accept？**
- reuseport 模式下客户端连接由内核分发，Worker 自己 accept
- S2S 连接不走 reuseport，由 Manager 统一 accept 再分发
- 这样 Manager 可以做连接数控制、权限校验

## 四、Worker 监听客户端连接

```
Worker::InitClientListener():
  1. IoBackend->CreateListenSocket(access_ip, access_port)
     → socket() → setsockopt(SO_REUSEPORT) → bind() → listen()

  2. m_iC2SListenFd = fd

  3. CreateFdAttr(fd, seq, CODEC_PB_INTERNAL)
     → 监听 fd 也注册到 mapFdAttr

  4. AddIoReadEvent → libev 监听 EV_READ

Worker IO 回调:
  watcher->fd == m_iC2SListenFd  → 客户端新连接

  AcceptClientConn():
    1. IoBackend->Accept(fd) → iAcceptFd + peerAddr
    2. CreateAcceptFdAttr(iAcceptFd, seq, access_codec)
       → eCodecType = CODEC_HTTP / CODEC_HTTPS / CODEC_WEBSOCKET
    3. AddIoReadEvent → 开始读客户端数据
```

## 五、Manager → Worker 消息发送

```cpp
bool Manager::SendToWorker(cmd, seq, body)
{
    for (每个 Worker)
    {
        // 优先: 共享内存队列
        if (pMgrToWorkerQueue->TryEnqueue(cmd, seq, body, len))
        {
            NotifyEventFd(iMgrToWorkerEventFd);   // 通知 Worker "有数据"
            continue;
        }

        // 降级: socketpair (shm 队列满了)
        SendTo(controlFd, cmd, seq, body);
    }
}
```

**Manager 发给 Worker 的消息类型**:
- `CMD_REQ_REFRESH_NODE_ID` — Center 分配了新的 node_id
- `CMD_REQ_NODE_REG_NOTICE` — 路由表更新
- `CMD_REQ_SERVER_CONFIG` — 配置热更新
- `CMD_REQ_RELOAD_SO` — 重新加载 .so 插件
- `CMD_REQ_RELOAD_MODULE` — 重新加载模块
- `CMD_REQ_SET_LOG_LEVEL` — 修改日志级别
- `CMD_REQ_SET_NODE_CUSTOM_CONFIG` — 自定义配置

## 六、Worker → Manager 消息发送

```cpp
bool Worker::SendToParent(MsgHead, MsgBody)
{
    // 优先: 共享内存队列
    if (pWorkerToMgrQueue->TryEnqueue(cmd, seq, body, len))
    {
        NotifyEventFd(iWorkerToMgrEfd);
        return true;
    }

    // 降级: socketpair
    SendTo(iManagerControlFd, MsgHead, MsgBody);
}
```

## 七、路由表同步 (shm)

```
Manager 从 Center 收到路由快照:
  OnCenterEvent():
    oSnapshot → SerializeAsString()
    SetNodeNotice(oSnapshot)      // 写入共享内存
      ├─ 先写 blob (protobuf 序列化)
      ├─ 再写 len
      └─ 最后 version++ (原子递增)

Worker 定时检测:
  CheckShareMem()  (ev_idle 回调, 每轮事件循环)
    ├─ IsNodeNoticeVersionChange()?
    │   比较本地记录的 version 和 shm 里的 version
    │
    ├─ 是 → GetNodeNotice() 读 shm
    │       解析 NodeNotice protobuf
    │       更新本地路由表 (mapNodeId)
    │       标记旧节点 offline → 断开 S2S 连接
    │
    └─ 否 → 跳过
```

**原子性保证**: Manager 先写数据再写 version，Worker 先读 version 再读数据。
如果 Manager 正在写 blob，Worker 读到旧 version，下次再读。

## 八、Worker 退出检测

```
Worker IO 回调:
  watcher->fd == iManagerDataFd

  FdTransfer():
    recv_fd_with_attr() 返回 0
    → Manager 已退出 (socketpair 对端关闭)
    → Destroy()
    → exit(2)
```

## 九、完整交互时序

```
时间线:
  Manager                          Worker
  ────────                         ──────
  1. fork() ────────────────────→  Run()
                                     ├─ InitClientListener (监听 access_port)
                                     ├─ ev_run() 进入事件循环
                                     └─ CheckShareMem() 定时检查路由

  2. 收到 Center 路由推送
     OnCenterEvent()
     WriteShm(route) ───────────→  CheckShareMem() 检测到 version 变化
                                   读 shm → 更新路由表

  3. S2S 连接到达
     accept(m_iS2SListenFd)
     send_fd_with_attr() ───────→ FdTransfer()
     close(fd)                     收到 fd → CreateAcceptFdAttr
                                   注册 EV_READ → 开始收发

  4. 配置变更
     SendToWorker(CMD_CONFIG) ──→  收到 controlFd 消息
                                   Dispose(MsgHead) → 更新配置

  5. 客户端请求 (Worker 自己处理)
                                    IoCallback:
                                      watcher->fd == m_iC2SListenFd
                                      → AcceptClientConn()
                                      → RecvDataAndDispose()
                                      → Dispose() → 业务处理
                                      → SendToClient()
```

## 十、跟单进程模型的对比

```
多进程 (当前):                    单进程 (简化后):
  Manager fork Worker              只有 App
  socketpair ×2                    无 IPC
  shm ring queue ×2                路由表: std::shared_ptr + atomic swap
  shm 路由版本                      配置: 文件 watcher (inotify)
  eventfd ×2                       无 fd 传递
  send_fd/recv_fd                  直接内存访问

复杂度: ★★★★★                   复杂度: ★★
调试: gdb attach 两个进程           一个 gdb
部署: ./node.sh start              ./app
```


---

## 十一、为什么 S2S 连接不能 Worker 直接 accept

### 11.1 identify 里带了 worker_index

```
S2S 连接的 identify 格式:
  "127.0.0.1:16068.0"
              ↑      ↑
           port   worker_index
```

Interface 要连 Logic 的 Worker-0，不是随便哪个 Worker。

### 11.2 SO_REUSEPORT 做不到精确路由

```
如果用 SO_REUSEPORT:

  Interface: connect(127.0.0.1:16068)
    → 内核随机选一个 Worker accept
    → 可能连到 Worker-0, 也可能 Worker-1
    → 没法指定 worker_index
    → 路由乱了

Manager accept 方案:

  Interface: connect(127.0.0.1:16068)
    → Manager accept (只有 Manager 监听这个端口)
    → 收到 CMD_REQ_CONNECT_TO_WORKER (worker_index=0)
    → Manager 查 mapSeq2WorkerIndex → 知道要发给 Worker-0
    → send_fd_with_attr(dataFd, fd, ip, codec) → Worker-0 收到
    → 精确路由到指定 Worker
```

### 11.3 完整 S2S 建连协议

```
Step 1: Interface Worker 发起连接
  AutoConnect("127.0.0.1:16068.0"):
    connect(127.0.0.1, 16068)        → TCP 到 Logic Manager
    mapSeq2WorkerIndex[seq] = 0       → 记录 "这个连接是给 Worker-0 的"

Step 2: TCP 连接建立, Interface Worker 发送握手
  connect 成功 → EV_WRITE 触发
  SendTo(CMD_REQ_CONNECT_TO_WORKER, worker_index=0)
    → 告诉 Logic Manager "我要连 Worker-0"

Step 3: Logic Manager 收到握手, 路由到正确 Worker
  Manager::Dispose(CMD_REQ_CONNECT_TO_WORKER):
    解析 worker_index=0
    send_fd_with_attr(dataFd, fd, ip, codec)
      → 通过 Unix socket 把 TCP fd 传给 Worker-0
    Manager 自己 close(fd)           → 不再管这个连接

Step 4: Logic Worker-0 接管连接
  FdTransfer():
    recv_fd_with_attr(dataFd)         → 拿到 fd
    CreateAcceptFdAttr(fd, seq, CODEC_PB_INTERNAL)
    AddIoReadEvent                    → 开始收发 S2S 消息

Step 5: Interface Worker 上也一样（反过来）
  Interface Manager accept Interface Worker 的 connect
    → 收到 CMD_REQ_CONNECT_TO_WORKER
    → send_fd 给 Interface Worker-0
    → Interface Worker-0 接管 fd

现在两端 Worker-0 之间有了 TCP 连接，可以收发 CMD_REQ_GEN_KEY 等业务消息。
```

### 11.4 客户端连接为什么不一样

```
客户端连接 (access_port):
  客户端: curl 127.0.0.1:27006
  不需要指定 worker_index → 连到哪个 Worker 都一样
  SO_REUSEPORT: 内核分发, Worker 直接 accept ✓

S2S 连接 (inner_port):
  调用方: "我要连 Worker-0"
  必须精确路由 → Manager accept + 协议协商 + send_fd ✓
```

### 11.5 一句话

**S2S 需要精确路由到指定 Worker，SO_REUSEPORT 做不到，所以必须 Manager 中转。**

