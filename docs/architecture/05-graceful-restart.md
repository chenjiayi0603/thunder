# Worker 优雅重启 — 零停机热更新

> 源码: `code/Net/src/labor/Worker.cpp` (EnterDrainMode), `code/Net/src/labor/Manager.cpp` (GracefulRestartWorker)
> 唯一实现 **fd 迁移** 的服务器 — 热更新不丢长连接

---

## 竞品对比

```
┌──────────────┬──────────┬──────────┬──────────┬──────────┬─────────┐
│              │  Nginx   │ HAProxy  │  Envoy   │  Apache  │ Thunder │
├──────────────┼──────────┼──────────┼──────────┼──────────┼─────────┤
│ 热更新方式    │ 新子进程  │ 新子进程  │ 新子进程  │ 新子进程  │ 新子进程 │
│ SO_REUSEPORT │    ✅    │    ✅    │    ✅    │    —     │    ✅   │
│ 旧连接排空    │    ✅    │    ✅    │    ✅    │    ✅    │    ✅   │
│ 空闲连接迁移  │    ❌    │    ❌    │    ❌    │    ❌    │    ✅   │
│ WS 不断      │    ❌    │    ❌    │    ❌    │    ❌    │    ✅   │
└──────────────┴──────────┴──────────┴──────────┴──────────┴─────────┘
```

Nginx/HAProxy/Envoy: master 不参与数据面，worker 间无 fd 通道 → 排空后强制 close。
Thunder: Manager 已有 socketpair + SCM_RIGHTS 基础设施 → fd 跨进程迁移。

> 云厂商方案（K8s preStop / AWS NLB drain / Envoy drain）也全是排空模式，长连接必断。TCP 序列号/窗口状态绑在内核 fd 上，跨机器跨 Pod 无法迁移。

---

## 连接处理：三条路径，互不干扰

```
                        新连接 (accept)
                        ──────────────
                        SO_REUSEPORT → 内核直接发给 new Worker
                              │              Manager 不参与 accept
                              ↓
                         new Worker


  old Worker                  Manager                  new Worker
  ──────────                 ───────                  ──────────
      │                          │                         │
      │  send_fd(空闲fd)  →      │  recv_fd → send_fd →   │  ← 路径2: 迁移
      │  SCM_RIGHTS              │  中转, 不读写            │     空闲keep-alive/WS
      │                          │                         │
      │                          │                         │
      │ 在途请求                  │                         │
      │ 排空处理                  │                         │  ← 路径3: 排空
      │ 不迁移                    │                         │     有活跃Step的不动
```

### 路径1: 新连接 — SO_REUSEPORT

内核根据 SO_REUSEPORT 直接将 TCP SYN 分发给 Worker。

```
新客户端 → TCP SYN → 内核 SO_REUSEPORT → new Worker accept
                                           old Worker 不 accept (m_bAccepting=false)
```

### 路径2: 空闲连接迁移 — SCM_RIGHTS

遍历所有连接，满足**三个安全条件**才迁移：

```cpp
for (auto& [fd, conn] : mapFdAttr) {
    // ① pRecvBuff 空 — 无未处理的用户态数据
    // ② pSendBuff 空 — 无待发送响应
    // ③ mapHttpAttr[fd] 空 — 无活跃协程/Step
    if (recvEmpty && sendEmpty && !hasActiveHttpStep) {
        send_fd_with_attr(iManagerDataFd, fd, szRemoteAddr, 32, eCodecType);
        // → Manager 接收 → 转发 new Worker → recv_fd, AddIoReadEvent
        m_pIoBackend->CloseFd(fd);
        mapFdAttr.erase(it);
    }
}
```

SCM_RIGHTS 原理：Unix domain socket 传 fd，内核在接收方创建新 fd 表项指向同一 TCP socket。`close(fd)` 只减引用计数，最后一个引用关闭才发 FIN。

```
old Worker ↔ Manager (dataFd) ↔ new Worker (dataFd)
```

### 路径3: 在途请求 — 排空

有活跃数据/协程的连接不迁移，留在旧 Worker 自然处理完。

```cpp
bool Worker::IsDrainComplete() {
    for (auto& [fd, conn] : mapFdAttr) {
        if (conn->pRecvBuff->ReadableBytes() > 0) return false;
        if (conn->pSendBuff->ReadableBytes() > 0) return false;
    }
    if (!mapCallbackStep.empty()) return false;   // 有活跃协程
    for (auto& kv : mapHttpAttr)
        if (!kv.second.empty()) return false;     // 有HTTP解析状态
    return true;
}
```

超时 30s 后强制退出（`DRAIN_GRACE_PERIOD`）。

---

## 状态机

```
WorkerLifecycle 状态:
  RUNNING → STARTING → DRAINING → NEW_ACTIVE

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

---

## 完整时间线

```
  old Worker            Manager             new Worker
  ─────────             ───────             ──────────
     │ [RUNNING]           │ [RUNNING]          │
     │                     │ 收到重启指令        │
     │                     │ fork+IPC ────────→ │
     │                     │                    │ [STARTING]
     │                     │                    │ InitClientListener(SO_REUSEPORT)
     │                     │                    │ LoadSo, InitIoBackend
     │                     │ ← WORKER_READY ──  │ [RUNNING, 开始 accept]
     │                     │ [DRAINING]         │
     │ ← SIGTERM           │                    │
     │ EnterDrainMode      │                    │
     │   fd迁移空闲连接 →  │ → RecvFdFromWorker │
     │   在途排空          │   → forward →      │ → recv_fd, AddIoReadEvent
     │ ev_run 排空循环     │                    │
     │ [排空完成]           │                    │
     │ Destroy, exit(0)     │                    │
     │                     │ ← SIGCHLD          │
     │                     │ [NEW_ACTIVE]       │ [RUNNING]
```

---

## 数据结构

```cpp
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

// Worker
std::atomic<bool> m_bDraining{false};
std::atomic<bool> m_bAccepting{true};
```

---

## 触发方式

```
etcd 配置变更 (so version变化)  → OnCenterEvent → GracefulRestartWorker(i)
SIGUSR2                         → GracefulRestartWorker(i)
Center Admin API                → NodeRestartWorkers → GracefulRestartWorker(i)
```

---

## 性能数据

```
冒烟测试 (k8s)            ✅ 12/12
k8s Pod 自愈 (Logic kill)  ✅ 31s 恢复
k8s WS 长连接断连验证       ✅ 符合预期
fd转移压测 (40请求)         ✅ 0 失败
Worker优雅重启              ✅ 完整周期
```
