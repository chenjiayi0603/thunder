# Worker 出生 60 秒被无声误杀 — 完整调试复盘

> 日期: 2026-06-12 | 关联: #69 #70 #71 #72 | 修复: `Manager.cpp` IoRead 路由 + CheckWorker 引用遍历
> 引入回归的提交: `1d33a9e` (06-02, Worker 优雅重启全链路)

---

## 一、最终结论 (TL;DR)

**每个首次启动的 Worker (gen-1) 都会在出生后整 60 秒被 Manager `SIGKILL` 误杀**，
与负载、IO 后端、包体大小全部无关。压测中观察到的 "asio_uring 64K 大包崩溃" 是伪相关 ——
60 秒死亡时钟恰好落在 64K 测试的时间窗内。

```
因果链 (三层缺陷叠加):

  ① Worker 端  fork 子进程先 CloseEventFd(iWorkerToMgrEfd) 再构造 Worker
               └→ 该函数签名是 CloseEventFd(int& efd)，关闭后置 efd = -1
               └→ Worker::m_iWorkerToMgrEfd = -1
               └→ SendToParent() 的 shm 分支条件 (efd >= 0) 永假
               └→ 心跳被迫走 control socket 回退路径          【#72】

  ② Manager 端 1d33a9e 在 IoRead() 加了:
                 if (m_mapWorkerFdPid.find(fd)) return RecvFdFromWorker(fd);
               └→ 但 m_mapWorkerFdPid 同时含 controlFd 和 dataFd
               └→ controlFd 上的心跳被 recv_fd_with_attr() 当
                  SCM_RIGHTS fd 传递消息读走 → 无 fd → return false
               └→ 心跳字节被吞，无任何日志                    【#70 主因】

  ③ 判定端    dBeatTime 只在启动时初始化一次 (Manager.cpp:212)
               └→ CheckWorker 每 10s 检查 (NODE_BEAT=10)
               └→ now − dBeatTime > worker_beat(60) → SIGKILL
               └→ 出生 + 60s 整，分秒不差
```

**为什么三个多月没人发现**: Worker 被杀后 Manager 在 1ms 内自动拉起新 Worker，服务不中断；
唯一痕迹是一个 defunct 僵尸进程和一行 INFO 日志 `worker_0 pid X is unresponsive, terminate it.`

---

## 二、症状与误导线索

### 2.1 最初的"案发现场" (issue #70 原始记录)

压测 asio_uring 后端 64K 大包时:
- Worker 被 signal 9 杀死，Manager 日志 `error 9: duty <pid> exit and sent signal 17 with code 9`
- 复测有 34 个 wrk timeout，RPS 仅 ~50k
- 64B/256B/1K/4K 都正常，"只有 64K 触发"

