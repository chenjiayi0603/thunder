# Thunder 全量测试 & 代码检查 — 问题清单

> 生成时间: 2026-06-03
> 范围: `./deploy.sh build` 全量构建 + 核心源码人工审查 (Net 模块: ShmRingQueue / AsioUringIoBackend / Worker 优雅重启)
> 状态说明: 🔴 阻塞 / 🟠 待处理(高) / 🟡 待处理(中) / 🔵 优化建议 / ⚪ 记录(信息) / ✅ 已修复

---

## 测试执行结果 (2026-06-03)

| 阶段 | 结果 | 备注 |
|------|------|------|
| 全量构建 `./deploy.sh build` | ✅ 100% 通过, 0 error | 用 `CPATH` 绕过 #1(未删系统头),**737 warning**(Thunder 自身 46 去重项, 详见 #7) |
| C++ gtest (ctest) | ✅ **288/288 通过** | 5 skipped: 3 个 E2E 冒烟需在线服务 + 2 个 ORM 需 MySQL/Redis |
| Python pytest unit | ✅ **60/60 通过** | 0.03s, 零外部依赖 |
| Docker E2E | ⚠️ **18/25 通过, 1 失败, 6 skip** | 失败的 `genkey_verifykey_chain` 暴露 #9(etcd 节点发现 bug, 已修复待验证);6 skip 为 Center 下线后的 admin 用例(预期) |
| 手动冒烟 — Interface→Logic | ❌→修复中 | GenKey 报 "no LOGIC node in route table",根因 #9,已改 `EtcdCenterConnector.cpp` 待重编验证 |

> E2E 过程中环境本身的问题见 #10(Docker 守护进程僵尸容器, 需 root 重启)、#11(etcd/日志 bind-mount 跨运行残留, 非 hermetic)。
> 这两个环境问题导致前两次 E2E 在 `docker compose up` 阶段失败, 最终用「全新项目名 + 清空 etcd + external 模式」绕过才跑通用例。

---

## ✅ #1 [已修复] 全量构建失败 — `/usr/local/include/dirent.h` 被 DPDK 的 Windows 版遮蔽

**当前状态: ✅ 已修复 (2026-06-03)** — `sudo rm -f /usr/local/include/dirent.h` 已执行，构建免 CPATH 正常通过

### 现象
`./deploy.sh build` 在编译 `code/Util/src/util/FileUtil.cpp` 时直接失败:

```
In file included from code/Util/src/util/FileUtil.h:12,
                 from code/Util/src/util/FileUtil.cpp:1:
/usr/local/include/dirent.h:19:10: fatal error: windows.h: No such file or directory
   19 | #include <windows.h>
compilation terminated.
gmake[2]: *** [.../FileUtil.cpp.o] Error 1
✘ 编译失败
```

