# Thunder Worker 优雅重启

> 2026-06-01 设计 | 2026-06-07 实现 fd 迁移

---

## 零、与其他服务器对比

Thunder 是唯一实现 **fd 迁移** 的服务器 —— 热更新不丢长连接。

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

原理区别:
  Nginx/HAProxy/Envoy: master 不参与数据面, worker 间无 fd 通道 → 排空后强制close
  Thunder: Manager 是数据面中心, 已有 socketpair(send_fd/recv_fd) 基础设施
           → SCM_RIGHTS 传递 fd → 同一 TCP 连接跨进程存活
```

云厂商方案 (K8s preStop / AWS NLB drain / Envoy drain) 也全是"排空"模式, 长连接必断。TCP 序列号/窗口状态绑在内核 fd 上, 跨机器跨 Pod 无法迁移。Thunder 的迁移限于同机同 Pod 内进程间。

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

  1. so 版本变更 (so_path 相同 但 version 不同)
     Manager::OnCenterEvent(ConfigUpdated):
       比较新旧 so 配置的 version 字段
       → 有差异才 GracefulRestartWorker()

  2. module 版本变更
     Manager::OnCenterEvent(ConfigUpdated):
       比较新旧 module 配置的 version 字段
       → 有差异才 GracefulRestartWorker()

  3. 手动触发 (运维)
     Center Admin API:
       POST /admin {"cmd": "restart", "args": ["worker", "0"]}
     → CMD_RESTART_WORKER → Manager → GracefulRestartWorker(0)
```

---

## 四、So/Module 热重载流程对比

### 4.1 配置格式

```json
{
  "so": [
    {"cmd": 101, "path": "plugins/CmdHello.so", "version": 1},
    {"cmd": 102, "path": "plugins/CmdGetToken.so", "version": 3}
  ],
  "module": [
    {"url_path": "/hello/hello", "so_path": "plugins/ModuleHello.so", "version": 2}
  ]
}
```

version 字段表示 .so 文件的版本号。so_path 相同但 version 不同 → 文件内容变更 → 需要优雅重启。

### 4.2 版本比较逻辑

```cpp
bool Manager::HasSoVersionChanged(const util::CJsonObject& oldConf,
                                   const util::CJsonObject& newConf)
{
    // 比较 so 数组里每个条目的 (path, version) 对
    auto oldSo = oldConf["so"];
    auto newSo = newConf["so"];

    if (oldSo.GetArraySize() != newSo.GetArraySize()) return true;

    for (int i = 0; i < oldSo.GetArraySize(); ++i)
    {
        std::string oldPath, newPath;
        int oldVer = 0, newVer = 0;
        oldSo[i].Get("path", oldPath);
        newSo[i].Get("path", newPath);
        oldSo[i].Get("version", oldVer);
        newSo[i].Get("version", newVer);

        if (oldPath != newPath || oldVer != newVer) return true;
    }
    return false;
}
```

### 4.3 现在 (in-place dlopen)

```
配置变更 → Worker::CheckShareMem()
  → LoadSo(new, force=true)
  → dlopen(RTLD_NODELETE)  ← 旧 .so 泄漏
  → mapSo[cmd] = new
  ❌ 旧 .so 内存永不释放
  ❌ 不管 version 是否真的变了, 都是 force reload
```

### 4.4 优雅重启后

```
配置变更 → Manager::OnCenterEvent()
  → HasSoVersionChanged(old, new)?
     否 → 跳过 (version 没变, 不需要重启)
     是 → GracefulRestartWorker(0)
           → fork new Worker (加载新版 .so)
           → old Worker SIGTERM → 排空 → exit
           → OS 回收旧进程全部内存
           ✅ .so 干净卸载
           ✅ 只有版本真正不同才重启
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

## 七、连接处理: 三条路径, 互不干扰

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

### 7.1 路径1: 新连接 — SO_REUSEPORT

Manager 不参与客户端连接的 accept。内核根据 SO_REUSEPORT 直接将 TCP SYN 分发给 Worker。

```
新客户端 → TCP SYN → 内核 SO_REUSEPORT → new Worker accept ✅
                                           old Worker 不 accept (m_bAccepting=false)
