# Manager.cpp 职责分析与架构优化

> 2026-06-01 | 基于源码分析

---

## 一、Manager 当前职责 (2732 行, 全混在一起)

```
Manager 做了 7 件事:

  1. 进程管理 (fork Worker, fork Loader, restart Worker, SIGCHLD)
     ── CreateWorker, CreateLoader, RestartWorker, OnChildTerminated

  2. S2S 连接管理 (accept inner_port, 协议协商, 投递给 Worker)
     ── AcceptServerConn, send_fd_with_attr, CMD_REQ_CONNECT_TO_WORKER

  3. Center 通信 (注册/心跳/上报)
     ── ReportToCenter, OnCenterEvent, TcpCenterConnector

  4. 共享内存写入 (路由表/配置)
     ── SetNodeNotice, SetCustomConfig, SetServerConfigFile

  5. IO 事件循环 (跟 Worker 一样的 ev_loop + epoll + callback)
     ── IoCallback, IoRead, IoWrite, IoTimeout, RecvDataAndDispose

  6. 消息分发 (跟 Worker 类似的 Dispose 逻辑)
     ── DisposeDataFromWorker, SendToWorker

  7. 杂项 (日志、配置解析、信号处理)
```

### 为什么乱

每一件事都直接写在 Manager.cpp 里，没有分层。比如：
- `CreateWorker()` 里同时做了 socketpair、shm ring queue、fork、Worker 构造
- `OnCenterEvent()` 里同时做了 node_id 更新、路由 shm 写入、Worker 通知
- `IoCallback` 除了 IO 还要管 S2S listen fd 的特殊处理

---

## 二、Load 分析结果

### Loader: 不是死代码, 但没启用

```
Loader 继承 Worker, 只做一件事:
  每1秒读 JSON 配置文件 → 变了就写 shm → Worker 的 CheckShareMem 拉取

跟 Center 推送配置是两套平行的配置同步机制:
  Center 推送: Center → Manager → shm → Worker       (生产在用)
  Loader 轮询:  本地文件 → Loader → shm → Worker      (默认关闭)

所有配置文件中都没搜到 "loader_process" → 当前部署没启用
```

### Manager/Worker 共享 13 个 IO 函数

```
AddIoReadEvent, AddIoWriteEvent, RemoveIoWriteEvent, DestroyConnect,
IoRead, IoWrite, IoError, IoTimeout, IoCallback,
RecvDataAndDispose, HandleIoReadComplete, HandleIoWriteComplete, OnIoComplete

根因: Manager 和 Worker 都需要处理 IO 事件, 但各自实现了相同的逻辑
区别: 只在 watcher data 类型不同 (tagManagerIoWatcherData vs tagIoWatcherData)
```

---

## 三、真正的问题: 关注点没有分离

### 3.1 进程管理应该独立

```
现状: CreateWorker() 里混着:
  - socketpair 创建
  - shm ring queue 创建
  - fork
  - Worker 构造
  - fd 注册到 libev
  → 80 行, 没法单独测试

应该:
  class WorkerProcessManager {
      struct WorkerProc { int pid, controlFd, dataFd; ShmRingQueue* queue; };
      WorkerProc Spawn(int index);    // fork + IPC setup
      void Kill(int pid);            // SIGKILL + waitpid
      void OnChildDeath(int pid);    // restart logic
  };
```

### 3.2 S2S 连接代理应该独立

```
现状: AcceptServerConn + send_fd_with_attr + CMD_REQ_CONNECT_TO_WORKER
  混在 Manager 的 IO 回调里

应该:
  class S2SConnectionBroker {
      void OnAccept(int listenFd);                    // accept + 读握手消息
      void RouteToWorker(int fd, int workerIndex);    // send_fd
  };
```

### 3.3 Center 通信应该独立 (已有 TcpCenterConnector)

```
现状: TcpCenterConnector 已独立, 但 OnCenterEvent 还在 Manager 里处理 shm 写入
  → 这部分也可以移进 TcpCenterConnector 或单独的路由同步类
```

### 3.4 IO 事件循环应该跟 Worker 共享

```
现状: Manager 和 Worker 各有一套 IO 处理代码 (13 个重复函数)

应该: 提取到 Labor 基类, Manager 和 Worker 只处理各自的特殊逻辑:
  - Manager: S2S accept → send_fd
  - Worker:  客户端 accept + 完整业务处理
```

---

## 四、优化建议 (按收益排序)

### P0: 拆分大函数 (不改架构, 只拆逻辑)

```
CreateWorker (80行) → SpawnWorker (30行) + SetupIPC (30行) + RegisterToLoop (20行)
AutoSend (100行)    → DoConnect (35行) + CachePending (20行) + OnConnectSuccess (25行) + SendTo (20行)
```

**收益**: 可读性, 每个函数可单独理解
**风险**: 低 (逻辑不变, 只拆函数)

### P1: 提取 IO 基类 (去重 13 个函数)

```
Manager 和 Worker 共享的 IO 逻辑 → Labor 基类
tagIoWatcherData 改模板 → tagIoWatcherDataT<T>
```

**收益**: 改 IO 逻辑只需改一处
**风险**: 中 (涉及继承结构调整)

### P2: 进程管理独立

```
CreateWorker + RestartWorker + OnChildTerminated → WorkerProcessManager 类
```

**收益**: Manager 减 ~300 行
**风险**: 中

### P3: S2S 连接代理独立

```
AcceptServerConn + send_fd + CMD_REQ_CONNECT_TO_WORKER → S2SConnectionBroker 类
```

**收益**: Manager 减 ~100 行, 逻辑清晰
**风险**: 中

### 不做的事

```
- 省 100 行模板代码 (callback 命名好维护, 不值得)
- 统一 SendTo 重载 (3 个重载各自有使用场景)
- Loader 删除 (虽然没启用, 但有使用场景)
```

---

## 五、优化后预期

```
当前 Manager.cpp:  2732 行, 7 个职责混在一起

P0 后 (拆大函数):     ~2700 行  每个函数 <30 行
P1 后 (提 IO 基类):   ~2300 行  消除 13 个重复函数
P2 后 (进程管理):     ~2000 行  进程管理独立
P3 后 (S2S 代理):     ~1900 行  连接管理独立

最终:
  Manager: ~1200 行 (Center通信 + 配置同步 + 协调)
  WorkerProcessManager: ~400 行
  S2SConnectionBroker: ~200 行
  Labor (IO 基类): 新增 ~300 行
```

## 六、Loader 分析

```
位置: code/Net/src/labor/Loader.cpp (175行)
继承: Loader : public Worker

作用:
  - Manager fork() 出的独立进程
  - 每1秒扫描 JSON 配置文件
  - 配置变化 → 写入 shm (LoaderConfigVersionData)
  - Worker::CheckShareMem() 检测到 → 热加载配置

跟 Center 推送的关系:
  - 两套平行的配置同步机制
  - Center 推送: 运维平台 → Center → Manager → shm → Worker
  - Loader 轮询: 手动编辑 JSON 文件 → Loader → shm → Worker

状态: 默认关闭 (loader_process: false), 当前部署未启用
建议: 保留, 不需要优化 (175行, 职责单一, 逻辑清晰)
```