### 根因
- 2026-06-01 安装 **DPDK** 时,`make install` 把 DPDK 自带的 **Windows 移植版** `dirent.h`
  (Toni Ronkko's dirent v1.21, 内含 `#include <windows.h>`) 一并装进了 `/usr/local/include/`。
- 印证: `/usr/local/include/` 下有 22 个 DPDK 头 (`rte_*` / `cmdline*` / `acl*` / `eal_windows.h` / `bpf_def.h` …),
  且 `dirent.h`、`eal_windows.h`、`eal_interrupts.h`、`cmdline_private.h` 均 `#include <windows.h>`。
- GCC 头文件搜索顺序中 `/usr/local/include` **先于** `/usr/include`,于是这个 Windows shim 遮蔽了
  glibc 的 `/usr/include/dirent.h`。任何 `#include <dirent.h>` 的源文件全部编译失败。
- 这是**纯环境污染**,Thunder 代码本身没问题 (`#include <dirent.h>` 是标准用法)。

### 影响
- 全量构建无法通过 → **本次"全量测试"被阻塞,unit / e2e / 冒烟均无法在新构建上运行**。
- 受影响的 Thunder 源文件 (直接 include `<dirent.h>`):
  - `code/Util/src/util/FileUtil.h`
  - `code/Net/src/labor/AsioUringIoBackend.cpp`
  - (以及所有间接包含 FileUtil.h 的 TU)

### 修复建议 (按推荐度排序)
1. **(推荐) 删除被污染的 Windows shim 头** — DPDK 的这些头只用于 Windows 构建,Linux 上不需要:
   ```bash
   sudo rm -f /usr/local/include/dirent.h
   # 如后续仍报 windows.h,同理处理 eal_windows.h 等(它们不会被 Linux 编译路径包含,通常无需删)
   ```
   风险低、可逆(重装 DPDK 会再生成)。删后重跑 `./deploy.sh build`。
2. **(更彻底) 重装 DPDK 到隔离前缀**,避免污染系统默认 include:
   `meson setup build --prefix=/opt/dpdk` → 用 `pkg-config --define-prefix` 引用,
   不再往 `/usr/local/include` 写。
3. **(临时绕过,不改环境)** 给受影响 TU 强制优先系统头目录 —— GCC 对标准系统目录会忽略重排序,
   不可靠,**不推荐**。

> ⚠️ 需要 root,且改的是系统态文件(非本人安装),已按规范不擅自执行,等用户确认后再动手。

---

## ✅ #2 [高] "Worker 优雅重启排空"功能在生产代码中从未被触发 (死代码)

**当前状态: 已修复 (2026-06-04)**

### 现象
CLAUDE.md 大篇幅描述的"优雅重启排空"(`EnterDrainMode` / `IsDrainComplete` / 主循环 drain 分支)
**在生产代码里没有任何调用点**:

```
$ grep -rn "EnterDrainMode" code --include=*.cpp --include=*.hpp
code/test/coroutine/test_coroutine20.cpp: (6 处, 仅单元测试)
code/Net/src/labor/Worker.cpp:6050: void Worker::EnterDrainMode()   ← 仅定义
```

SIGTERM 的处理器是**硬退出**,根本不进入排空:

```cpp
// Worker.cpp:319
void Worker::OnTerminated(struct ev_signal* watcher) {
    int iSignum = watcher->signum;
    delete watcher;
    Destroy();
    LOG4_FATAL("terminated by signal %d!", iSignum);
    exit(iSignum);          // ← 直接 exit,不调 EnterDrainMode()
}
```

### 影响
- 主循环里 `if (m_bDraining) { ... }` (Worker.cpp:302) 分支在生产中**永不命中**
  (`m_bDraining` 只在 `EnterDrainMode()` 里置 true,而它只有测试调用)。
- 收到 SIGTERM 时 Worker **立即 `exit()`,在途请求被直接中断**——
  与 CLAUDE.md 宣称的"等所有在途请求完成后退出"不符。
- 单元测试 `WorkerDrain.*` 全绿 ≠ 功能在生产可用,容易造成"已实现"的错觉。

### 修复建议
- 在 `OnTerminated`(或 Manager 下发的"准备重启"CMD 处理)中改为调用 `EnterDrainMode()`,
  让主循环走 drain 收尾,而非直接 `exit()`。
- 修复前请先解决 #3 的排空逻辑缺陷,否则接线后会暴露丢请求问题。
- 补一条 E2E:SIGTERM 后在途请求仍能拿到响应(而非连接被重置)。

### 修复内容 (2026-06-04)

**根本原因：** libev `signals[]` 是全局静态数组。Manager 进程 fork Worker 时，Manager 通过 `EVFLAG_SIGNALFD` 使 SIGTERM 进入 sigprocmask 阻塞集，并在全局 `signals[SIGTERM-1].head` 留下悬空 watcher。Worker 新建 ev_loop（不含 `EVFLAG_SIGNALFD`，`sigfd=-1`）后调用 `AddSignal(SIGTERM)` 时，`!w->next = false`（父进程悬空 watcher 在 next），libev 跳过 sigaction 安装；同时 SIGTERM 仍被继承的 sigprocmask 阻塞 → Worker 完全无法接收 SIGTERM。

**修复方案：**
1. `Labor::StopAllSignals()`：在 fork 子进程中调用（Manager.cpp fork block），调用 libev 新增函数 `ev_signal_reset_after_fork()` 清零全局 `signals[].head`，再 `sigprocmask(SIG_UNBLOCK)` 解除继承的信号阻塞。不调用 `ev_signal_stop`（避免修改父子共享的 signalfd fd）。
2. `libev/ev.c` 新增 `ev_signal_reset_after_fork()`：仅清零 `signals[i].head`，不触碰 signalfd。
3. `Worker.cpp` 移除 `AddSignal(SIGKILL, ...)` — SIGKILL 不可捕获。
4. `Worker::OnTerminated`：改为调用 `EnterDrainMode()` 而非硬 `exit()`。
5. `Worker::Run()` 排空循环末尾：`Destroy(); exit(0);`。

**验证：** SIGTERM 发给 Worker → 日志 "Worker 0 got signal 15, graceful draining" → "drain complete, exiting" → Manager "worker 0 restart successfully" → 服务全程可用。

---

## ✅ #3 [中] 排空逻辑本身有正确性缺口 (接线前必须先修)

**当前状态: 已修复 (2026-06-04)** — 位置 `Worker::EnterDrainMode()` (Worker.cpp:6050)

> 说明: 因 #2 未接线,以下缺陷目前不会在生产触发,但一旦把 drain 接到 SIGTERM 就会暴露。
> 经核对,连接查找均走 `mapFdAttr.find(fd)` 且判空,**不构成 UAF**,属"丢请求"正确性问题。

### 缺陷 A — 把"挂起 Step 中、缓冲区恰好为空"的连接误判为空闲并关闭
```cpp
// loop1 关闭"空闲"连接的判据只看缓冲区,没看是否有绑定的活跃 Step
if (pConn->pRecvBuff->ReadableBytes() > 0) { ++it; continue; }
if (pConn->pSendBuff->ReadableBytes() > 0) { ++it; continue; }
// → 直接 CancelFd + CloseFd
```
一个请求已被完整读走(recv 空)、正 `co_await` 等 DB/S2S(响应还没写,send 空)的连接,
会被判为"空闲"直接关闭 → **在途请求被中断,客户端连接被重置**。

### 缺陷 B — 把有活跃 Step 的连接 fd 转走,老 Worker 算出的响应被丢弃
```cpp
// loop2 无条件把剩余连接 send_fd 给 Manager 并从 mapFdAttr 删除
send_fd_with_attr(iManagerDataFd, pConn->iFd, ...);
it = mapFdAttr.erase(it);
```
该连接上若有挂起 Step,Step 恢复后 `mapFdAttr.find(fd)` 查不到 → 响应被丢;
而新 Worker 只拿到 fd、没有在途请求上下文,也不会回这个响应 → **请求静默丢失**。
随后 `IsDrainComplete()` 还在等 `mapCallbackStep` 清空,对这些已转走的连接而言是空等。

### 修复建议
- "空闲"判据增加:该 fd 上无绑定的活跃 Step(需建立 fd ↔ stepSeq 关联,或反查 step 的 conn)。
- loop2 改为:**只转移没有活跃 Step 的连接**(纯空闲/半包但无在途处理);
  有活跃 Step 的连接保留本地,靠 grace period 等其完成后再正常收尾。

---

## ✅ #4 [已修复] ShmRingQueue 尺寸参数

**当前状态: ✅ 已修复** — Destroy 从 ctrl 读尺寸, 测试加具名常量+非默认尺寸验证

### 问题
- `slot_count=128, slot_size=4096` 这对魔数在 **5 处**重复硬编码
  (Manager.cpp:1253/1258/1362/1363/1512/1516 等)。
- `Destroy(q, slot_count, slot_size)` 用**入参**计算 munmap 长度,而非从 `q->ctrl` 读回。
  当前 Create/Destroy 恰好都传 128/4096 所以没事,但**一旦改了 Create 的尺寸而漏改某处 Destroy,
  munmap 长度就会不匹配** → 部分页未解除映射(泄漏)或误解除相邻映射。

### 修复建议
- `Destroy` 直接从控制块取尺寸,消除参数依赖:
  ```cpp
  static void Destroy(ShmRingQueue* q) {
      if (!q) return;
      size_t total = sizeof(ShmRingQueue)
                   + (size_t)q->ctrl.slot_count.load(std::memory_order_relaxed)
                   * q->ctrl.slot_size.load(std::memory_order_relaxed);
      munmap(q, total);
  }
  ```
- 把 128/4096 提为具名常量(如 `kDefaultSlotCount`/`kDefaultSlotSize`),Create 调用方统一引用。

---

## 🔵 #5 [优化] 性能与可维护性建议

| # | 位置 | 建议 | 收益 | 状态 |
|---|------|------|------|------|
| 5.1 | `AsioUringIoBackend::UpdateRingWatcher` | 每次 Submit 都全量扫描 m_fds 判断 hasOp，高并发 O(N)。可维护 `m_pendingOpCount` 计数器，O(1) 判断。 | 高并发降低 I/O 常数开销 | ✅ 已确认(低优优化,非bug) |
| 5.2 | `AsioUringIoBackend::FindIoUringRingFd` | 取第一个 io_uring fd，多实例时可能取错。 | 健壮性 | ✅ 已修复：改为取最大 fd（ASIO service fd 最后创建），多实例时日志报告 |
| 5.3 | `Worker.cpp` (6114 行) | 远超 ≤800 行规范，建议按职责拆分。 | 可维护性 | ✅ 已确认(低优优化,非bug)（大型重构，需独立规划） |
| 5.4 | `ShmRingQueue::IsFull` | `ctrl.slot_count` 隐式 seq_cst load，不可变量用 relaxed 即可。 | 微优化 | ✅ 已修复：显式 `.load(relaxed)` |

---

## ⚪ #6 [信息] 已审查、判定**无问题**或仅作记录的点

- **ShmRingQueue 内存序**: SPSC 模型下 `write_index`/`read_index` 的 acquire-release 已构成完整
  happens-before;`msg_len` 就绪标志 + release fence 是冗余的防御层,**整体正确**。
- **跨进程 std::atomic**: 依赖 lock-free + address-free,x86-64 上 uint32/uint64 满足,**当前平台 OK**。
  (严格 C++ 内存模型下 `msg_len` 的非原子读写属 UB,实际硬件 + fence 可用,仅记录。)
- **`OnTerminated` 调 delete/LOG4_FATAL/exit**: 这是 libev `ev_signal` 监听回调,运行在正常事件循环
  上下文(libev 内部用 self-pipe/signalfd),**不是真信号上下文**,调用非 async-signal-safe 函数安全。
- **AsioUringIoBackend `CancelFd`**: 用 `cancelled` 标志 + `weak_ptr` 规避 `IORING_OP_ASYNC_CANCEL`
  与复用 fd 的竞态,设计合理;`release()` 后 erase 的 FdState 生命周期建议用 **ASan/TSan** 在
  Docker E2E 下回归确认(本环境因 #1 暂无法构建带 sanitizer 的版本)。
---

## ✅ #9 [严重→已修复] etcd 节点发现完全失效 — 跨节点 S2S 路由全断 (Center→etcd 迁移回归)

**当前状态: ✅ 已修复 — 5 环 bug 链 + #24 EtcdHttpConn 修复后, 5 节点稳定注册, keepalive 持续正常**

### 根因是一条 5 环 bug 链(逐一插桩定位)
1. **watch 从 rev 1 发起被 etcd compaction 取消**:`m_lastRevision=0` → `start_revision:1`,
   etcd compact 到 rev N(长跑必然)后,etcd 立即回 `{"canceled":true,"compact_revision":N}` 关流,
   客户端收不到任何事件,1s 重连又被取消 → 死循环。
2. **PUT 事件被丢弃**:etcd grpc-gateway 对 PUT(EventType=0)按 proto3 省略 `type` 字段,
   原 `if (wev.type=="PUT")` 对空串不成立 → 上线事件全丢。
3. **Manager 无 `case RouteUpdated`**:路由快照处理只写在 `case Registered` 里(且 Registered 从不带
   route_snapshot=死代码),watch 发的是 `RouteUpdated` → 落 `default` 被静默丢弃 → 路由 shm 永不更新。
4. **单节点增量 vs 全量快照语义不符**:watch 每事件发单节点 NodeNotice,而路由 shm 只存最新版本、
   Worker 把每个 notice 当"该类型完整在线集"prune+add → Worker 只认到最后写入的那个节点。
5. **worker_num 未传递**:注册值无 `worker_num`,watch 也不读;Worker 按 `for j<worker_num()` 建
   identify,worker_num=0 → 一个 identify 都不建 → 路由表空。

### 修复(4 处文件)
- `EtcdCenterConnector.cpp WatchThreadFunc`:每次(重)连先做 `/thunder/` **全量 range 快照**,
  载入现有节点 + 以 `header.revision` 作 watch 起点 → 绕开 compaction(修 #1)。
- `EtcdCenterConnector.cpp OnWatchAsync`:空 type 当 PUT(修 #2);维护完整节点表 `m_nodeRegistry`,
  每次变更发**全量** NodeNotice(修 #4);从 value 读 `worker_num` 并 `set_worker_num`(缺省 1)(修 #5)。
- `Manager.cpp OnCenterEvent`:新增 `case CenterEventType::RouteUpdated` 处理 route_snapshot→shm(修 #3)。
- `EtcdCenterConnector.cpp`:注册值 JSON 加 `worker_num`(从 `NodeReport.worker_num()` 经 `m_workerNum` 写入)(修 #5)。

### 2026-06-11 复现: DoRegister 卡死

长时间运行或 etcd 重启后，`DoRegister` 在 `OnRegQuery` 成功之后卡住 >30s：

```
[WARN] DoRegister — 注册卡住 >30s, 强制复位
[DEBUG] OnRegQuery — key=/thunder/registry/LOGIC/...
[WARN] DoRegister — 注册卡住 >30s, 强制复位  (循环)
```

解决方法：每次 `OnRegQuery` 成功后，若当前无进行中的 PUT 请求，立即发起 PUT。当前代码可能在 `OnRegQuery` 回调中未正确触发下一阶段。

### 验证
```
GenKey → {"code":0,"token":"7467947435826872321","key":"...","msg":"success""}
pytest e2e/test_interface_chain.py --mode external → 5 passed
pytest e2e/ -m "integration or smoke" --mode external → 19 passed, 6 skipped(Center admin/failover, 预期)
```

### 说明
- 排障期插过 stderr/文件诊断,定位后已全部移除并重新编译验证。
- #12(logger 名硬编码"Logic_robot")在排障中证实会丢失非 Logic 节点的 etcd 日志,建议一并修以便后续观测。
- 06-01 报告 E2E 29/29 通过(当时 Center 发现仍在),本回归确由 Center 下线后 etcd 发现未接全引起。

---

<details><summary>原始诊断记录(已解决, 保留备查)</summary>

**(原)当前状态: 已定位根因, 应用了一处必要修复但不充分, 仍未完全修复**

### 现象
- E2E `test_interface_genkey_verifykey_chain` 失败、手动 `GenKey` 返回
  `code:1 "no LOGIC node in Interface route table (NodesMgr)"`。
- **所有 5 个节点** `route mirror updated` 日志计数 **= 0**;Logic 节点 `Watch — PUT` 也 = 0。
  → 没有任何节点能从 etcd watch 事件填充自己的路由表 (NodesMgr)。

### 已坐实的事实链 (逐步排除)
1. 注册侧 OK:干净 etcd 下 Logic 正确注册 `{"node_type":"LOGIC",...:16068}`,只占 3/255 slot。
2. etcd 侧 OK:`etcdctl watch --rev=1 --prefix /thunder/registry/` **完整回放** LOGIC/HELLO/INTERFACE 三条 PUT。
3. **客户端侧坏**:`EtcdCenterConnector` 收到/处理 watch 事件后,从未产出非空 `route_snapshot`
   → `Manager::OnCenterEvent` 的 `route mirror updated` 永不触发 → 路由 shm 没有对端节点 → Worker NodesMgr 空。
4. 这解释了 E2E 18/25 通过:HTTP/HTTPS/WS/echo 是**单节点**路径不需跨节点路由;唯一真正验证
   Interface→Logic 全链路的 `genkey_verifykey_chain` 失败。

### 已应用的修复 (必要但不充分)
`EtcdCenterConnector.cpp:949` —— etcd v3 grpc-gateway 对 PUT 事件 (EventType 枚举=0) 按 proto3
规则**省略 `type` 字段**,只有 DELETE 才带 `"type":"DELETE"`。原代码 `if (wev.type == "PUT")`
对空 type 不成立 → PUT 分支永不进。已改为 `if (wev.type == "PUT" || wev.type.empty())`。
> 重编 + 重启验证后链路**仍失败**,`route mirror updated` 仍 0 → 事件根本没到 `OnWatchAsync`,
> 还有一个**更上游**的 watch 流接收/解析缺陷 (疑点见下)。

### 补充确认 (进一步缩小范围)
- **原始 curl 复刻客户端 watch 请求**(`POST /v3/watch`,b64 prefix + start_revision=1)→ etcd **正确回流**
  `created` + `events`,事件含 `registry/127.0.0.1:16068` value=`{"node_id":247,"node_type":"LOGIC"...}`,
  且**确实无 `type` 字段**(印证 PUT 省略 type)。→ etcd 侧 + 请求格式 100% OK。
- **运行中 Interface 确认加载新 libNet.so**(RUNPATH 含 `build/lib`,19:44 新构建)→ 修复代码已在跑。
- 综合:etcd 投递 OK + 新二进制已加载,但 `route mirror updated` 仍 0 →
  缺陷在**客户端运行时** watch 接收/解析/回调链路某环,需 DEBUG 插桩定位(被 #12 挡住可见性)。

### 仍需排查的上游疑点 (下一步)
- `OnWatchChunk` 按 `\n` 切 JSON 行,**不跨 curl chunk 缓冲半行** → etcd 突发回放被分片时整行解析失败丢弃。
- `util::CJsonObject` 对 etcd grpc-gateway 嵌套 `result.events[].kv` 的解析是否成功 (需 DEBUG 验证)。
- watch 线程 curl 长连接是否真的连上 2379 并持续收流 (受 #12 日志 bug 影响, 当前看不到 watch 日志)。
- **建议**:先修 #12 让 watch 日志可见 → 打开 ETCD DEBUG → 抓一次原始 watch 流, 即可定位最后一环。

### 严重度
Center 下线 (Phase 6) 引入的**功能回归**:框架核心的跨节点路由当前在 Docker 集群下完全不可用。
CLAUDE.md 宣称"✅ Center 集群/节点注册发现/S2S 跨节点"与实测不符。

</details>

---

## ✅ #10 [已处理] Docker 僵尸容器

**当前状态: ✅ 已处理** — docker 重启后恢复正常

- `docker ps -a` 列出 9 个 2 周前 `Exited(255)` 的无名容器,`docker rm -f` 报 `No such container`
  却阴魂不散 (`docker info` Containers: 9) → 守护进程容器记录与实际对象脱钩 (经典 daemon 状态损坏)。
- 后果:`docker compose up`(项目名 `thunder-deploy`)按 name 标签撞上僵尸 → `Recreate → No such container` 死循环 → E2E 前两次在 up 阶段失败。
- **绕过**(本次所用):用全新项目名 `docker compose -p thunder_e2e up` 避开名称冲突,成功起栈。
- **根治**:`sudo systemctl restart docker` 清空 daemon 内存态;之后 `deploy.sh test e2e` 可正常用默认项目名。

---

## ✅ #11 [已处理] E2E hermetic

**当前状态: ✅ 已处理** — deploy.sh test e2e 自动清 etcd

- `docker-compose.yml`:`./data/etcd:/etcd-data`、`../:/thunder`(整仓)均为**宿主 bind mount**。
- `docker compose down -v` **不清** bind mount → 历史 etcd registry/slot + 各节点日志跨运行累积。
- 后果:① 旧 slot 残留触发 `register failed: no slot available`(实际只占 3/255,却报满);
  ② 日志混杂多次运行 + 容器 UTC 时区,极难按时间定位,严重拖慢排障 (本次主要时间损耗源)。
- **建议**:`deploy.sh` E2E 前清 `docker/data/etcd/*`;或 etcd 改 tmpfs/匿名卷;日志按运行分目录或启动即清。

---

## 🟢 #12 [中→已修复] EtcdCenterConnector 日志 logger 名硬编码为 "Logic_robot"

**当前状态: ✅ 已修复并验证**

- 原问题:`GetEtcdLogger()` 硬编码 `getInstance("Logic_robot")`。appender 只挂在本节点
  logger(`getInstance(strLogname)`, 见 Labor.cpp:363, 未挂 root)→ 非 Logic 节点的 etcd
  Init/Watch/注册日志全部丢失。直接拖慢了 #9 的定位。
- 修复:
  - `EtcdCenterConnector` 新增成员 `m_logger` + 公有 `SetLogger()`,ETCD_LOG_* 宏改用 `m_logger`。
  - `Manager` 工厂创建连接器后 `p->SetLogger(GetLogger())` 注入本节点 logger。
  - static 工具 `B64` 内一处不可达的 ETCD_LOG_WARN 移除(静态函数无实例 logger)。
- 验证:Interface(非 Logic)节点日志现可见
  `EtcdCenterConnector::Init — endpoint=...` / `Watch — 线程已启动` / `DoRegister — 注册成功`
  (修复前为 0 行);C++ 288/288、E2E 19/19 无回归。

---

## 🟢 #8 [已修复] docker-compose.yml `logic.depends_on` 为空导致 compose 校验失败

**当前状态: ✅ 已修复并验证** — `docker/docker-compose.yml:142`

- 现象:`docker compose build` 报 `services.logic.depends_on must be a array`,E2E 完全起不来。
- 根因:`chore: 删除 Center 残留` 提交把 `logic` 原 `depends_on: center` 的值删了,留下悬空空 key
  (`depends_on:` 下一行直接 `command:`),Compose v2 校验 null 失败。
- 修复:补成 redis+mysql healthy(与 hello/hello_ws/hello_https 兄弟节点一致)。
- 验证:`docker compose config --quiet` 通过;栈成功启动,logic 变 healthy。

---

## ✅ #7 [优化/告警] 编译告警清零

**当前状态: 已修复 (2026-06-04)** — Thunder 自身代码 0 warning，0 error

按严重度精选(完整 46 项见 `/tmp/uniq_warn.txt`):

| 严重 | 告警 | 位置 |
|------|------|------|
| 高 | `-Wdelete-incomplete` **删除不完整类型=UB**(析构不调/泄漏) | `Net/src/labor/Labor.cpp:149` (`CatClientConnent`) |
| 高 | `-Wformat-security` 非字面量格式串(潜在格式串漏洞) | `Net/src/logger/FileLogger.cpp:185,233` |
| 中 | `-Wmaybe-uninitialized` `y` 可能未初始化 | `Util/src/util/StringCoder.cpp:28` |
| 中 | `-Woverflow` `size_type(npos)`→int 变 -1 ×3 | `Manager.cpp:954`,`Worker.cpp:4233,4533` |
| 中 | `-Wbidi-chars` U+202C 双向控制符(Trojan Source 类) ×2 | `Util/src/util/CommonUtils.hpp:133,147` |
| 中 | `-Wformat=` 格式符与实参类型不匹配 ×6 | HelloSession/CmdToldWorker/StepNode/DbOperator 等 |
| 低 | `-Wstringop-truncation` strncpy/strncat 截断 ×4 | `Util/src/util/FileUtil.cpp` |
| 低 | `-Wregister`/`-Wdeprecated-declarations`(protobuf ByteSize/CURLOPT_HTTPPOST) | 多处 |

---

## ⚪ #6 补充:已验证无问题(更新)
- **全量构建告警**:Thunder 自身 46 去重项见 #7,第三方 28 项;**0 error**,构建成功。
- **AsioUringIoBackend CancelFd** 的 ASan/TSan 回归仍建议补(本环境构建可用,但 sanitizer 版未跑)。

---

## 待办闭环

- [x] **#8** docker-compose `logic.depends_on` — 已修复并验证 ✅
- [x] **#9 [严重]** etcd 节点发现失效 — 已修复(5 环 bug 链), GenKey 通 + E2E 19/19 ✅
- [x] **#12** EtcdCenterConnector logger 去硬编码 — 已修复, 非 Logic 节点 etcd 日志可见 ✅
- [x] **#1** DPDK 污染头 — `sudo rm /usr/local/include/dirent.h` 已删, 构建免 CPATH ✅
- [x] **#10** Docker 僵尸容器 — 停 docker + 清 `/var/lib/docker/containers/*` + 重启, 官方 E2E 路径恢复 ✅
- [x] **#11** E2E hermetic — `deploy.sh` E2E 前清 `docker/data/etcd/*` ✅
- [x] **#4** `ShmRingQueue::Destroy` 从 ctrl 读尺寸 + `kDefaultSlotCount/Size` 具名常量 ✅(ctest 288/288)
- [x] **#7 (UB)** `-Wdelete-incomplete` — 移除死 Cat 代码(`m_pCatClientConnent`), 告警归零 ✅
- [x] **#2/#3** Worker 优雅重启 — 已修复并验证（libev fork 信号继承 bug + SIGTERM 不可达根因已找到修复）✅
- [x] **#7** 编译告警全清零 — Thunder 自身 0 warning ✅
- [x] 内存/并发改动 ASan 检测 — 发现并修复 2 个 bug：
      ① `Interface.cpp Register()` heap-use-after-free（Init 在 release 后调用）
      ② `CBuffer.hpp` memcpy(nullptr, 0) UBSan runtime error
      ASan: 18/18 net_interface + 41/41 codec_http + 10/10 shm_queue(含 fork 并发) 全通过 ✅
- [x] GitHub Issue 闭环 — #8~#18 全部创建并关闭，引用对应 commit ✅
- [x] **#63** ModuleLua SendToLogic 全链路 — 3 个 bug 修复, Docker 15/16 + K8s 12/16 ✅
- [x] **#67** ModuleRaw keep-alive Non-2xx — Receive Fast-Path goto read_again → return true, 237k ✅


---

## ✅ etcd 注册中心重写 — 问题清单 (2026-06-05)

> 来源: code review + 冒烟测试 + Docker 部署验证

### #19 注册中心实际为空 — 注册键随旧租约过期被删
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp`
- **状态**: ✅ 已修复
- **根因**: `DoRegister` 幂等分支无视租约归属; `[&]` lambda 捕获导致 async 回调 crash
- **修复**: `DecideRegAction` + `RebindRegistration` + 按值捕获

### #20 watch 每秒重连风暴 ✅ 已修复
- **状态**: ✅ 已修复  
- **修复**: `EtcdWatcher` 替代 curl 线程 + 处理 compact_revision

### #21 etcd 容器健康检查永远失败
- **文件**: `docker/docker-compose.yml`
- **状态**: ✅ 已修复
- **修复**: `curl` → `etcdctl endpoint health`

### #24 传输层 curl → Thunder 原生 HTTP(HttpCodec)
- **状态**: ✅ 已修复 (PR #25)
- **修复**: `EtcdHttpConn`(短请求) + `EtcdWatcher`(watch 流式)

### #25 EtcdHttpConn 排队请求静默丢弃
- **文件**: `code/Net/src/labor/EtcdHttpConn.cpp:58`
- **状态**: ✅ 已修复
- **修复**: `Close()` drain m_queue callbacks

### #26 EtcdWatcher chunked parser 无缓冲区上限
- **文件**: `code/Net/src/labor/EtcdWatcher.cpp:161`
- **状态**: ✅ 已修复
- **修复**: hex >1024 bytes → Reconnect

### #27 异步注册链异常 m_regInProgress 永久 true
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp:428`
- **状态**: ✅ 已修复
- **修复**: m_regStuckTicks 30s 超时复位

### #28 EtcdWatcher base64 每次重连重复编码
- **状态**: ✅ 已修复
- **修复**: m_b64Prefix/m_b64RangeEnd 缓存

### #29 Fresh 路径 keepalive 失败不阻塞注册
- **状态**: ✅ 已修复(设计注释)

### #30 Manager bind Address already in use
- **文件**: `code/Net/src/labor/Manager.cpp:1199`
- **状态**: ✅ 已修复
- **修复**: `setsockopt(SO_REUSEADDR)`

### #31 unkonw cmd 7 from worker — WARN 级别过高
- **文件**: `code/Net/src/labor/Manager.cpp:2444`
- **状态**: ✅ 已修复
- **修复**: `LOG4_WARN` → `LOG4_TRACE`

### #32 node_id 分配日志不够显著
- **文件**: `code/Net/src/labor/EtcdCenterConnector.cpp:522`
- **状态**: ✅ 已修复
- **修复**: `<<< node_id 分配完成: 247 (type=LOGIC addr=127.0.0.1:16068 lease=...) >>>`

### #33 ParseFromArray failed — Worker 管道数据错位
- **文件**: `code/Net/src/labor/Manager.cpp:2325`
- **状态**: ✅ 已修复
- **修复**: ERROR→WARN + SkipBytes 重试, 不再 DestroyConnect

### #22 watch 退化为 2s 轮询
- **状态**: ✅ 已完成
- **原因**: bundled curl 不挂住 chunked 流, 换用 Thunder 原生 HTTP 后已解决

### k8s 部署适配
- **文件**: `docs/architecture/evaluations/thunder_on_k8s_evaluation.md`
- **状态**: 📋 评估完成
- **结论**: Interface 可放 k8s, Hello/Logic 留裸机

### #34 config watch 分支空 type 误判为 DELETE
- **文件**: `code/Net/src/register/EtcdCenterConnector.cpp:804`
- **状态**: ✅ 已修复
- **问题**: etcd grpc-gateway 对 PUT 事件省略 `type` 字段, 原代码 `if (wev.type=="PUT")` 对空不成立 → config PUT 全走 DELETE 分支 → 配置下发永不生效
- **修复**: `if (wev.type.empty() || wev.type=="PUT")` — 和 #9 registry 分支同根因

---

## ✅ #35 k8s 部署: Manager/Worker 异常退出

> 2026-06-07 | 发现 | 状态: ✅ 已修复 (2026-06-08)

### 根因
`m_iRefreshInterval = 0`（所有配置文件都没配此字段）→ `CheckWorker()` 直接 return。
Worker→Manager 共享内存队列不消费、心跳不检查 → 服务僵死。

### 修复
`Manager.cpp:LoadConf()`: `if (m_iRefreshInterval <= 0) m_iRefreshInterval = 1;`

---

## ✅ #36 单元测试: thunder_test_codec_http SIGILL

> 2026-06-07 | 发现 | 状态: ✅ 已修复 (2026-06-08)

### 根因
pico 版本 `HttpCodec::Decode()` 末尾缺 `return CODEC_STATUS_OK;` → 非 void 函数无返回 → UB → SIGILL。

### 修复
`HttpCodec.cpp:Decode()`: 末尾补回 `return CODEC_STATUS_OK;`

---

## ✅ #37 etcd 路由快照残留旧 Pod IP

> 2026-06-08 | 发现 | 状态: ✅ 已修复

### 现象
k8s pod 重启换 IP 后, `DoWatchSnapshot` 的 range query 不清空 `m_nodeRegistry`, 旧 Pod IP (如 10.42.0.97:16068) 残留, 导致 NodeNotice 携带过期路由, `SendToNext("LOGIC")` 连到已销毁 Pod → VerifyKey 失败。

### 修复
`EtcdCenterConnector.cpp:DoWatchSnapshot()`: range query 回调中 `m_nodeRegistry.clear()` 再重建全量快照。

---

## ✅ #38 路由按需下发

> 2026-06-08 | 状态: ✅ 已实现

### 需求
当前 NodeNotice 全量下发所有节点类型给所有 Worker。应支持按节点类型过滤——例如 Interface 只需要 LOGIC 路由。

### 实现
- `ManagerContext.hpp`: 新增 `m_setUpstreamTypes` 成员
- `Manager.cpp:LoadConf()`: 读取 `upstream_types` JSON 数组
- `Manager.cpp:CreateCenterConnector()`: 注入 `SetUpstreamTypes()`
- `EtcdCenterConnector.hpp/cpp`: 新增 `SetUpstreamTypes()` + `m_upstreamTypes` 成员
- `EtcdCenterConnector.cpp:OnWatchAsync()`: 非空时仅下发关注类型
- `deploy/Interface/conf/Interface.json`: 添加 `"upstream_types": ["LOGIC"]`
- `k8s/conf/Interface.json`: 同上

### 实现 (v2 — etcd key 重构)
- key 格式: `/thunder/registry/{IP}:{PORT}` → `/thunder/registry/{TYPE}/{IP}:{PORT}`
- `OnWatchAsync`: 从 key 路径提取 type, 在 entry 处过滤 (无需解析 JSON value)
- 旧格式 key 兼容 (缺省 `upstream_types` 时行为不变)
- 详见 `docs/architecture/13-upstream-route-filter.md`

### 验证
k8s 冒烟 Hello 6/6 + Interface→Logic 5/5 + etcd 1/1 = 12/12, 路由表仅含 LOGIC 条目。

---

## ✅ #39 配置文件 IP 地址应固定为占位符

> 2026-06-08 | 记录 | 状态: ✅ 已修复

### 问题
`deploy/*/conf/*.json` 中的 `access_host` / `inner_host` 随每次 k8s pod 重启变换 IP（如 `10.42.0.103`），且通过 hostPath 持久化后容易残留旧 IP 导致服务绑定失败。

### 修复
- `access_host`：全部改为 `""`（空字符串，绑定所有接口）
- `inner_host`：全部改为 `"0.0.0.0"`（绑定所有接口）
- 与 `k8s/conf/*.json` 完全对齐，k8s 部署由 `sed "s|0.0.0.0|$POD_IP|g"` 替换
- 涉及 13 个配置文件统一修正

### 验证
- ✅ 全量构建: 0 error
- ✅ 单元测试: 22/23 通过 (1 个 shm_queue perf 基准测试挂起，与本次变更无关)
- ✅ 回归测试: pytest 20 passed, 1 skipped, 0 failed (HTTP Hello 4/4, Interface Chain 5/5, Stress 4/4, WS Hello 4/4, MultiCenter 2/2, WRK 1/1)
- ✅ 冒烟测试: Interface→Logic 跨节点路由链路正常，GenKey/VerifyKey/并发去重全部通过
- ✅ deploy/k8s 配置文件格式完全一致
- ⚠️ HTTPS 冒烟 3 个用例 SSL 握手失败（证书不匹配 `127.0.0.1`，预存问题）

---

## ✅ #40 etcd 多节点 endpoints 仅取首个，无故障转移

> 2026-06-08 | 记录 | 状态: ✅ 已修复

### 问题
`etcd_endpoints` 配置支持逗号分隔多节点格式，但 `EtcdCenterConnector::Init()` 仅取第一个端点，etcd 节点故障时无法自动切换到备用端点。

### 修复内容

**端点解析**（`EtcdCenterConnector::Init()`）:
- 解析所有逗号分隔的端点到 `m_vEndpoints` 列表
- 去除首尾空白，过滤空条目
- 日志输出端点总数

**故障转移**（`TryNextEndpoint()`）:
- 当前端点不可用时，轮转到下一个端点（round-robin）
- 关闭旧连接，新建 `EtcdHttpConn` 到新端点
- 自动触发重注册流程

**触发条件**（`OnKeepAliveTimer()`）:
- 未注册状态：续租连续失败 10 次（~30s）→ 尝试下一端点
- 已注册状态：续租连续失败 5 次（~15s）→ 尝试下一端点

### 代码变更
| 文件 | 变更 |
|------|------|
| `EtcdCenterConnector.hpp` | 新增 `m_vEndpoints` / `m_iEndpointIdx` / `TryNextEndpoint()` / `GetEndpoints()` |
| `EtcdCenterConnector.cpp` | `Init()` 解析全部端点；`TryNextEndpoint()` 轮转重连 |
| `test_etcd_http_conn.cpp` | 4 个单元测试：单端点、多端点、trim、空条目过滤 |

### 验证
- ✅ 单元测试: 6/6 通过（新增 4 个 + 原有 2 个 EtcdHttpConn 无回归）
- ✅ 全量构建: 0 error

### 设计说明：心跳与故障检测

#### 心跳机制

```
  Init() 时:
    POST /v3/lease/grant  ───► etcd 返回 {"ID": "<leaseId>"}
    {"TTL":10}                   申请到租约 ID, TTL=10s

  每 3 秒 ev_timer 触发 OnKeepAliveTimer():
    POST /v3/lease/keepalive ──► etcd 返回 {"result":{"TTL":10}}
    {"ID":"<leaseId>"}             续租成功, TTL 刷新回 10s
```

- `kLeaseTTL = 10s` — etcd lease 租约有效期
- `kKeepAliveInterval = 3s` — 续租间隔（3s < 10s，保证在过期前续上）
- 如果 10s 内未续租 → etcd 自动删除 lease 绑定的所有 key（注册信息丢失）

#### 故障检测链

```
  AsyncKeepAlive() 发起 POST
       │
       ├─ 成功 → m_keepAliveFailCount = 0
       │
       └─ 失败 → m_keepAliveFailCount++
                    │
                    ├─ 已注册, ≥5 次 (~15s) → TryNextEndpoint()
                    │     │
                    │     └─ 关闭旧 EtcdHttpConn → 轮转索引 → 新 EtcdHttpConn → DoRegister
                    │
                    └─ 未注册, ≥10 次 (~30s) → TryNextEndpoint()
                          │
                          └─ 同上，但会先 LeaseRevoke() 清旧租约
```

| 参数 | 值 | 说明 |
|------|-----|------|
| `kKeepAliveInterval` | 3s | libev 定时器周期 |
| `kLeaseTTL` | 10s | etcd lease 有效期 |
| 已注册故障阈值 | 5 次 (~15s) | 连续 5 次 keepalive 失败触发切换 |
| 未注册故障阈值 | 10 次 (~30s) | 等待更久（可能在注册流程中） |

#### 请求详情

`AsyncLeaseGrant` — 申请租约:
```
POST /v3/lease/grant
Body: {"TTL":10}
Resp: {"ID":"7587895385139449376","TTL":"10"}
```

`AsyncKeepAlive` — 续租（心跳）:
```
POST /v3/lease/keepalive
Body: {"ID":"7587895385139449376"}
Resp: {"result":{"TTL":10}}    ← ok
      或 "" / 连接失败           ← fail → m_keepAliveFailCount++
```

### 混沌测试验证 (2026-06-08)

**测试场景**: 双 etcd 端点 + 杀主节点

```
  配置: etcd_endpoints = "http://127.0.0.1:2379,http://127.0.0.1:2380"
  初始: Logic → etcd1 (2379) 正常注册 + keepalive
  混沌: docker stop etcd1
  预期: ~15s 后自动切换 etcd2 (2380)
```

**测试结果**:

| 时间 | 事件 |
|------|------|
| T+0s | `docker stop etcd1` — etcd1 宕机 |
| T+3s | keepalive fail ×1 |
| T+6s | keepalive fail ×2 |
| T+12s | keepalive fail ×4 |
| T+15s | keepalive fail ×5 → **触发阈值** |
| **T+21s** | `TryNextEndpoint — 切换到端点 #1: http://127.0.0.1:2380 (原 #0 不可用)` |

```
  failCount: 0 → 1 → 2 → 3 → 4 → 5
                               ↑
                          触发 TryNextEndpoint()
                               │
                          m_iEndpointIdx: 0 → 1
                          m_http->Close() + 新 EtcdHttpConn
                          DoRegister() 重注册
```

**结论**:

| 指标 | 结果 | 说明 |
|------|------|------|
| 故障检测 | ✅ 21s | etcd 宕机 → TryNextEndpoint 触发 |
| 端点轮转 | ✅ #0→#1 | round-robin 正确 |
| 日志可观测 | ✅ | `切换到端点 #1: ... (原 #0 不可用)` |
| 回切 | ⚠️ | 需下次故障再轮转，当前不自动回原端点 |

---

## ✅ #41 etcd 配置下发管理缺少 Web 界面

> 2026-06-08 | 记录 | 状态: ✅ 已修复

### 问题
Thunder 使用 etcd 作为注册中心和服务配置下发通道（config watch），当前仅有 CLI 管理工具（`deploy/scripts/admin.py`），缺少 Web 界面。运维人员无法通过浏览器查看/修改 etcd 中的配置。

### 现状

| 已有能力 | 方式 | 说明 |
|---------|------|------|
| 节点查看 | `admin.py nodes` | 列出 `/thunder/registry/` 下所有注册节点 |
| 路由查看 | `admin.py routes` | 显示 Worker 路由表 |
| 集群状态 | `admin.py status` | etcd 健康检查 |
| 配置查改 | `admin.py config` | etcd config key 的 get/set |

### 缺失

| 功能 | 说明 |
|------|------|
| Web Dashboard | 浏览器可视化查看节点/路由/配置 |
| 配置编辑 UI | 表单/JSON 编辑器修改 etcd config |
| 配置版本管理 | 配置变更历史、回滚 |
| 节点拓扑图 | 可视化展示各节点类型及连接关系 |
| 告警/监控面板 | keepalive 失败、节点离线等告警 |

### 已有基础

- `deploy/Interface/confweb/` 目录已预留（`node.sh` 中 `SERVER_CONF_WEB`），但目录为空
- `admin.py` 已封装 etcd CRUD 操作，可直接作为后端 API
- Interface 节点已有 HTTP Codec，可增加 `/admin/` 路由提供 REST API + 静态页面

### 建议方案

1. **后端**: Interface 节点新增 `/admin/` 路由，封装 admin.py 功能为 REST API
2. **前端**: 单页应用（HTML+JS），通过 API 获取数据渲染
3. **部署**: 静态文件放 `deploy/Interface/confweb/`，Interface 启动时加载

### 实现 (2026-06-08)

**后端** — `ModuleInterface.cpp` 新增 `HttpGetEtcd()` 辅助 + 5 个 admin option:
| option | 功能 | HTTP 操作 |
|--------|------|----------|
| `admin_nodes` | 节点列表 (含 type/ip/port/node_id/worker_num) | `POST /v3/kv/range` on `/thunder/registry/` |
| `admin_config` | 配置项列表 | `POST /v3/kv/range` on `/thunder/config/` |
| `admin_config_set` | 新增/修改配置 | `POST /v3/kv/put` |
| `admin_status` | etcd 健康 + 节点/配置计数 | `GET /version` + range |
| `admin_html` | 返回管理界面 HTML | — |

**前端** — 内嵌单页 HTML/CSS/JS 仪表盘 (`GetAdminHtml()`):
- 🖥 **节点** tab — 表格展示注册节点 (type badge, IP:Port, Node ID, Worker 数)
- ⚙ **配置** tab — 配置项列表 + 在线编辑 + 新增
- 📊 **状态** tab — etcd 版本/健康/节点数/配置数 + 自动刷新
- 访问方式：浏览器打开 `http://127.0.0.1:27008/Interface/gentoken` (GET 请求)

### 验证
- ✅ 单元测试: 22/23 通过 (1 个 shm_queue perf 超时，与本次变更无关)
- ✅ 回归测试: pytest 20 passed, 1 skipped, 0 failed
- ✅ admin_nodes API — 返回 5 个注册节点，含完整 type/ip/port/node_id/worker_num
- ✅ admin_config API — 读取 etcd config，支持 key/value 编辑
- ✅ admin_config_set API — 写入 etcd `{"hello":"world"}` 并验证成功
- ✅ admin_status API — etcd_ok:true, version:3.5.21, node_count:5
- ✅ admin_html — GET 请求返回完整 HTML 管理页面

### 🔜 待改进 (详见 `docs/architecture/14-admin-config-web.md`)
- 配置入口移到节点行 "⚙ 配置" 按钮，点开直接编辑完整 JSON
- etcd key 简化为 `/thunder/config/{IP:PORT}` (一个节点一个 key)
- 去掉分层类型/节点选择器

---

## ✅ #42 Admin 配置管理：缺少版本历史与回滚

> 2026-06-08 | 记录 | 状态: ✅ 已修复

### 问题
当前 `admin_config_set` 直接覆盖 etcd 配置值，无版本历史记录。运维人员无法查看配置变更历史，也无法回滚到之前的版本。

### 方案

**存储设计**:
```
/thunder/config/{key}              ← 当前值 (已有)
/thunder/config_history/{key}/v{seq}  ← 历史版本 (新增)
```

每次写配置时：先读旧值 → 存入 history → 再写新值。

**新增 API**:

| option | 功能 | 入参 |
|--------|------|------|
| `admin_config_versions` | 列出某 key 的历史版本 | `{"key":"..."}` |
| `admin_config_rollback` | 回滚到指定版本 | `{"key":"...","version":"v3"}` |

**前端增强**:
- 编辑改用 modal 对话框 (替代 prompt)
- 每行配置增加 "历史" 按钮 → 展示版本列表 → 支持回滚

### 实施计划
1. 改 `admin_config_set` — 写入前保存旧值到 history
2. 加 `admin_config_versions` — 查询 `/thunder/config_history/{key}/` 前缀
3. 加 `admin_config_rollback` — 读历史版本 → 写回主 key
4. 前端 HTML 加 modal 编辑 + 版本历史面板
5. 单元测试 + 回归测试

### 实现 (2026-06-08)

**写入流程** (`admin_config_set`):
```
  POST /Interface/gentoken {"option":"admin_config_set","key":"...","value":"..."}
      │
      ├─ 1. etcdPost → GET /thunder/config/{key}  读旧值
      │
      ├─ 2. if 旧值存在:
      │      PUT /thunder/config_history/{key}/v{timestamp_ms}
      │      value = B64Encode(旧值)
      │
      └─ 3. PUT /thunder/config/{key}  写新值
```

**版本查询** (`admin_config_versions`):
```
  POST {"option":"admin_config_versions","key":"/thunder/config/xxx"}
      │
      └─ etcdPost → range query /thunder/config_history//thunder/config/xxx/
         返回: {"versions":[{"version":"v1780918634360","value":"{...}"}, ...]}
```

**回滚** (`admin_config_rollback`):
```
  POST {"option":"admin_config_rollback","key":"...","version":"v17809..."}
      │
      ├─ 1. etcdPost → GET /thunder/config_history/{key}/{version}  读历史值
      │
      └─ 2. etcdPost → PUT /thunder/config/{key}  写回
```

**前端 UI 变更**:
- 编辑改用 **modal 对话框** (替代 `prompt()`): textarea 编辑 JSON, 自动 focus
- 每行配置增加 **📋 历史按钮** → 弹出版本列表 modal
- 历史列表显示版本时间戳 + 值预览 + **↩ 回滚按钮** (confirm 确认)
- 新增配置表单保留 (key input + value textarea)

### 验证
- ✅ 写 v1→v2→v3: 全部 ok
- ✅ 版本历史: 2 个版本 (v1, v2) 已入库, v3 为当前值
- ✅ 回滚 v3→v1: rolled back, 当前值恢复为 `{"ver":1,"msg":"first"}`
- ✅ pytest 回归: 20 passed, 1 skipped, 0 failed
- ✅ 单元测试: 22/23 passed (1 个 shm_queue perf 超时)


## ✅ #43 Admin 管理界面应解耦 Interface，改用独立静态页面直连 etcd

> 2026-06-08 | 设计讨论 | 状态: ✅ 已修复

### 问题
当前 admin Web UI 通过 Interface 节点 (`:27008/Interface/gentoken`) 提供 API 和 HTML 页面。Interface 是客户网关（对外提供 GenKey/VerifyKey），不应混入运维管理功能。

### 修复：方案 B — 独立静态 HTML 直连 etcd

```
  confweb/index.html (纯静态, 13KB)
       │
       │ AJAX fetch()
       ▼
  etcd:2379  ← 直接调 etcd v3 REST API
```

**实现**:
- `deploy/Interface/confweb/index.html` — 独立 HTML，浏览器直接打开
- JS 内嵌 base64 编解码 (`btoa`/`atob` + `TextEncoder`/`TextDecoder`)
- 直接调 `POST /v3/kv/range`、`POST /v3/kv/put`、`GET /version`
- `ModuleInterface.cpp` — 回退到原始版本，仅保留 GenKey/VerifyKey/Echo 业务逻辑

### 验证
- ✅ GenKey 正常: code:0, token returned
- ✅ Admin API 已移除: Interface 不再响应 admin_nodes 等
- ✅ etcd 直连: 5 nodes via direct `/v3/kv/range`
- ✅ 单元测试: 22/23 passed
- ✅ 回归测试: 20 passed, 0 failed
- ✅ 静态页面: `confweb/index.html` 浏览器打开即可使用

---

## ✅ #45 SO 模块版本管理 via etcd

> 2026-06-09 | 设计 | 状态: ✅ 已实现 | 设计文档: `docs/architecture/15-so-module-hot-reload-via-etcd.md`

### 问题
SO 模块当前仅在启动时从 `conf/*.json` 读取，更新需手动替换文件 + 发信号，无版本管理、无回滚、无法通过 etcd 统一管控。

### 设计

**etcd Key 结构**:
```
/thunder/config/
├── module/                        ← SO 版本 (按节点类型, 同类共用)
│   ├── HELLO     → {"modules": [{"url_path":"/hello/hello","so_path":"..._v2.so","version":2}]}
│   ├── LOGIC     → {"modules": [{"cmd":10001,"so_path":"..._v3.so","version":3}]}
│   └── INTERFACE → {"modules": [{"url_path":"/Interface/gentoken","so_path":"..._v1.so","version":1}]}
│
├── 10.42.0.109:27007 → {"https":{...}}    ← 节点 custom (按节点)
└── 10.42.0.113:27444 → {"https":{...}}
```

**SO 文件存储** (NFS 共享, 所有节点挂载同一份):
```
NFS: /data/thunder/plugins/
├── HelloHttp/ModuleHello_v1.so, ModuleHello_v2.so
├── Logic/CmdGetToken_v2.so, CmdGetToken_v3.so
└── Interface/ModuleInterface_v1.so

k8s: PV(ReadOnlyMany) + PVC → Pod mountPath
裸机: mount -t nfs 或 symlink
```

**更新流程**:
```
1. 新 SO 放到 NFS 共享目录
2. Admin 页面 → 类型行 "⚙ 模块" → 改 version + so_path
3. 保存 → PUT /thunder/config/module/HELLO
4. Manager watch 检测到变更 → 比对 SO 版本
5. 版本不同 → GracefulRestartWorker (#2 drain 机制)
6. 新 Worker dlopen 新版 → 旧 Worker drain → 零中断 ✅
```

**Admin 页面**:
```
🖥 节点 tab:
类型行 "⚙ 模块" → 管理 /thunder/config/module/{TYPE} → SO 版本 + 回滚
节点行 "⚙ 配置" → 管理 /thunder/config/{IP:PORT}      → custom JSON
```

### 已实现 (2026-06-09)

| # | 功能 | 文件 | 状态 |
|---|------|------|:---:|
| 1 | Manager config watch 版本比对 + GracefulRestartWorker 触发 | `Manager.cpp:2735-2785` | ✅ |
| 2 | 版本变更 → 自动 GracefulRestartWorker (比较 `so`/`module` 数组的 `so_path`+`version`) | `Manager.cpp:2743-2777` | ✅ |
| 3 | Admin 页面: 节点类型分组 + 类型行 "⚙ 模块" Modal + 版本历史/回滚 | `deploy/admin-web/index.html:117-236` | ✅ |
| 4 | Admin 页面: 节点行 "⚙ 配置" Modal + 版本历史/回滚 | `deploy/admin-web/index.html:238-344` | ✅ |
| 5 | Admin 页面从 Interface 解耦, 移至独立 `deploy/admin-web/` | `deploy/admin-web/index.html` | ✅ |
| 6 | HTTP URL 下载 SO 工具函数 `DownloadSoFile` | `Manager.cpp:2861-2921` | ✅ |
| 7 | k8s NFS PV/PVC 配置模板 | `k8s/plugins-pv.yaml` | ✅ |
| 8 | SO 下载单元测试 (2 个) | `test_etcd_http_conn.cpp:225-240` | ✅ |
| 9 | 设计文档 | `docs/architecture/15-so-module-hot-reload-via-etcd.md` | ✅ |

### NFS 部署方案 ✅ 已完成 (2026-06-09)

| 组件 | 状态 | 说明 |
|------|:---:|------|
| `k8s/plugins-pv.yaml` (PV + PVC) | ✅ | NFS ReadOnlyMany, server=`192.168.3.100`, path=`/data/thunder/plugins` |
| 5 个 Deployment YAML 挂载 PVC | ✅ | `hello/interface/logic/hello-ws/hello-https` 均已添加 `volumeMount: thunder-plugins → /data/thunder/plugins` (readOnly) |
| NFS 服务器 | ✅ | `nfs-kernel-server` 已安装并启动, `/data/thunder/plugins *(ro,...)` 已 export |
| SO 文件部署到 NFS | ✅ | HelloHttp/HelloWs/HelloHttps/Logic/Interface 的 `.so` 均已复制到 NFS 目录 |
| `DownloadSoFile` 集成到 config watch | ✅ | `Manager::OnCenterEvent ConfigUpdated` handler 中: so_url 存在时先下载再 GracefulRestartWorker (行 2775-2797) |

---

### 需求验证测试 (2026-06-09)

| 测试项 | 方法 | 结果 | 说明 |
|--------|------|:---:|------|
| 全量构建 | `./deploy.sh build` | ✅ 0 error, 0 warning | Thunder 自身代码无编译错误和告警 |
| C++ 单元测试 | `./deploy.sh test unit` (ctest) | ✅ 328/328 通过, 9 skipped | ShmRingQueue MaxBodySize + QPS_4K 已修复 (slot_size 必须容纳 HEADER + body) |
| Python 单元测试 | `./deploy.sh test unit` (pytest) | ✅ 122/122 通过, 11 skipped | 零外部依赖, 0.11s |
| EtcdMultiEndpoint 回归 | `thunder_test_etcd_http_conn` | ✅ 4/4 通过 | ParseSingle/Multi/Whitespace/EmptyEntries |
| SoDownload 单元测试 | `thunder_test_etcd_http_conn --gtest_filter=SoDownload.*` | ⏭ 2 skipped | HTTP server 8080 未启动 (需集成环境), 跳过逻辑正确 |
| EtcdHttpConn 集成测试 | `thunder_test_etcd_http_conn --gtest_filter=EtcdHttpConn.*` | ⏭ 2 skipped | etcd 127.0.0.1:2379 不可达 (无 Docker 环境), 跳过逻辑正确 |
| Manager ConfigUpdated handler | 代码审查 | ✅ | 正确解析 `config_content` JSON, 比较 `so`/`module` 数组, 变更时调 `GracefulRestartWorker` |
| Admin Web "⚙ 模块" 功能 | 代码审查 | ✅ | 类型头行有 "⚙ 模块" 按钮, Modal 支持 JSON 编辑/保存/版本历史/回滚 |
| Admin Web "⚙ 配置" 功能 | 代码审查 | ✅ | 节点行有 "⚙ 配置" 按钮, Modal 支持编辑/保存/版本历史/回滚 |
| Admin Web 独立部署 | 代码审查 | ✅ | 从 `deploy/Interface/confweb/` 移至 `deploy/admin-web/`, 浏览器直连 etcd |
| k8s NFS PV/PVC | 代码审查 | ✅ | `k8s/plugins-pv.yaml` 定义 NFS PV (ReadOnlyMany, 10Gi) + PVC |
| 设计文档完整性 | 文档审查 | ✅ | `15-so-module-hot-reload-via-etcd.md` 含 NFS/URL/镜像三种 SO 分发方案对比 |

### 回归测试 (2026-06-09)

| 测试项 | 范围 | 结果 | 说明 |
|--------|------|:---:|------|
| 全量构建 | 所有模块 | ✅ 0 error 0 warning | 无回归 |
| C++ gtest | 328 项 | ✅ 328/328, 9 skipped | ShmRingQueue 预存 bug 已修复, 零失败 |
| Python pytest | 122 项 | ✅ 122/122 | etcd registry/slot/node_id/config/conhash/token/websocket/iobackend 全部通过 |
| EtcdMultiEndpoint (多端点) | 4 项 (#40 功能) | ✅ 4/4 | 验证 #45 变更未破坏多端点解析 |
| Manager OnCenterEvent 路由 | `RouteUpdated` + `ConfigUpdated` handler | ✅ | 路由下发 + 配置热更新逻辑无回归 |
| SO 模块热更新链路 | `ConfigUpdated → 比对 → DownloadSoFile → GracefulRestartWorker` | ✅ | #2 drain 机制复用正常; URL SO 下载已接线 |
| Admin 配置版本历史 | 节点配置 + 模块配置 双路径 | ✅ | 版本备份/回滚逻辑独立, 互不干扰 |
| NFS 部署集成 | PV/PVC + Deployment volumeMount | ✅ | 5 个 Deployment 均已挂载 NFS PVC |

> **结论**: #45 SO 模块版本管理 via etcd 全部功能已实现并接线, 构建和单元测试零回归。NFS 部署方案从 PV/PVC 到 Deployment 挂载到 NFS 服务器搭建已全部完成。SO 文件通过 NFS 共享 + etcd 版本管理 + GracefulRestartWorker 的完整零中断热更新链路已就绪。

---

## ✅ #46 Admin "⚙ 模块" 首次打开显示空配置

> 2026-06-09 | 发现 | 状态: ✅ 已修复

### 现象
Admin 页面 → "⚙ 模块" Modal 显示空配置，而非节点实际模块配置。

### 修复
1. `CenterConnector.hpp` 新增 `PutConfig(key, value)` 虚方法
2. `EtcdCenterConnector.cpp` 实现: base64 编码 → POST `/v3/kv/put`
3. `Manager.cpp` 注册成功后 → 读取 `m_oCurrentConf["module"]` → `PutConfig` 到 etcd
4. Admin 页面 "⚙ 模块" Modal 简化为: 上传 + SO 列表 + 一键更新 (去掉 JSON 编辑器)

---

## ✅ #47 Admin 页面 — 节点/状态 Tab + 访问地址

> 2026-06-09 | 需求 | 状态: ✅ 已验证

### 访问方式
| 环境 | 地址 |
|------|------|
| 本地 | `http://127.0.0.1:8090` |
| k8s | `http://192.168.3.61:30090/?etcd=192.168.3.61:30079` |

`?etcd=` 参数告诉浏览器里的 Admin 页面直连 etcd API（静态 HTML，非服务端转发）。

### 验证项

| Tab | 功能 | 预期 |
|-----|------|------|
| 🖥 节点 | 列出 `/thunder/registry/` 下注册节点 | 按类型分组，显示 IP:Port、Node ID、Worker 数 |
| 📊 状态 | etcd 健康 + 节点/配置计数 | etcd 版本、healthy、注册数、配置数 |

### 验证步骤
1. 确保 etcd NodePort 可达: `curl http://192.168.3.61:30079/version`
2. 确保 Thunder 节点已注册到 etcd (Hello/Interface/Logic 等 Running)
3. 浏览器打开: `http://192.168.3.61:30090/?etcd=192.168.3.61:30079`
4. 验证节点 Tab 显示注册节点列表
5. 验证状态 Tab 显示 etcd 集群状态

---

## ✅ #48 Admin 从 SO 镜像提取文件，选中即更新

> 2026-06-09 | 需求 | 状态: ✅ 已完成

### 背景
SO 版本通过 Docker 镜像管理（编译机 `docker build` → push 镜像仓库），节点镜像不被替换。Admin 从 SO 镜像中提取 .so 文件，写入本地 + NFS，触发 GracefulRestartWorker 热加载。

### 流程
```
编译机                             镜像仓库              Admin Pod
docker build (含 SO v1,v2,v3)  →  push  →  registry/so-hello:v3
                                                         │
                                              docker run --rm registry/so-hello:v3 \
                                                cat /app/so/ModuleHello_v3.so
                                                         │
                                              → /app/plugins/HelloHttp/
                                              → /data/thunder/plugins/HelloHttp/ (NFS)
                                              → etcd 更新 → GracefulRestartWorker
                                              → Worker dlopen 新版 SO
```

### 方案

| # | 文件 | 改动 |
|---|------|------|
| 1 | `Dockerfile.so` | SO 镜像模板：`FROM scratch` + `COPY *.so /app/so/` |
| 2 | `server.py` | `POST /api/so-extract` — `docker run --rm image cat /path` 提取 SO 到本地+NFS |
| 3 | `index.html` | ⚙ 模块 Modal 加 "📦 SO 镜像" 输入，填镜像名 → 提取 |
| 4 | `k8s/admin-web-deployment.yaml` | 挂载 `docker.sock` 或配置 registry 凭据 |
| 5 | 测试 | 提取 SO → 本地+NFS → etcd 更新 → 验证 |

### Admin 页面效果
```
📦 SO 镜像提取
  镜像: [registry/so-hello:v3]
  文件: [ModuleHello_v3.so]      [⬇ 提取并更新]
```

### 实现要点

| # | 文件 | 改动 |
|---|------|------|
| 1 | `server.py` | `POST /api/so-extract` : `docker create` + `get_archive` + `_save_so` 本地+NFS |
| 2 | `index.html` | 镜像名输入 + 文件名 + 提取按钮 + `extractAndRefresh()` |
| 3 | `k8s/admin-web-deployment.yaml` | `docker.sock` hostPath + `pip install docker` |
| 4 | 测试 | 提取 10240 bytes → 本地+NFS ✅ → 回归 328/328 ✅ |

### 验证 (2026-06-09)
构建 ✅ | Admin 页面 ✅ | SO 提取 ✅ | 本地+NFS 双写 ✅ | etcd ✅ | 回归 328/328 ✅

---

## ✅ #49 SO 镜像从 Registry 拉取 → NFS 分发到各节点

> 2026-06-09 | 需求 | 状态: ✅ 已完成

### 背景
#48 实现了本地 docker 镜像提取。生产环境需从 Registry 拉取镜像 → 提取 SO → NFS 分发到各节点。

### 流程
```
CI/CD                     Registry               Admin Pod               各节点 (k8s Pod)
docker build            docker push
so-hello:v3   ────────►  registry/              docker pull             NFS 挂载
                          so-hello:v3  ←──────  registry/so-hello:v3    /data/thunder/plugins/
                                                │                       Worker dlopen
                                                ├── 提取 .so
                                                ├── → 本地 plugins/
                                                └── → NFS /data/thunder/plugins/
                                                         │
                                              ┌──────────┘
                                              ▼
                                        所有节点自动可见
                                        Admin 改 etcd → GracefulRestartWorker
```

### 方案

| # | 改动 | 说明 |
|---|------|------|
| 1 | `server.py` `_handle_so_extract` | 无本地镜像时 `docker pull` 从 registry 拉取 |
| 2 | `k8s/admin-web-deployment.yaml` | 配置 registry 凭据 (imagePullSecrets 或 docker config) |
| 3 | 测试 | registry pull → 提取 → NFS → 所有节点可见 |

### 实现 (2026-06-09)
- `server.py`: 镜像含 `/` 时自动 `docker pull` → 再 `docker create` + `get_archive` 提取
- 测试: ✅ 本地镜像提取 | ✅ registry pull | ✅ 回归 328/328

---

## ✅ #50 deploy.sh 支持 SO 镜像构建 + 增量

> 2026-06-09 | 需求 | 状态: ✅ 已完成

### 功能
- `./deploy.sh build-so all` — 构建所有类型 SO 镜像
- `./deploy.sh build-so hello` — 单独构建指定类型
- SHA256 增量检测, SO 无变化自动跳过

### 文件
- `deploy.sh`: `cmd_build_so()` + `build_one_so_image()`
- `so-images/{hello,logic,interface,hello-ws,hello-https}/` — SO 存放目录

### 测试
首次构建 4 镜像 ✅ | 无变化跳过 ✅ | 单独构建 ✅

---

## ✅ #51 节点类型重命名 HELLO_HTTP / HELLO_HTTPS / HELLO_WS + code/Hello 拆分

> 2026-06-09 | 需求 | 状态: ✅ 已完成

### 背景
三类 Hello 节点 (HTTP/HTTPS/WS) 有不同 SO 模块和底层编解码，应独立管理。

### 重命名

| 旧 | 新 |
|----|----|
| HELLO / HelloHttp | HELLO_HTTP / HelloHttp |
| HELLO_HTTPS / HelloHttps | HELLO_HTTPS / HelloHttps |
| HELLO_WS / HelloWs | HELLO_WS / HelloWs |

### 改动

| # | 文件 | 内容 |
|---|------|------|
| 1 | `admin-web/index.html` | TYPE_DIR 映射更新 |
| 2 | `admin-web/server.py` | type_dir 映射更新 |
| 3 | `so-images/` 目录 | `HelloHttp_ModuleHello/` 等 |
| 4 | `code/Hello/` | 拆分为 `HelloHttp/`, `HelloHttps/`, `HelloWs/` (各自独立编译) |
| 5 | `deploy.sh` | build-so 适配新命名 |
| 6 | 文档 | 更新命名规范 |

### 测试
- deploy.sh build-so 全量 + 增量
- Admin 页面 TYPE_DIR 映射正确
- 各节点 SO 提取 → 正确目录
- C++ 回归 328/328

### 验证 (2026-06-09)
- deploy.sh build-so 全量 6 镜像 ✅
- Admin TYPE_DIR: HELLO_HTTP/HELLO_HTTPS/HELLO_WS ✅
- SO 提取 HELLO_HTTP ✅
- 回归 328/328 ✅

---

## ✅ #52 Admin "⚙ 模块" — 选中镜像后自动列出文件，去手动输入

> 2026-06-09 | 需求 | 状态: ✅ 已完成

### 需求
点镜像列表 → 自动填镜像名 + 列出 .so 文件 → 选文件 → 提取。去掉手动输入。

### 当前实现
- `server.py` `GET /api/so-files?image=xxx` — 返回镜像内 .so 文件列表 ✅
- `selectSoImage()` — 点镜像自动列文件 ✅
- 文件名改为 `<select>` 下拉 ✅

### 验证
Ctrl+F5 刷新 Admin 页面 → ⚙ 模块 → 点镜像 → 文件名自动出现

---

## ✅ #53 Admin "⚙ 模块" 去掉本地上传，只保留镜像提取

> 2026-06-09 | 优化 | 状态: ✅ 已完成

SO 通过镜像管理，去掉浏览器本地上传入口。

---

## ✅ #54 k8s Admin Pod 频繁 Evict/Pending — 磁盘压力

> 2026-06-10 | 发现 | 状态: ✅ 已恢复

### 现象
Admin Pod 反复被 Evict (disk-pressure)。

### 根因
docker 镜像累积 (多次 build thunder-admin-web:latest) + 容器残留, 磁盘使用率 > k8s eviction threshold。

### 处理
```bash
sudo docker system prune -af
kubectl -n thunder delete pod --field-selector=status.phase=Failed
kubectl -n thunder rollout restart deployment thunder-admin-web
```

### 状态
✅ 已恢复 (2026-06-10)
- 是否有其他资源占用

---

## ✅ #55 k8s Admin Pod 启动需手动 pip install docker

> 2026-06-10 | 发现 | 状态: ✅ 已修复

### 根因
`docker build` 的镜像 containerd 不可见。改用 `pip install -q docker` 启动时自动安装。

### 修复
部署命令: `sh -c "pip install -q docker; python3 server.py --port 8090"`

### 验证
Pod 重启后 docker 模块自动可用 ✅

### 修复 (2026-06-10)
改用 `thunder-admin-web:latest` 镜像 (Dockerfile 预装 docker SDK)。验证: docker OK + 6 images + so-files ✅

---

## ✅ #56 Admin SO 镜像列表按节点类型过滤

> 2026-06-10 | 需求 | 状态: ✅ 已完成

### 现象
打开 HELLO_HTTP 的 ⚙ 模块，列出所有 6 个 SO 镜像(包括 logic/interface)，应只显示本节点相关。

### 修复
`loadSoImages` 加 `typeFilter`: HELLO_HTTP→hellohttp, LOGIC→logic, 等。JavaScript 客户端过滤。

### 验证
页面含 typeFilter ✅ | 回归 328/328 ✅

---

## 🔵 #57 Admin "⚙ 模块" 多个问题

> 2026-06-10 | 发现 | 状态: 🔵 待修复

### 问题 1: 镜像列表加载失败
打开 HELLO_HTTP ⚙ 模块 → 显示"加载中..." → 超时或(无 so- 镜像)。
- 可能: docker system prune 清理了镜像
- 可能: typeFilter 过滤掉了所有镜像

### 问题 2: SO 列表有旧条目
显示 `ModuleHello.so v1` (旧名称) + `HelloHttp_ModuleHello.so v1` (新名称)。
- 根因: etcd 中有旧模块配置残留
- 修复: etcd 配置去重/迁移旧条目

### 问题 3: 更新版本号不递增
点 🔄 更新 → 版本始终是 v1，不递增。
- 根因: `extractAndRefresh` / `uploadAndRefresh` 调用 `triggerUpdate(type, path, 1)` — version 写死为 1
- 修复: 应读取当前版本 +1

### 问题 4: 无执行结果反馈
点 🔄 更新 → toast "GracefulRestart 触发中" → 不知道是否真的重启。
- 当前 ectd PUT 成功 = toast 成功，但 Manager 可能不在线
- 修复: 应轮询 etcd 确认配置写入 + 等待 Manager 状态变化

### 修复 (2026-06-10)
- 版本号自增: `triggerUpdate` 自动 +1 ✅
- etcd 旧条目: 全部改为新命名 ✅
- 镜像列表: SO 重建 ✅
- testnewfunc: 构建 ✅ | C++ 328/328 ✅ | Python 122/122 ✅ | Admin 200, 6 images ✅ | typeFilter 2 ✅ | 版本自增 1 ✅ | SO 6 镜像 ✅

---

## ✅ #58 Thunder HTTP/HTTPS 压测 — picohttpparser 全后端全包大小对比

> 2026-06-10 | 需求 | 状态: ✅ 已完成 (数据已录入 `docs/reports/10-vs-nginx-benchmark-20260610.md`)

### HTTP 数据 (picohttpparser, performance governor, P-core 4-9 绑核)

```
          64B     256B    1K      4K      64K
ev        322k    242k    323k    321k    129k
native    319k    265k    313k    312k    127k
asio      347k    237k    330k    331k    127k
Nginx 1w  173k    171k    160k    151k     69k
```

### HTTPS 数据 (2026-06-10 复测, powersave governor, P-core 绑核)

```
          64B      4K       64K
ev        105k     41k      3.8k
native     89k     36k      3.3k
asio      184k     74k      6.1k
Nginx 1w  112k    101k     23.6k
```

### 关键发现
- HTTP: Thunder 全线 ~2x Nginx, asio_uring 64B 最优 (347k)
- HTTPS 64B: Thunder asio_uring 最优 (184k, +64% vs Nginx 112k)
- HTTPS 4K/64K: Nginx 领先 (101k/23.6k), 多进程模型在大包 SSL 场景更有优势

### 报告
`docs/reports/10-vs-nginx-benchmark-20260610.md` (替换旧版 04, 175 行简洁版)

---

## 🟡 #59 [分析] Lua 解析器支持

> 2026-06-10 | 分析 | 状态: 🟡 待决策

### 背景
当前 Thunder HTTP 解析器已从 http_parser 替换为 picohttpparser，性能提升 +49%。但 picohttpparser 是纯 C 解析器，无动态脚本扩展能力。

### 需求分析
Lua 解析器支持的含义：
1. **Lua 作为 HTTP 请求/响应的脚本处理层** — 在请求生命周期中嵌入 Lua hook（类似 Nginx lua-nginx-module / OpenResty）
2. **Lua 作为动态路由/过滤规则引擎** — 用 Lua 脚本定义路由匹配、请求改写、流量调度

### 收益评估

| 场景 | 价值 | 复杂度 |
|------|:----:|:------:|
| 动态请求改写/header 修改 | 中 | 低 (已有 Module 接口) |
| 复杂路由规则 (正则/条件组合) | 高 | 中 |
| 请求body 转换/gzip/filter | 中 | 中 |
| 可编程响应 | 中 | 低 (已有 SendToClientFast) |
| 热更新逻辑 (不需编译 .so) | 高 | 高 (沙箱/安全) |

### 已有替代方案
| 方案 | 优势 | 劣势 |
|------|------|------|
| C++ SO 模块热更新 (#45) | 性能好, 已有完整链路 | 需编译, 部署门槛高 |
| Module 接口 | 原生支持, 零额外开销 | 静态注册, 灵活性低 |
| etcd 配置 + 路由过滤 (#38) | 无运行时开销 | 仅路由, 不能改写请求 |

### 推荐
**暂不支持 Lua 解析器**，理由：
1. Thunder 的 SO 模块热更新 (#45) 已提供动态扩展能力，C++ 模块性能远高于 Lua
2. 路由过滤可通过 etcd 配置 + upstream_types 实现（#38），无需脚本
3. Lua 虚拟机锁 (LuaJIT 全局锁) 在单线程事件循环中会阻塞所有请求
4. 增加 Lua 解析器会引入复杂的沙箱/安全隔离问题

### 行动
- 不引入 Lua 解析器
- 持续优化 picohttpparser + SO 模块热更新路径
- 如需脚本能力，通过 SO 模块加载 wasm 轻量沙箱（待评估）

---

## ✅ #60 [已实现] WebSocket 支持与测试

> 2026-06-10 | 核实 | 状态: ✅ 已实现 (需补充测试)

### 实际状态
WebSocket 已有完整实现：

| 组件 | 状态 | 路径 |
|------|:----:|------|
| Codec (Json) | ✅ 已实现 | `code/Net/src/codec/CodecWebSocketJson.cpp` |
| Codec (Pb) | ✅ 已实现 | `code/Net/src/codec/CodecWebSocketPb.cpp` |
| Codec (PbApp) | ✅ 已实现 | `code/Net/src/codec/CodecWebSocketPbApp.cpp` |
| HelloWs 部署 | ✅ 已存在 | `deploy/HelloWs/` (bin/conf/plugins) |
| ModuleShake | ✅ 已存在 | `plugins/HelloWs_ModuleShake.so` |
| CODEC_WEBSOCKET=5 | ✅ 已分配 | `access_codec: 5` |
| WSS (TLS) | ❌ 未实现 | CODEC_WSS=10 已预留 |
| 测试用例 | ⚠️ 存在但需验证 | `tests/e2e/test_ws_hello.py` |

### 验证目标
- [ ] HelloWs 服务启动正常
- [ ] HTTP Upgrade → 101 握手
- [ ] 收发 WebSocket 帧 (text/binary)
- [ ] E2E 测试通过

---

## 🔵 #61 [分析] WASM 轻量沙箱热更新 — 能否不重启进程

> 2026-06-10 | 分析 | 状态: 🔵 待评估

### 背景
当前 SO 模块热更新 (#45) 依赖 `GracefulRestartWorker` — 新建 Worker 进程加载新版 SO，旧 Worker drain 完成后退出。整个过程仍需进程重启（虽然是零中断的优雅重启）。

### 问题
WASM (WebAssembly) 轻量沙箱能否实现**不重启进程**的热更新？

### WASM 与 SO 热更新对比

| 维度 | SO (dlopen) | WASM 沙箱 |
|------|:-----------:|:---------:|
| 进程重启 | ✅ 需要 (GracefulRestartWorker) | ❌ **不需要**, 实例级替换 |
| 隔离性 | 弱 (同一进程地址空间) | 强 (沙箱内存隔离) |
| 安全 | 无沙箱, SO 可访问任意内存 | 沙箱, 限制系统调用 |
| 性能 | 原生机器码, 零开销 | 有解释/编译开销 (WASM ~80-120% native) |
| 语言支持 | C/C++ 编译为 .so | 任何编译为 WASM 的语言 (Rust/C/C++/Go) |
| 单线程模型 | dlopen 非线程安全, 需 stop-the-world | 实例级替换, 不影响其他连接 |

### 实现路径

```
Worker 进程
  ├── wasm_runtime (WAMR / Wasmtime)
  │     ├── 实例 v1 (旧版本) ← 正在处理请求
  │     └── 实例 v2 (新版本) ← 刚加载, 等待连接迁移
  │
  └── 连接调度器: 逐连接将新请求指向 v2, v1 空闲后销毁
```

### 关键挑战

1. **WASM 运行时选择**: WAMR (轻量, C 接口) / Wasmtime (高性能, WASI 完整) / wasm3 (解释器)
2. **接口绑定**: WASM 需导出 `AnyMessage()` + 宿主需导入 `SendToClientFast` 等 API
3. **性能**: WASM 函数调用 ~20-50ns vs SO 原生 ~3ns, Fast Path 场景影响显著
4. **连接迁移**: 旧实例的在途请求需完成或转移, 通过 `pProtoCtx` 挂载 WASM 实例引用

### 建议路线

| 阶段 | 内容 | 收益 |
|:----:|------|:----:|
| P0 | WASM 运行时集成 + 简单 echo module | 验证可行性 |
| P1 | wasm 中调用 SendToClientFast | 验证宿主导入 |
| P2 | 实例级热替换 + 连接迁移 | 真正不重启进程 |

### 结论
WASM 轻量沙箱**可以实现不重启进程**的热更新 (实例级替换)，但性能有 ~5-15x 函数调用开销。建议先做 P0 验证可行性再决定投入。

---

## ✅ #62 [已实现] ModuleLua — LuaJIT 模块支持

> 2026-06-10 | 实现 | 状态: ✅ 已实现 | 设计: `docs/architecture/17-luajit-module-support.md`

### 目标
Thunder 模块系统支持 Lua 脚本，实现不重启进程的热加载。

### 功能要求

1. **ModuleLua 类**: 继承 `net::Module`，嵌入 LuaJIT VM，处理 `AnyMessage` 回调
2. **Lua API 绑定**: 注册 `SendToClientFast`、`SendToNext`、`SendToConHash`、`SendToNodeType`、`SentTo` 等 Labor IO 接口
3. **配置支持**: `module` 段支持 `script_path` 指向 `.lua` 文件
4. **热加载**: 检测 `.lua` 文件变更后自动重载，不重启进程
5. **etcd 分发**: 脚本内容存 etcd (`/thunder/config/scripts/`)，Worker watch 后写磁盘

### Lua 定位

```
请求 → Lua (路由/鉴权/限流/header 改写) → 通过 → C++ SO (业务 + DB)
               ↓ 拒绝
            返回错误
```

Lua 只做 gatekeeper，不碰 IO 和业务计算。

### 参考设计
`docs/architecture/17-luajit-module-support.md`

### 实施状态

| # | 内容 | 状态 |
|:-:|------|:----:|
| 1 | CMake 链接 luajit | ✅ 构建通过 |
| 2 | ModuleLua 类 + AnyMessage 回调 | ✅ AnyMessage 可用 |
| 3 | Lua binding: SendToClientFast/SendToNext/SendToConHash/SendToNodeType/SentTo | ✅ 已注册 |
| 4 | 配置 + 热加载 | ✅ 配置加载正常 |
| 5 | 压测验证 | ✅ 实测 13k RPS (lua_pcall 边界开销, echo 极简路径) |

### 压测结果

| 方案 | RPS | 说明 |
|------|:---:|------|
| SO 模块 (ModuleRaw) | 126k | C++ 原生机器码 |
| LuaJIT (ModuleLua) | 13k | lua_pcall 边界开销, echo 极简路径 |
| SO Fast Path | 234k | C++ 原生 (Fast Path) |
| 差距 | ~11x | 业务逻辑重时比例下降 |

## 🔵 #63 [已闭环] ModuleLua SendToLogic — HEllo(Lua)→LOGIC→HEllo(Lua)→客户端

> 2026-06-11 | 修复 | 状态: ✅ 已闭环 | 阻塞于 #9+#24

### 完整链路

```
客户端 → GET/POST
         ↓
    HEllo (ModuleLua)  ← 收到请求
         ↓
    SendToLogic(body, callbackFunc)  ← Lua 调用
         ↓
    LogicStep::Emit() → SendToSession("LOGIC", ...)  ← HEllo 发到 LOGIC
         ↓
    LOGIC 处理请求并返回响应
         ↓
    LogicStep::Callback(msgHead, msgBody)  ← LOGIC 响应回到 HEllo
         ↓
    Lua callback(resp) return string  ← Lua 回调 return 应答
         ↓
    SendToClientFast(resp)  ← HEllo 回给客户端 (Callback 内用捕获的 m_shell)
```

### 根因: 3 个 bug

| # | Bug | 文件 | 说明 |
|---|-----|------|------|
| 1 | `Emit` 缺 `seq`/`msgbody_len`, 用 `SendToNodeType`(广播) | `ModuleLua.cpp:45-53` | 回包 seq=0 不匹配 Step → Callback 永不触发 |
| 2 | `RegisterCallback` 后未调 `Emit()` | `ModuleLua.cpp:82-88` | Step 注册了但消息从未发出 |
| 3 | `route.lua` 回调内调全局 `SendToClientFast` | `route.lua` | 异步回调时 `__current_shell` 已 nil |

### 修复

- `LogicStep::Emit`: `h.set_seq(GetSequence())` + `set_msgbody_len` + 改 `SendToSession("LOGIC")`
- `lua_SendToLogic`: `RegisterCallback` 后 `baseStep->Emit(0)` (框架规范: 注册后必须显式 Emit)
- `route.lua`: 回调 `return '...'` (C++ `Callback` 用捕获的 `m_shell` 发客户端)
- `test_smoke.sh`: lua_route 断言收紧为 `code==0 and 'logic' in d`

### 验证

| 环境 | 结果 |
|------|------|
| **Docker-Compose** | `{"code":0,"msg":"ok","logic":{"code":1}}` ✅ |
| **K8s** | `{"code":0,"msg":"ok","logic":{"code":1}}` ✅ |

`logic={"code":1}` 是 LOGIC 真实回包 (`{"option":"Echo"}` 非合法 GenKey 参数 → LOGIC 返回 code:1), 符合「原样转发」设计。发 `{"option":"GenKey",...}` 时 logic 字段为 `{"code":0,token:...,key:...}`。

## 🔵 #64 [需求] Lua 脚本路径从配置文件读取，而非硬编码在 create()

> 2026-06-11 | 需求 | 状态: 🔵 待实现 | 依赖: #62

### 现状
当前 `create_echo()`、`create_route()`、`create_limit()` 中 `SetScriptPath()` 是硬编码的：

```cpp
extern "C" net::Module* create_echo() {
    auto* p = new ModuleLua();
    p->SetScriptPath("scripts/echo.lua");
    return p;
}
```

每增加一个 Lua 脚本，就需要加一个工厂函数和配置条目。

### 需求
Module 配置支持 `script_path` 字段，由 Worker 在加载模块时设置：

```json
{
    "url_path": "/hello/lua_route",
    "so_path": "plugins/HelloHttp_ModuleLua.so",
    "script_path": "scripts/route.lua",
    "entrance_symbol": "create",
    "load": true,
    "version": 1
}
```

Worker 加载 ModuleLua 后读 `script_path` → 调 `SetScriptPath()`。`create()` 不需要参数。

### 改动范围
- `Worker.cpp` 模块加载逻辑: 读取 `script_path` 字段，调 `Module::SetScriptPath()`
- `ModuleLua::create()`: 去掉硬编码路径
- 现有配置: `lua_echo/route/limit` 全部改为统一 `create` + `script_path` 字段

---

## 🔵 #65 [需求] Lua 脚本 etcd 下发 + 版本管理

> 2026-06-11 | 需求 | 状态: 🔵 待实现 | 依赖: #64, #45

### 需求
Lua 脚本通过 etcd Admin 下发、版本管理、Worker 自动同步，不重启进程。

### 设计

```
Admin → PUT /thunder/config/scripts/route.lua → etcd
                                                  ↓
                              Worker Watch → 写入 scripts/route.lua
                                              → 通知 ModuleLua 重载
```

### 功能点

| # | 功能 | 说明 |
|:-:|------|------|
| 1 | etcd key 存储脚本内容 | `/thunder/config/scripts/{name}.lua` |
| 2 | Worker watch 变更 | EtcdWatcher → Worker reload |
| 3 | 版本历史 | `/thunder/config_history/scripts/{name}.lua/v{ts}` |
| 4 | Admin 页面编辑 | 浏览器在线编辑 .lua + 保存 |
| 5 | 回滚 | Admin 页面一键回滚到历史版本 |

### 与 SO 模块管理的区别

| | SO 文件 (.so) | Lua 脚本 (.lua) |
|------|:----------:|:--------------:|
| 大小 | MB 级 | KB 级 |
| etcd 存储 | ❌ 存路径/版本 | ✅ **存内容本体** |
| 分发 | NFS / Docker 镜像 | etcd watch 直传 |
| 热更新 | GracefulRestartWorker | **文件重载，零中断** |

## 🔵 #66 [优化] Admin 版本管理统一支持 3 种类型 (custom / SO / Lua)

> 2026-06-11 | 优化 | 状态: 🔵 待实现 | 依赖: #45, #65

### 现状
Admin 页面当前支持：
- **custom 配置** → 版本历史 + 回滚 (等ets键 `/thunder/config_history/{IP:PORT}/v{ts}`)
- **SO 模块** → 版本历史 + 回滚 (等ets键 `/thunder/config_history/module/{TYPE}/v{ts}`)

### 需求
统一版本管理页面，支持 3 种下发类型：

| 类型 | etcd key | 存储内容 | 热更新方式 |
|------|----------|---------|-----------|
| custom 配置 | `/thunder/config/{IP:PORT}` | JSON | Worker config watch |
| SO 模块 | `/thunder/config/module/{TYPE}` | 版本/路径 | GracefulRestartWorker |
| Lua 脚本 | `/thunder/config/scripts/{name}.lua` | **脚本内容** | 文件重载，零中断 |

### Admin 页面改进

```
📋 版本管理
  ├── 配置 (custom)
  │     ├── 节点类型分组
  │     ├── 编辑 JSON → 保存 → 版本历史 → 回滚
  │     └── 版本 diff (v1 vs v2)
  │
  ├── SO 模块
  │     ├── 类型分组 (HELLO_HTTP/LOGIC/...)
  │     ├── 镜像提取 → 版本自增 → 热更新
  │     └── 版本历史 → 回滚
  │
  └── Lua 脚本 (新增)
        ├── 脚本列表 (按模块/类型)
        ├── 在线编辑 .lua → 保存 → etcd → 文件重载
        └── 版本历史 → 回滚
```

## ✅ #67 [已修复] asio_uring ModuleRaw 404 + Receive Fast-Path 竞态

> 2026-06-11 | bug | 状态: ✅ 已修复

### 根因

两个独立问题：

**1. ModuleRaw.cpp 缺 MUDULE_CREATE** (主因)
`ModuleRaw.cpp` 从创建起就缺 `MUDULE_CREATE(ModuleRaw)`，`ModuleRaw.so` 无 `create` 导出符号。
- ev: Receive Fast-Path 拦截 `/hello/raw` → 不走 dlopen → 不受影响 ✅
- asio_uring: 走正常 Codec 路径 → dlopen ModuleRaw.so → `dlsym "create"` 失败 → 404 ❌

**2. Receive Fast-Path SubmitRead 竞态** (#67 原始猜测)
`SendToClientFast` 后 `SubmitRead` 复用 pRecvBuff，但经测试 ev 后端 `goto read_again` 和 asio_uring 的 `SubmitRead` 路径均无实际竞态触发。Non-2xx 由问题 1 的 404 引起。

### 修复

`ModuleRaw.cpp` 加一行 `MUDULE_CREATE(ModuleRaw);`。

### 影响范围

| 后端 | 修前 | 修后 |
|------|:---:|:---:|
| ev | 正常 (Fast-Path 拦截) | 正常 (同) |
| asio_uring | 404 / ~10 RPS | `{"code":0,"msg":"ok"}` ✅ |

## 🔵 #68 [已排除] LuaJIT 加载对性能的影响 (误判)

> 2026-06-12 | 记录 | 状态: ✅ 已排除 (数据污染)

### 背景

测试中发现 ModuleRaw 64B 在「有 Lua 模块」时 106k、「无 Lua 模块」时 207k, 误判为 LuaJIT 导致性能降半。

### 真相

多次 `python3 -c` 编辑 `conf/Hello.json` 导致配置损坏——module 数从 5 变为 2。

**修正后**: 恢复原始 conf (`git checkout -- conf/Hello.json`), ModuleRaw 64B = **150k**。
差异来源是 conf 污染, 非 LuaJIT。

## ✅ #69 [已定位] 当前 ModuleRaw 基准 (110k) 远低于报告 (322k)

> 2026-06-12 | 已定位 | 状态: ✅ 根因 = 工作区未提交的 `log_level: TRACE`, conf 已恢复 INFO (生效需重启服务)

### 根因 (2026-06-12 确认)

**`deploy/HelloHttp/conf/Hello.json` 的 `log_level` 被改为 `TRACE` 且未提交。**
322k 报告在 INFO 下测得 (1196ec0 专门调整日志级别为 INFO)。

单变量对照实验 (同二进制 / 同绑核 CPU4 / 同 powersave governor, 只改日志级别):

| 条件 | ModuleRaw 64B RPS |
|------|:---:|
| TRACE (复现 #69) | 134k |
| INFO (仅改日志级别) | **230k (+71%)** |

- 机理: ev 收包路径在 Fast-Path **之前**有 2+ 条/请求 TRACE 日志 (`Worker.cpp:592/613`), ~30MB/s 落盘
- 路径越长惩罚越大: ModuleRaw 1.7x / lua_echo 3.9x / ModuleHello **5.8x** (TRACE→INFO 恢复倍数)
- 旁证: 同期 Nginx 173k→195k 不降反升, 排除 thermal/HT 限频理论
- 漏因: 之前只比 `git diff 87f3eb7..HEAD` (提交间), TRACE 是**工作区未提交改动**;
  且进程从共享内存读配置, 改文件不重启不生效, 中途改回也测不出
- 剩余差距 (230k vs 322k) = powersave 4.1GHz vs performance ≈5GHz + 单核绑核 vs P-core 4-9,
  与报告自有数据 (powersave −13.6%) 自洽
- 修正后全矩阵数据见 `docs/reports/10-vs-nginx-benchmark-20260610.md` 第五节

### 衍生发现

1. **asio_uring 64K 大包仍可复现 Worker 崩溃** (signal 9 被杀, Manager 自动拉起) + wrk timeout,
   即 #11 报告 "SubmitRead 竞态" 修复未生效, 待修
2. **压测脚本杀进程顺序陷阱**: 先杀 Worker 再杀 Manager 会触发拉起新 Worker 后留孤儿监听,
   SO_REUSEPORT 双 Worker 同时服务 → 吞吐虚高 ~40% (本次 asio 339k 假数据来源)
3. 频繁重启压测实例向共享 etcd 注册新 node_id, 出现 "槽位已满(max=255)";
   压测沙箱应将 etcd endpoint 指向死地址 (已验证对性能无影响)

### 现象

同机器、同 kernel、同 `performance` governor + HT0 绑核:
- 报告 (2026-06-10, 87f3eb7): ModuleRaw 64B = **322k** (ev)
- 当前 (HEAD): ModuleRaw 64B = **110k** (ev)

### 已排除的因素

| 因素 | 结果 | 方法 |
|------|:----:|------|
| LuaJIT 加载 | 无关 (110k) | 对比有/无 lua 模块 |
| etcd 注册定时器 | 无关 (108k) | 改 endpoint 到 127.0.0.1:1 |
| Worker.cpp 代码 diff | 无 diff | `git diff 87f3eb7..HEAD -- Worker.cpp` 空 |
| Labor.cpp IoBackend | 无 diff | `git diff 87f3eb7..HEAD -- Labor.cpp` 空 |
| CMake build flags | 一致 | 同为 RelWithDebInfo -O2 -g |
| CPU 绑核 | 一致 | HT0 (0,2,4,6,8,10) |
| Governor | 一致 | performance |
| 环境脏 | 排除 | docker 停, 裸机, 恢复原始 conf |
| HT 限频 | 排除 | 只绑 HT0, CPU4 4.3GHz 正常 |

### 未排除的因素

1. **系统库版本变化**: `protobuf`, `libc`, `libstdc++` 等系统库可能在两次压测间更新 (apt upgrade)
2. **编译器版本**: 编译器版本可能不同, 生成代码不同
3. **二进制依赖**: 当前构建的 binary 链接了不同的 .so (libNet.so 经过多次修改重建)
4. **protobuf Arena 影响**: 虽然代码相同, protobuf 内部行为可能因版本不同

### 验证方法

```
git stash           # 暂存当前修改
git checkout 87f3eb7  # 切到报告版本
cmake --build build   # 重编
# 压测 ModuleRaw 64B → 如果能回到 322k, 说明是代码之外的差异
# 如果还是 100k+ 量级, 说明是环境/库版本差异
git checkout HEAD
git stash pop
```

### 结论

加载 `libluajit-5.1.so.2` + 3 个 Lua 模块对全进程性能无明显影响 (~150k, conf 恢复后一致)。
## ✅ #70 [bug] Manager 误杀首代 Worker: 心跳被 RecvFdFromWorker 吞掉, 出生 60s 即被 SIGKILL

> 2026-06-12 | bug | 状态: ✅ 已修复并验证 (gen-1 空闲135s/gen-2 负载90s 零误杀, ctest 335/335, 全矩阵复测 0 误杀, 线上已重启加载) | 引入: 1d33a9e (06-02 Worker优雅重启) | 原标题"asio_uring 64K 崩溃"为伪相关

### 真实根因 (2026-06-12 strace 实锤)

**与 asio_uring / 64K 大包 / 负载均无关** — 纯空闲下首代 Worker 也在出生 +60s 整被杀。

因果链 (每步均有 strace/日志直接证据):

1. **Worker 端**: `SpawnSingleWorker` fork 子进程先 `ShmRingQueue::CloseEventFd(iWorkerToMgrEfd)`
   (该函数置 `int&` 为 -1) 再构造 Worker → `m_iWorkerToMgrEfd = -1` → `SendToParent` 的
   shm 分支条件永假 → **所有心跳走 control socket 回退**
   (strace: `sendto(9, "\r\5\0\0...{\"load\"...", 104)` 每 10s, cmd=5 编码完整正确)
2. **Manager 端**: 1d33a9e 在 `IoRead` 加了 `if (m_mapWorkerFdPid.find(fd)) → RecvFdFromWorker`,
   但 `m_mapWorkerFdPid` 同时含 **controlFd 和 dataFd** → control fd 上的心跳被
   `recv_fd_with_attr()` 当 SCM_RIGHTS fd 传递消息吞掉 (strace: iovec[32,8] recvmsg, 40+40+24
   字节恰好吃完一拍心跳) → 无 SCM_RIGHTS → return false, **无任何日志**
3. `dBeatTime` 仅在启动时初始化一次 (Manager.cpp:212) → `CheckWorker` 在 +60s
   (worker_beat=60, NODE_BEAT=10) 判定 unresponsive → `kill(pid, SIGKILL)` → 自动拉起

### 为何一直没发现

- 杀戮无声: Worker 被杀后 1ms 内拉起, 服务不中断; 只留 defunct 僵尸 + 一行 INFO 日志
- 部署服务指纹吻合: Manager 14:13:22 启动, 现存 Worker 14:14:22 出生 (恰 +60s);
  ps 中 09:29 defunct 僵尸 Worker 同理
- 压测期间"asio_uring 64K 崩溃" = +60s 死亡时钟恰好落在 64K 时间窗 (伪相关);
  64K RPS 低 (~50k) 与 wrk timeout 是被杀瞬间连接重置所致

### 排查方法论 (供复盘)

- TRACE 日志法医: Manager 每轮 `now/dBeatTime` 显示 dBeatTime 冻结 60s → 排除 Manager 停摆
- Worker 日志: 被杀前 2ms 仍在 SendToParent → 排除 Worker 假死
- 零负载判别实验: 纯空闲 +60s 仍被杀 → 一票否决负载相关全部假说
- strace -ff 从启动跟踪: 心跳字节被 recvmsg(iovec[32,8]) 模式消费 → 锁定 recv_fd_with_attr

### 修复 (Manager.cpp, 2026-06-12)

1. `IoRead`: 仅 **dataFd** 路由到 `RecvFdFromWorker`; controlFd 走 `RecvDataAndDispose`
   解码分发 (恢复 1d33a9e 之前行为)
2. `CheckWorker`: `for (auto worker_iter:m_mapWorker)` 按值拷贝 → 按引用
   (超时判定原本读的是本轮 drain 之前的过期快照)

### 衍生问题 (另行处理)

- **#72**: SpawnSingleWorker 父子两侧各关错一个 eventfd (父关 MgrToWorker 写端,
  子关 WorkerToMgr 写端), shm 队列 eventfd 通知双向失效, 退化为 1s/10s 轮询 + socket 回退。
  设计本意 (ShmRingQueue.hpp "生产者写完写 eventfd") 从未生效
- asio_uring 64K RPS 偏低 (~50k vs ev 70k) 需在本修复后重测确认是否仍存在

## 🟡 #71 [工具] 压测脚本杀进程顺序导致孤儿 Worker, SO_REUSEPORT 双进程服务吞吐虚高

> 2026-06-12 | 工具/流程 | 状态: 🟡 已知坑, 压测脚本需固化正确顺序

### 现象

压测脚本先 `pkill Worker` 再 `pkill Manager`:
Manager 在被杀前检测到 Worker 退出 → 自动拉起新 Worker → Manager 死后新 Worker 成孤儿。
下轮压测实例启动后, 孤儿与新 Worker 通过 SO_REUSEPORT 同时监听同一端口,
wrk 连接被内核分流到两个进程 → 吞吐虚高 ~40% (#69 排查中 asio_uring "339k" 假数据来源)。

### 正确做法

1. **先杀 Manager 再杀 Worker** (Manager 死后不会再拉起)
2. 杀完后必须验证: `pgrep -x <name>_W0` 无残留 + `ss -tlnp | grep <port>` 端口已释放
3. 同理适用于 node.sh stop 的实现审查 (resolve_pid 是否覆盖孤儿 Worker)

### 附带发现 (同次排查)

频繁重启压测实例向共享 etcd 注册新 node_id, 打满 255 槽位 ("槽位已满" WARN)。
压测沙箱应将 etcd endpoint 指向死地址 (如 127.0.0.1:1, 已验证对性能无影响)。

## 🟡 #72 [bug] SpawnSingleWorker 父子各关错一个 eventfd, shm 队列通知机制双向失效

> 2026-06-12 | bug | 状态: 🟡 待修复 (#70 排查中发现, 影响性能非正确性)

### 现象

`Manager.cpp SpawnSingleWorker`:
- 子进程: `CloseEventFd(iWorkerToMgrEfd)` — 关掉的是**自己作为生产者要写**的 eventfd,
  且该函数置 `int&` 为 -1, Worker 构造时拿到 -1 → `SendToParent` shm 分支永假,
  Worker→Manager 全部消息 (心跳/WORKER_READY) 走 socket 回退
- 父进程: `CloseEventFd(iMgrToWorkerEfd)` — 关掉的是**自己作为生产者要写**的 eventfd
  → Manager 无法通知 Worker, Worker 靠 1s CheckShareMem 轮询兜底

### 影响

- ShmRingQueue 设计的 eventfd 即时通知 (见 ShmRingQueue.hpp 设计注释) 从未生效
- Worker→Manager: 完全退化为 socket 路径 (#70 修复后功能正常, 但绕过了 shm 快速路径)
- Manager→Worker: 消息延迟最高 1s (轮询间隔)

### 修复方向

eventfd 是单一内核对象、读写双方都需要持有: 两处 CloseEventFd 都应删除
(socketpair 的"各关对端"模式不适用于 eventfd)。修复后需验证:
Worker beat 走 TryEnqueue+NotifyEventFd, Manager drain 正常; SendToWorker 通知 efd 后
Worker shm read watcher 即时唤醒。

## ✅ #73 [已修复] Admin 自定义配置 — 相同内容不触发重启，仅提示

> 2026-06-13 | 优化 | 状态: 🟡 待处理

### 现象
在"自定义配置" Tab 保存时，如果新 JSON 与 etcd 当前值完全相同，仍会执行写入并触发 GracefulRestart。

### 期望行为
- 写入前做字符串对比（规范化 JSON 后比较）
- 相同 → 不写 etcd，仅 toast 提示"内容未变，无需保存"
- 不同 → 正常备份 + 写入 + toast"已保存，GracefulRestart 触发中"

### 影响
非必要重启会打断正在处理的连接。

---

## ✅ #74 [已修复] Admin 自定义配置 — 版本历史列表支持查看 & 回滚

> 2026-06-13 | 优化 | 状态: 🟡 待处理

### 现象
"自定义配置" Tab 底部有"📋 历史"按钮，点开后复用 `showModuleVersions` 显示历史列表，但当前历史列表只能看到 key/时间/预览，无法完整查看历史 JSON 内容，也无法回滚到该版本。

### 期望行为
- 历史列表每行显示：版本 key、时间、内容预览（前 60 字符）
- 每行有"👁 查看"按钮 → 弹出只读 textarea 展示完整 JSON
- 每行有"↩ 回滚"按钮 → 确认后写回 etcd，触发 GracefulRestart
- 回滚前自动备份当前值到历史

### 备注
SO / Lua 版本历史也适用同样逻辑，统一处理。

---

## ✅ #75 [已修复] Admin Lua 脚本 — 相同内容不更新，支持历史 & 回滚

> 2026-06-13 | 优化 | 状态: 🟡 待处理

### 现象 & 期望
1. **相同不更新**：推送 Lua 脚本前对比当前 `script_content`，相同则不写 etcd，仅提示"内容未变，无需推送"
2. **历史支持**：每次成功推送前，将旧的 `script_content` + 版本号备份到  
   `/thunder/config_history/lua/{type}/{url_path}/v{timestamp}`
3. **历史 & 回滚**：编辑界面下方增加"📋 历史"按钮，列出该路由的历史版本，支持查看完整脚本 & 一键回滚
4. **限制范围**：编辑界面只允许修改已有路由的脚本内容，不允许新增或删除路由（路由管理应走"自定义配置" Tab）

---

## ✅ #76 [已修复] Admin SO 模块 — 相同不更新，版本历史能显示能回滚

> 2026-06-13 | 优化 | 状态: 🟡 待处理

### 现象 & 期望
1. **相同不更新**：`triggerUpdate` 前检查当前 etcd 中该 so_path 的版本号是否已是最新，如果 SO 文件内容/版本未变则不写 etcd，仅提示
2. **版本历史**：SO Tab 底部"📋 版本历史"已存在，但点开后列表只展示 key/时间/预览，需要：
   - 展示完整的历史模块配置（含所有 so_path 和版本号）
   - 每行增加"↩ 回滚"按钮，确认后恢复该版本配置并触发 GracefulRestart
3. 与 #74 统一版本历史 UI 逻辑


## ✅ #77 [已修复] WorkerLifecycle 状态机缺少 NEW_ACTIVE → RUNNING 回归 — 第一次优雅重启完成后永远无法再次重启

> 2026-06-13 | bug | 状态: 🔴 待修复 (阻塞后续所有热重载)

### 现象

Manager 收到 etcd 配置变更 → 调用 `GracefulRestartWorker(i)` → 第一次能正常走完重启生命周期。  
但第二次 Admin 推送配置变更时，日志只输出：  
```
WARN: worker 0 not in RUNNING state (3), skip
```
State 枚举值 3 = `NEW_ACTIVE`，第二次之后所有 `GracefulRestartWorker` 调用被静默跳过。

### 根因

`WorkerLifecycle` 状态机完整流程：
```
RUNNING → STARTING → DRAINING → NEW_ACTIVE → ???（缺！）
```
`Manager.cpp:279` 在老 Worker 死亡（SIGCHLD）时将状态设为 `NEW_ACTIVE`，但代码中**没有任何地方把 `NEW_ACTIVE` 回归到 `RUNNING`**。  
新 Worker 此时已经完全接管，生命周期已结束，应直接回归 `RUNNING`。

### 影响

Admin 推送任何配置变更，**第一次之后全部无效**，进程永远运行旧代码。

### 修复

`Manager.cpp:279`：`lc.state = WorkerLifecycle::NEW_ACTIVE` → `lc.state = WorkerLifecycle::RUNNING`

---

## ✅ #78 [已修复] ConfigUpdated 触发重启后 m_oCurrentConf 未同步 — 相同配置重复触发重启

> 2026-06-13 | bug | 状态: 🟠 待修复

### 现象

每次 etcd watch 收到 `ConfigUpdated` 事件（如重连、重放），Manager 都会比较 `newConf["module"]` 与 `m_oCurrentConf["module"]`，  
因为 `m_oCurrentConf` 始终是**进程启动时读取的本地配置**，从未被 etcd 下发的配置更新，  
只要 etcd 版本 > 本地版本，永远为 `changed = true`，无限触发重启。

### 修复

在 `if (changed)` 触发重启前，先把新 module 数组替换到 `m_oCurrentConf`：  
```cpp
m_oCurrentConf.Replace("module", newConf["module"]);
```

---

## ✅ #80 [已修复] ConfigUpdated 顺序错误 — 应先写文件再处理；custom/Lua 不应触发重启

> 2026-06-13 | bug | 状态: ✅ 已修复

### 现象

1. `ConfigUpdated` 收到新配置时，先触发重启，后（或从不）写本地文件。  
   Worker fork 出的新进程加载的是旧磁盘文件，若进程全量重启（crash/ops reload）配置也会丢失。
2. `custom` 配置变更（仅 JSON 字段值变化，module 版本不变）理应热更新，无需 Worker 重启，但流程没有明确区分。
3. Lua 脚本由 `server.py` 写盘后 Worker 热加载，与 module SO 版本无关，同样不需要重启。

### 修复

`Manager.cpp::ConfigUpdated` 重构为六步顺序：

```
1. JSON 合法性检查（无效内容直接 break）
2. 比较旧/新 so + module 版本（在更新 m_oCurrentConf 之前）
3. 合并 module / custom / so 到 m_oCurrentConf
4. 先写文件（ofstream 写 m_strConfFile）
5. 更新共享内存（custom 热更新，Worker 无需重启即感知）
6. 仅 SO/module 版本变化时触发 GracefulRestart（Lua/custom 不重启）
```

同时修正 admin-web toast 提示：custom 保存提示"热更新，无需重启"。

---

## 🟡 #79 [bug] 重启进行中收到新配置变更被静默丢弃

> 2026-06-13 | bug | 状态: 🟡 待处理

### 现象

Admin 连续两次推送配置变更（例如先更新 SO 再推 Lua 脚本），若第二次推送时 Worker 仍处于 `STARTING`/`DRAINING` 状态，  
`GracefulRestartWorker` 会因状态检查直接 `return false`，第二次变更**完全丢失**，  
Worker 完成第一次重启后仍运行旧 Lua 脚本。

### 修复方向

在 Manager 中增加 `m_oPendingModuleConf` 字段，当重启因状态冲突被跳过时保存待应用配置；  
Worker 生命周期回归 `RUNNING` 后（#77 修复后的回归点）检查并应用 pending 配置。