```

### 7.2 路径2: 空闲连接迁移 — SCM_RIGHTS

EnterDrainMode 开头遍历所有连接，满足**三个安全条件**才迁移:

```cpp
// Worker.cpp:EnterDrainMode — fd 迁移
for (auto& [fd, conn] : mapFdAttr) {
    // ① pRecvBuff 空 — 无未处理的用户态数据
    // ② pSendBuff 空 — 无待发送响应
    // ③ mapHttpAttr[fd] 空 — 无活跃协程/Step
    if (recvEmpty && sendEmpty && !hasActiveHttpStep) {
        send_fd_with_attr(iManagerDataFd, fd, szRemoteAddr, 32, eCodecType);
        // → Manager::RecvFdFromWorker 接收 → 转发 new Worker
        m_pIoBackend->CloseFd(fd);
        mapFdAttr.erase(it);
    }
}
```

**三个条件同时满足 = 真正空闲**。任何在途请求至少违反一条，留在旧Worker排空。

### 7.3 路径3: 在途请求 — 排空

有活跃数据/协程的连接不迁移。留在旧 Worker 自然处理完。

```cpp
// IsDrainComplete() — 检查所有在途请求是否完成
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

超时 30s 后强制退出 (DRAIN_GRACE_PERIOD)。

---

## 八、状态机 & 触发

```
WorkerLifecycle 状态:
  RUNNING → STARTING → DRAINING → NEW_ACTIVE

触发方式:
  etcd 配置变更 (so version变化)  → OnCenterEvent → GracefulRestartWorker(i)
  SIGUSR2                         → RestartWorkers() → GracefulRestartWorker(i) [待修复]
  Center Admin API                → NodeRestartWorkers → RestartWorkers() [待改为优雅]
```

### GracefulRestartWorker 流程

```
1. 检查 state == RUNNING
2. SpawnSingleWorker → fork new Worker
3. new Worker 初始化 → CMD_WORKER_READY → Manager 收到
4. Manager: state = DRAINING; kill(oldPid, SIGTERM)
5. old Worker: EnterDrainMode → fd迁移 + 排空
6. old Worker exit → Manager: state = NEW_ACTIVE
```

### 完整时间线

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
     │ ...                 │                    │
     │ [排空完成]           │                    │
     │ Destroy, exit(0)     │                    │
     │                     │ ← SIGCHLD          │
     │                     │ [NEW_ACTIVE]       │ [RUNNING]
```

---

## 九、SIGUSR2 修复计划

当前 SIGUSR2 → RestartWorkers() → SIGKILL (硬杀)。应改为调用 GracefulRestartWorker:

```cpp
// Manager.cpp — 当前
else if (SIGUSR2 == watcher->signum) {
    pManager->RestartWorkers();  // → kill(SIGKILL) ❌
}

// 应改为
bool Manager::RestartWorkers() {
    for (auto& [pid, attr] : m_mapWorker) {
        GracefulRestartWorker(attr.iWorkerIndex);  // ✅ 优雅重启
    }
}
```

---

## 十、实际改动量 (2026-06-07 final)

| 改动 | 文件 | 行 |
|------|------|----|
| `EnterDrainMode` fd迁移 | Worker.cpp | +60 |
| `RestartWorkers` → GracefulRestartWorker | Manager.cpp | +8 |
| `SpawnSingleWorker` eventfd修复 | Manager.cpp | -1 |
| `CheckWorker` m_iRefreshInterval=0修复 | Manager.cpp | +2 |
| nginx风格 CLI (`-s reload/restart/stop`) | main.cpp | +50 |
| PID 文件 (`conf/thunder.pid`) | main.cpp | +30 |
| **合计** | | **~150行** |

> 设计文档原估算 ~205行。实际实现更少，因为 fd 迁移只传空闲连接(安全), SO_REUSEPORT 处理新连接, 排空处理在途请求。

---

## 十一、触发方式 (nginx 风格 CLI)

```
  Hello -s reload   → SIGUSR1 → RefreshServer (重载配置)
  Hello -s restart  → SIGUSR2 → GracefulRestartWorker (优雅重启Worker)
  Hello -s stop     → SIGTERM → graceful shutdown
  Hello -s quit     → SIGTERM (同 stop)
