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

## 🔴 #1 [阻塞] 全量构建失败 — `/usr/local/include/dirent.h` 被 DPDK 的 Windows 版遮蔽

**当前状态: 未处理 (需 root 权限,等确认)**

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

## 🟡 #4 [中] ShmRingQueue 的 `Create/Destroy` 尺寸参数重复硬编码,有 munmap 尺寸不匹配隐患

**当前状态: 未处理** — `code/Net/include/labor/types/ShmRingQueue.hpp:282/300`

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

| # | 位置 | 建议 | 收益 |
|---|------|------|------|
| 5.1 | `AsioUringIoBackend::UpdateRingWatcher` | 每次 Submit 都 `for (auto& [fd,sp] : m_fds)` 全量扫描判断 hasOp,连接多时是热路径 O(N)。可维护一个 `m_pendingOpCount` 计数器,Submit/complete 时 ±1,O(1) 判断。 | 高并发下降低每次 I/O 的常数开销 |
| 5.2 | `AsioUringIoBackend::FindIoUringRingFd` | 遍历 `/proc/self/fd` 取**第一个** io_uring fd;若进程存在多个 io_uring 实例会取错。建议在创建 io_context 后立刻定位(此时只有一个),或显式断言唯一性。 | 健壮性 |
| 5.3 | `Worker.cpp` (6114 行) | 远超项目自定的"单文件 ≤800 行"规范,巨型文件不利于审查/编译增量。建议按职责拆分(连接管理 / Step 调度 / DB 回调 / drain / 信号)。 | 可维护性、编译并行度 |
| 5.4 | `ShmRingQueue` 诊断 | `IsFull/Count/IsEmpty` 里 `(w-r) >= ctrl.slot_count` 对 atomic 成员做隐式 seq_cst load,可显式 `.load(relaxed)`(不可变量)省一次全序屏障。 | 微优化 |

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

## 🟢 #9 [严重→已修复] etcd 节点发现完全失效 — 跨节点 S2S 路由全断 (Center→etcd 迁移回归)

**当前状态: ✅ 已修复并经 E2E 验证(GenKey 拿到 token, E2E `genkey_verifykey_chain` 通过, 全套 19/19 runnable 通过)**

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

### 验证
```
GenKey → {"code":0,"token":"7467947435826872321","key":"...","msg":"success"}
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

## 🟡 #10 [中/环境] Docker 守护进程僵尸容器 — E2E `docker compose up` 失败

**当前状态: 未处理 (需 root 重启 docker 守护进程)**

- `docker ps -a` 列出 9 个 2 周前 `Exited(255)` 的无名容器,`docker rm -f` 报 `No such container`
  却阴魂不散 (`docker info` Containers: 9) → 守护进程容器记录与实际对象脱钩 (经典 daemon 状态损坏)。
- 后果:`docker compose up`(项目名 `thunder-deploy`)按 name 标签撞上僵尸 → `Recreate → No such container` 死循环 → E2E 前两次在 up 阶段失败。
- **绕过**(本次所用):用全新项目名 `docker compose -p thunder_e2e up` 避开名称冲突,成功起栈。
- **根治**:`sudo systemctl restart docker` 清空 daemon 内存态;之后 `deploy.sh test e2e` 可正常用默认项目名。

---

## 🟡 #11 [中/测试基建] E2E 非 hermetic — etcd/日志 bind-mount 跨运行残留

**当前状态: 未处理**

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

## 🔵 #7 [优化/告警] 编译 46 处去重告警 (违反"零告警"规范)

**当前状态: 未处理** — 构建本身 0 error 通过,但 CLAUDE.md 要求 `-Wall -Wextra` 零告警

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
- [ ] **#2/#3 [需设计]** Worker 优雅重启 — **未做, 有前置依赖**:Step 是通用续延回调, 无 step↔fd
      映射;#3 修复需先建该追踪, #2(SIGTERM→drain)在 #3 之前接线会丢在途请求。属独立设计任务,
      需配 CLAUDE.md「优雅重启回归」套件。详见下方 #2/#3 条目。
- [ ] **#7 (其余)** `-Wformat-security`×2 / `-Wmaybe-uninitialized` / `-Wformat=`×6 / `-Woverflow`×3 等
      (非 UB, 可批量清)
- [ ] 内存/并发改动按 CLAUDE.md 跑 ASan + valgrind + TSan 并贴报告
- [ ] 按项目规范:已修 bug(#1/#4/#7/#8/#9/#10/#11/#12)走 GitHub Issue + `fix/` 分支 + PR 闭环