三个看似合理、实际全错的怀疑方向:
1. ~~Receive Fast-Path SubmitRead 与 io_uring 在途读的缓冲区竞态~~ (#67 旧怀疑)
2. ~~64K body 跨多次读完成与批量提交交互~~
3. ~~OOM killer~~ (signal 9 的常见来源)

### 2.2 解读 "signal 17 with code 9"

```cpp
// Manager.cpp:255 waitpid 回收循环
else if (WIFSIGNALED(iStatus))  { iReturnCode = WTERMSIG(iStatus); }
LOG4_FATAL("error %d: duty %d exit and sent signal %d with code %d!",
           iStatus, iPid, watcher->signum, iReturnCode);
//          ↑9        ↑pid              ↑17=SIGCHLD       ↑9=SIGKILL
```

iStatus=9 → 低 7 位 = 9 = **SIGKILL**。Worker 是被人 `kill -9` 的，不是自己崩的 ——
**所以没有 core dump、没有调用栈可看**（SIGKILL 不可捕获、不产生 core）。

谁发的 SIGKILL？候选: OOM killer / systemd-oomd / 某进程。
journalctl 无 oomd 记录 → 转向进程内部找凶手 → `grep "kill(" Manager.cpp`:

```cpp
// Manager.cpp:1958  CheckWorker()
if ((ev_now(m_loop) - worker_iter.second.dBeatTime) > m_iWorkerBeat)
{
    LOG4_INFO("worker_%d pid %d is unresponsive, terminate it.", ...);
    kill(worker_iter.first, SIGKILL);     // ← 凶手是 Manager 自己
}
```

沙箱 Manager 日志确认: 每次 "崩溃" 前都有这行 INFO。**凶手锁定，动机待查**。

---

## 三、判别实验链 — 如何一步步排除假说

### 3.1 实验 1: TRACE 日志法医 (排除 Manager 停摆)

打开 TRACE 重跑，Manager 每轮 CheckWorker 打印 `now / dBeatTime / worker_beat`:

```
[16:50:52] now 1781254252.57  dBeatTime 1781254242.569721  worker_beat 60
[16:51:02] now 1781254262.57  dBeatTime 1781254242.569721  ← 冻结
[16:51:12] now 1781254272.57  dBeatTime 1781254242.569721  ← 冻结
...
[16:51:42] now 1781254302.57  dBeatTime 1781254242.569721  → gap=60.005 → SIGKILL
```

两个事实:
- Manager 循环**每 10 秒准点运行** → Manager 没有停摆/卡死
- `dBeatTime` 从某一时刻起**彻底冻结**，60 秒一次都没更新过 → 心跳通道断裂，不是延迟

### 3.2 实验 2: Worker 日志比对 (排除 Worker 假死)

被杀 Worker 的日志，死亡时刻 16:51:42.575:

```
[16:51:32,573] SendToParent()   ← 被杀前 10s 还在发心跳
[16:51:42,573] SendToParent()   ← 被杀前 2ms 还在发心跳!
```

Worker 活蹦乱跳地发心跳，Manager 的 dBeatTime 却冻结 → **丢失发生在传输/接收环节**。

### 3.3 实验 3: 零负载判别 (一票否决所有负载相关假说)

```bash
# 全新实例，不发任何请求，纯空闲观察
worker born 16:55:28  →  killed 16:56:26  (+60s 整, 零负载)
```

**纯空闲也死，且死亡时刻 = 出生 + 60s 整** → asio_uring / 64K / 竞态 / 缓冲区
全部假说一票否决。60 = worker_beat 超时阈值 → dBeatTime 从出生就没被更新过。

> 配套旁证: 部署服务 Manager 14:13:22 启动，现存 Worker 14:14:22 出生 (恰 +60s) ——
> 生产环境的 gen-1 早就被无声杀过一轮，ps 里 09:29 的 defunct 僵尸同理。

### 3.4 实验 4: socket 队列观测 (二分定位丢失环节)

心跳走 socket (后证实)，若 Manager 没读，RecvQ 应堆积:

```bash
ss -xp | grep <mgr_pid>     # RecvQ = 0  → 不是"没人读"，要么没发、要么读了没处理
```

### 3.5 实验 5: strace 从启动跟踪 (锁定吞噬点)

ptrace_scope=1 无法 attach，改为 strace 直接拉起实例 (作为子进程不受限):

```bash
strace -ff -o /tmp/tb-strace/t -e trace=write,read,recvmsg,recvfrom,sendto,epoll_ctl,epoll_wait \
    bin/HelloHttp conf/Hello.json
```

**Worker 侧** — 心跳每 10s 正常写出，编码完整:

```
sendto(9, "\r\5\0\0\0\25Y\0\0\0\35\2P\362\211\nW{\"load\":3,\"conn"..., 104) = 104
#          │ │           │                    └ body: 负载 JSON (87B)
#          │ │           └ seq
#          │ └ cmd = 5 (CMD_REQ_UPDATE_WORKER_LOAD)
#          └ 编解码头
```

**Manager 侧** — 心跳确实到达并被读走，但读法不对:

```
epoll_ctl(5, EPOLL_CTL_ADD, 8, {EPOLLIN, data=0x100000008})   ← controlFd 注册正常
recvmsg(8, {iov=[{iov_len=32}, {iov_len=8}]}, ...) = 40   ┐
recvmsg(8, {iov=[{iov_len=32}, {iov_len=8}]}, ...) = 40   ├ 3 次恰好吃完一拍心跳(104B)
recvmsg(8, {iov=[{iov_len=32}, {iov_len=8}]}, ...) = 24   ┘
```

`iovec[32, 8]` 的特征签名 = `recv_fd_with_attr(fd, remoteAddr[32], &codecType)` ——
**心跳字节被 fd 传递接收函数当垃圾读走了**，没有 SCM_RIGHTS 控制消息 → return false → 无日志。

与 Manager 日志交叉验证: 被杀 Worker 在世的 63 秒里，`DisposeDataFromWorker`
(所有 worker→manager 消息的分发入口) **一次都没执行过**。证据链闭合。

---

## 四、根因代码

### 4.1 #70 主因: IoRead 误路由 (1d33a9e 引入)

```cpp
// Manager.cpp  ── 1d33a9e 之前 (正确):
bool Manager::IoRead(...)
{
    if (watcher->fd == m_iS2SListenFd) return AcceptServerConn(...);
    return RecvDataAndDispose(pData, watcher);          // 一切走解码分发
}

// 1d33a9e 之后 (引入 bug):
    // Worker dataFd: 接收子进程传来的客户端 fd       ← 注释说的是 dataFd
    if (m_mapWorkerFdPid.find(watcher->fd) != end)     // ← 但 map 含两种 fd!
        return RecvFdFromWorker(watcher->fd);          // controlFd 心跳也被吃
    return RecvDataAndDispose(pData, watcher);
```

`m_mapWorkerFdPid` 的插入点 (CreateWorker / RestartWorker 均如此):

```cpp
m_mapWorkerFdPid.insert({stWorkerAttr.iControlFd, iPid});   // 心跳/协议消息通道
m_mapWorkerFdPid.insert({stWorkerAttr.iDataFd,    iPid});   // SCM_RIGHTS fd 传递通道
```

### 4.2 #72 帮凶: fork 时关错 eventfd (心跳被逼上 socket)

```cpp
// SpawnSingleWorker 子进程分支:
ShmRingQueue::CloseEventFd(iWorkerToMgrEfd);   // ← 关掉自己作为"生产者要写"的 efd
                                               //    且该函数置 int& 为 -1
Worker* pWorker = new Worker(..., iMgrToWorkerEfd, iWorkerToMgrEfd /* = -1 */);

// Worker::SendToParent:
if (m_pWorkerToMgrQueue && m_iWorkerToMgrEfd >= 0)   // -1 → 永假
    { TryEnqueue(...); ... }                          // shm 快速路径从未用过
// ↓ 全部消息走 socket 回退 → 撞上 4.1 的误路由
```

eventfd 是单一内核对象、读写双方都要持有 —— socketpair 的"各关对端"模式不适用。
父进程同样关错: `CloseEventFd(iMgrToWorkerEfd)` 关掉了自己通知 Worker 要写的端。
ShmRingQueue.hpp 设计注释里的 "生产者写完写 eventfd" 机制**从未生效过**，
Worker→Manager 退化为 socket，Manager→Worker 退化为 1 秒轮询 (CheckShareMem)。

### 4.3 次要缺陷: CheckWorker 按值遍历

```cpp
for (auto worker_iter : m_mapWorker)        // ← 按值拷贝
{
    ... drain 共享内存队列 → SetWorkerLoad 更新【map 里】的 dBeatTime ...
    if ((ev_now - worker_iter.second.dBeatTime) > m_iWorkerBeat)   // ← 读的是【拷贝】
        kill(..., SIGKILL);                 // 超时判定用的是 drain 之前的过期快照
}
```

---

## 五、修复与验证

### 5.1 修复 (Manager.cpp, 19 行)

```cpp
// ① IoRead: 仅 dataFd 走 RecvFdFromWorker, controlFd 恢复解码分发
auto fd_pid_iter = m_mapWorkerFdPid.find(watcher->fd);
if (fd_pid_iter != m_mapWorkerFdPid.end())
{
    auto worker_iter = m_mapWorker.find(fd_pid_iter->second);
    if (worker_iter != m_mapWorker.end()
        && worker_iter->second.iDataFd == watcher->fd)     // ← 精确到 dataFd
    {
        return(RecvFdFromWorker(watcher->fd));
    }
}
return(RecvDataAndDispose(pData, watcher));

// ② CheckWorker: for (auto worker_iter : ...) → for (auto& worker_iter : ...)
```

#72 (eventfd) 影响性能不影响正确性，血量大，单独记 issue 另行修复。

### 5.2 验证矩阵 (全部通过)

| 测试 | 修复前 | 修复后 |
|------|:------:|:------:|
| gen-1 纯空闲存活 135s | ❌ +60s 整必死 | ✅ 存活, 0 误杀 |
| kill -9 后 gen-2 带 wrk 负载存活 90s | — | ✅ 存活, 0 误杀 |
| 心跳处理 (TRACE 取证, 35s 窗口) | 0 条 | ✅ 每 10s 一条 cmd-5 |
| ctest 单元测试 | — | ✅ 335/335 |
| wrk 64B 性能 | 230k (下午热态) | ✅ 204k (晚间, 同量级) |
| 冒烟 `/hello/raw` | — | ✅ `{"code":0,"msg":"ok"}` |

### 5.3 遗留事项

- **部署服务 (root 启动, 14:13) 仍运行旧二进制** — 需重启加载修复:
  `sudo bash deploy/HelloHttp/node.sh restart`。其他节点 (Logic/Interface/Https/Ws)
  同样受影响 (共用 libNet 的 Manager)，需一并重启
- #72 eventfd 修复后, asio_uring 64K 的 ~50k RPS (vs ev 70k) 需重测确认是否仍偏低
- ModuleLuaHttp 在当前 shell 环境缺 lua.hpp 编不过 (与本修复无关, .so 用旧产物)

---

## 六、方法论沉淀

1. **"signal 9 + 自动重启" ≠ 崩溃**。先分清 SIGSEGV(有 core 可看栈) 与
   SIGKILL(外部强杀, 查发送者)。`grep "kill(" ` 自家代码常常就是凶手。
2. **零负载判别实验性价比最高**。一次空跑直接否决了竞态/大包/OOM 全family假说,
   把"偶发崩溃"变成"确定性 60 秒定时死亡" —— 确定性 bug 比偶发 bug 好查一个数量级。
3. **时间指纹是强证据**: "+60s 整" 直指超时阈值; "Manager 14:13:22 / Worker 14:14:22"
   直接证明生产环境同样中招。
4. **静默吞噬要靠 strace 包抄**: 三层 if 各有一个无日志的 return/丢弃分支
   (SendToParent shm 条件、drain 的 mapFdAttr 查找、RecvFdFromWorker 失败)，
   日志只能证明"没发生"，strace 才能证明"字节去了哪"。
   `iovec[32,8]` 这种 syscall 参数形状是函数级指纹。
5. **伪相关警惕**: 重启实例的压测脚本以 ~66s 为周期, 与 60s 死亡时钟形成"差拍",
   杀戮总落在每轮最后一个测试项 (64K) 上 —— 相关不等于因果, 改变实验周期即可识破。
6. **回归二分用 `git log -S`**: `-S "RecvFdFromWorker"` 一发命中引入提交 1d33a9e。