```

对照 nginx:

| nginx | 信号 | Thunder CLI |
|-------|------|-------------|
| `nginx -s reload` | HUP | `Hello -s reload` (SIGUSR1) |
| `nginx -s reopen` | USR1 | `Hello -s reload` (SIGUSR1) |
| `nginx -s stop` | TERM | `Hello -s stop` (SIGTERM) |
| `nginx -s quit` | QUIT | `Hello -s quit` (SIGTERM) |
| `kill -HUP` | 手动 | `kill -SIGUSR2` (应急) |

实现: PID 文件 (`conf/thunder.pid`) + `kill(pid, sig)`。

---

## 十二、插件热发布流程

```
正常路径:
  1. 编译新 .so (version 1→2)
  2. etcdctl put /thunder/config "so: [{version:2}]"
  3. Manager 检测 version 变更 → GracefulRestartWorker
  4. new Worker 加载新 .so, old Worker 排空退出
  5. 零停机

应急路径 (etcd挂了):
  1. 编译新 .so, 替换文件
  2. kubectl exec -- kill -SIGUSR2 <pid>
  3. 同上 4-5
```

---

## 十三、测试结果 (2026-06-07 final)

```
冒烟测试 --k8s           ✅ 12/12
k8s Pod 自愈 (Logic kill) ✅ 31s 恢复
k8s WS 长连接断连验证      ✅ 符合预期
fd转移压测 (40请求)        ✅ 0 失败
Worker优雅重启             ✅ 完整周期
单元测试                   ✅ 19/19
```

### 测试脚本

```
tests/test_smoke.sh              # 冒烟 (--k8s 双模式)
tests/test_k8s_scale.sh          # Layer1: k8s Pod 扩缩容/自愈/WS断连
tests/test_graceful_restart.sh   # Layer2: Thunder Worker 优雅重启 (--k8s)
tests/chaos_etcd.sh              # etcd 混沌测试
```

---

## 十四、发现 & 修复的预存 Bug

| Bug | 影响 | 修复 |
|-----|------|------|
| `SpawnSingleWorker` close eventfd 再传参 | Worker→Manager 通知 fd 无效 | 删除 close 行 |
| `CheckWorker` m_iRefreshInterval=0 跳过全部逻辑 | shm 消息永不消费 | 默认值设为 1 |
| `RestartWorkers` SIGKILL 硬杀 | Worker 优雅重启白写了 | 改为 GracefulRestartWorker |

---

## 十五、ReportToCenter

`Manager::ReportToCenter(boRegister)` — etcd 注册 + 节点状态上报。

```
调用链:
  ev_timer (NODE_BEAT=1s) → PeriodicTaskCallback
    → CheckWorker()         ← 消费Worker shm消息
    → ReportToCenter(false) ← etcd keepalive + 负载上报
    → RefreshServer()       ← 重载配置

首次启动: ReportToCenter(true) → etcd lease + registry key
```

上报内容: `node_type`, `node_id`, `node_ip`, `node_port`, `access_ip`, `access_port`, `worker_num`, `workers[{load,connect,recv,send,client}]`

---

## 十六、原理: SCM_RIGHTS

```
send_fd_with_attr(sock, fd, ip, codec):
  Unix domain socket + SCM_RIGHTS, 内核在接收方创建新 fd 表项指向同一 TCP socket
  close(fd) 只减引用计数, 最后一个引用关闭才发 FIN
```

Manager 中转 (Worker 间无直连):
```
  old Worker ↔ Manager (dataFd) ↔ new Worker (dataFd)
```
