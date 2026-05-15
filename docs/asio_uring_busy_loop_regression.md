# AsioUringIoBackend ring_fd 忙循环回归问题

> 状态：**根因已确认（源码核对）** — 修复方案待评审
> commit: 3e6939a (fix: Worker 侧 ENOTCONN 修复引入后触发)
> 修订：四/五节经 ASIO + libev 源码逐行核对重写，旧 4 假设已证伪（见 4.3）

---

## 一、问题现象

问题描述：
AsioUringIoBackend导致的 interface  到 logic 转发失败的问题

### 1.1 触发条件

提交 3e6939a（Worker 侧 ENOTCONN 修复）部署后，Interface 节点 `Interface_robot_W0` 进程 CPU 升至 31%（此前正常为 <5%）。`/tmp/asio_uring_diag.log` 迅速增长至 19GB，内容为 `OnRingReady` 回调的重复刷屏。

### 1.2 日志特征

```
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
[07:17:03] [IODIAG AsioUring OnRingReady ring_fd=13 poll=0
...（每秒数万行）
```

关键特征：
- `ring_fd=13` — 同一次启动中 ring_fd 不变
- `poll=0` — `io_context::poll()` 返回 0，即**没有完成事件可处理**
- 刷屏前约 13 分钟正常（07:04 启动，07:17 开始刷屏），此期间成功的读操作日志可见

### 1.3 功能影响

| 测试 | 结果 | 说明 |
|------|------|------|
| `test_interface_http_co20_echo` | ✅ PASS | 纯 HTTP echo，无 S2S 转发 |
| GenKey 链式测试 (4 个) | ❌ FAIL | HTTP 连接无响应 |
| 直接 curl Interface | ❌ 超时 | 服务不响应 |

---

## 二、忙循环机制 — 死循环调用链

```
libev ev_run 主循环
  └─ epoll_wait()                            ← 立即返回（ring_fd 可读）
       └─ ev_io callback: OnRingReady         ← ring_fd=13 EV_READ 触发
            ├─ m_ioCtx.poll()                 ← ASIO poll（无超时）
            │    └─ io_uring_service::run(0, ops)
            │         ├─ io_uring_peek_cqe(&ring_, &cqe)
            │         │    └─ 返回 -EAGAIN   ← CQ 为空！
            │         ├─ 跳过 CQE 处理循环 (result != 0)
            │         └─ return 0             ← count = 0
            │
            └─ diag_log()                     ← 写入 + fflush 磁盘
                 └─ OnRingReady poll=0
  └─ 下一轮 ev_run → epoll_wait 立即返回 ...
```

**核心矛盾**：`epoll_wait` 认为 ring_fd 可读，但 `io_uring_peek_cqe` 返回 `-EAGAIN`（无 CQE）。libev 使用**电平触发**（level-triggered）监控 ring_fd，只要 `ring->cq.head != ring->cq.tail`（内核视角的 CQ 环形缓冲区状态），epoll 就会持续返回可读。

---

## 三、相关代码路径

### 3.1 AsioUringIoBackend 初始化三路驱动

```cpp
// AsioUringIoBackend::Init()
ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ);  // ring_fd ev_io
ev_prepare_init(&m_prepare, &OnPrepare);                       // epoll_wait 前
ev_check_init(&m_check, &OnCheck);                             // epoll_wait 后
```

### 3.2 三路回调

```cpp
// OnPrepare — epoll_wait 前拉取
void AsioUringIoBackend::OnPrepare(ev_prepare* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();  // 有完成事件才打日志 (n>0)
}

// OnCheck — epoll_wait 后拉取
void AsioUringIoBackend::OnCheck(ev_check* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
}

// OnRingReady — ring_fd 可读触发  ← 这是刷屏的来源
void AsioUringIoBackend::OnRingReady(ev_io* w, int) {
    auto* be = static_cast<AsioUringIoBackend*>(w->data);
    auto n = be->m_ioCtx.poll();
    diag_log("[IODIAG AsioUring OnRingReady ring_fd=%d poll=%zu\n",
             be->m_ringFd, n);  // ← 无条件日志！
}
```

### 3.3 ASIO io_uring_service::run() — CQE 收割核心

```cpp
void io_uring_service::run(long usec, op_queue<operation>& ops)
{
    // 1. 非阻塞窥探 CQE
    ::io_uring_cqe* cqe = 0;
    int result = (usec == 0)
        ? ::io_uring_peek_cqe(&ring_, &cqe)    // -EAGAIN if empty
        : ::io_uring_wait_cqe(&ring_, &cqe);

    // 2. 循环收割
    int count = 0;
    while (result == 0 || local_ops > 0)
    {
        if (result == 0)
        {
            io_queue* io_q = static_cast<io_queue*>(::io_uring_cqe_get_data(cqe));
            io_q->set_result(cqe->res);
            ops.push(io_q);
        }
        ::io_uring_cqe_seen(&ring_, cqe);  // 标记 CQE 已消费
        ++count;
        result = (count < complete_batch_size)
            ? ::io_uring_peek_cqe(&ring_, &cqe) : -EAGAIN;
    }

    decrement(outstanding_work_, count);
}
```

**关键行为**：
- `io_uring_peek_cqe` 返回 `-EAGAIN` 表示 CQ 为空
- `io_uring_cqe_seen` 逐个标记 CQE 已消费（更新 `ring->cq.khead`）
- 循环结束后**不会显式调用** `io_uring_cq_advance`

### 3.4 ASIO eventfd 注册 —— 本构建下**不存在**（勿被 ASIO 文档误导）

> ⚠️ 上游 ASIO 在 “io_uring 作为 epoll 辅助” 模式下会创建 eventfd 并
> `io_uring_register_eventfd`。但 Thunder 编译宏使该路径 **整段编译掉**，
> 运行期没有 eventfd。详见 [4.1 事实 1](#41-逐条源码证据) 与 [4.3 旧 4 假设证伪](#43-旧-4-假设逐条证伪)。
> 此处仅保留说明以澄清：早期对 eventfd 的怀疑方向已被源码排除。

`io_uring_service.ipp` 中 `init_ring()` 的 eventfd 创建/注册（`:541-560`）、
`event_fd_read_op` 类（`:563-609`）、`register_with_reactor()` 函数体
（`:611-617`）全部位于 `#if !defined(ASIO_HAS_IO_URING_AS_DEFAULT)` 内；
而 `CMakeLists.txt:38` 的 `ASIO_DISABLE_EPOLL + ASIO_HAS_IO_URING` 经
`config.hpp:951-955` 推导出 `ASIO_HAS_IO_URING_AS_DEFAULT` 已定义 →
上述 eventfd 代码在本构建中不参与编译。真正的唤醒源是 `interrupt()` 的
NOP CQE（见第四节）。

---

## 四、根因（已验证 — ASIO + libev 源码逐行核对）

> 本节的每条结论都附 `文件:行号` 源码证据，可逐一复核。原“核心假设”
> 4 条已被源码证伪，见 4.3。

### 4.0 结论先行

真因是三者叠加，被 commit 3e6939a 放大：

1. ASIO 的 `interrupt()` 用 `io_uring_prep_nop`（`data==this`）作自唤醒，
   **每个异步读/写提交都会产生一个立即完成的 NOP CQE**；
2. Thunder 用 **电平触发** 的 libev `ev_io` 监听原始 `ring_fd`，
   CQ 里只要有 NOP CQE 就立刻把 `epoll_wait` 唤醒；
3. `OnRingReady` 每次唤醒都 **无条件 `diag_log()` + `fflush()`**。

ENOTCONN 修复前 Worker 出向 S2S 连接同步失败、asio_uring 上几乎没有存活
异步 op，该闭环不被触发；修复后 Interface↔Logic 持久连接成功，持续 S2S
读不断重新喂闭环 → 永不收敛的纯空转 + 磁盘 fflush 风暴。

### 4.1 逐条源码证据

**事实 1 — 根本不存在 eventfd（编译期已排除）**

- `CMakeLists.txt:38`：`add_compile_definitions(... ASIO_HAS_IO_URING ASIO_DISABLE_EPOLL)`
- `code/3party/asio/include/asio/detail/config.hpp:951-955`：
  `ASIO_DISABLE_EPOLL` ⇒ `ASIO_HAS_EPOLL` 未定义；
  `!ASIO_HAS_EPOLL && ASIO_HAS_IO_URING` ⇒ `#define ASIO_HAS_IO_URING_AS_DEFAULT 1`
- `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp`：
  `init_ring()` 里 eventfd 创建/`io_uring_register_eventfd` 在
  `#if !defined(ASIO_HAS_IO_URING_AS_DEFAULT)`（`:541-560`）内；
  `event_fd_read_op` 类（`:563-609`）与 `register_with_reactor()` 函数体
  （`:611-617`）同样整段被 `#if` 包住 → **全部编译掉**。

⇒ Thunder 这套 ASIO 构建里 **没有 eventfd**，旧假设 1/2/3 的前提不存在。

**事实 2 — `interrupt()` 用 NOP SQE 自唤醒**

`io_uring_service.ipp:519-528`：

```cpp
void io_uring_service::interrupt() {
  mutex::scoped_lock lock(mutex_);
  if (::io_uring_sqe* sqe = get_sqe()) {
    ::io_uring_prep_nop(sqe);
    ::io_uring_sqe_set_data(sqe, this);   // data == this
  }
  submit_sqes();
}
```

NOP 由内核近乎立即完成 → 投递一个 `data==this` 的 CQE。

**事实 3 — poll 模式下每个异步 op 都触发一次 `interrupt()`**

- `scheduler.ipp:669-681` `wake_one_thread_and_unlock()`：
  `wait_usec_ == 0`（即 `io_context::poll()` 路径）时短路进入
  `if (!task_interrupted_ && task_) { task_interrupted_=true; task_->interrupt(); }`。
- `io_uring_service.ipp:283-308` `start_op()`：op 入队后走
  `post_submit_sqes_op()`（`:730-742`）→ `scheduler_.post_immediate_completion(&submit_sqes_op_)`
  → 内部 `wake_one_thread_and_unlock()` → 上面那条 `interrupt()`。
- Thunder 的 `SubmitRead`/`SubmitWrite` 每次都 `async_read_some`/`async_write_some`
  → 一次 `start_op` → 一个 NOP。

**事实 4 — 电平触发监听原始 ring_fd**

`code/Net/src/labor/AsioUringIoBackend.cpp` `Init()`：
`ev_io_init(&m_ringWatcher, &OnRingReady, m_ringFd, EV_READ)`。
libev 的 Linux epoll backend 默认电平触发：CQ 非空（含 NOP CQE）
`epoll_wait` 立即返回。

**事实 5 — `OnRingReady` 无条件 `diag_log()` + `fflush()`**

`AsioUringIoBackend.cpp:266-271`：每次唤醒都 `diag_log(...poll=%zu)`，
`diag_log`（`:22-39`）内 `fputs` 后 `fflush`。这是 31% CPU 与 19GB 日志的
直接放大器。

**事实 6 — `SubmitRead`/`SubmitWrite` 末尾再入 `m_ioCtx.poll()`**

`AsioUringIoBackend.cpp:195`（SubmitRead）/`:231`（SubmitWrite）各有
`m_ioCtx.poll();  // 立即提交 SQE 给内核`。使每次读/写提交都同步走一遍
scheduler → interrupt → NOP，进一步抬高 NOP 频率。

### 4.2 忙循环闭环

```
S2S 读完成 → Thunder 读回调 → SubmitRead 再投递
  → io_uring_service::start_op → post_submit_sqes_op
     → post_immediate_completion → wake_one_thread_and_unlock
        → (wait_usec_==0 且 !task_interrupted_) → interrupt()
           → io_uring_prep_nop(data==this) + submit
  → NOP 立即完成 → CQE 入队 → ring_fd 变可读（电平触发）
  → libev epoll_wait 立即返回 → OnRingReady
     → m_ioCtx.poll() → run(0) 收割：ptr==this（NOP/中断）→ 0 个用户 op
     → diag_log("...poll=0") + fflush                ← 19GB / 31% CPU
  → SubmitRead 末尾再入 poll() 再产生 NOP …            ← 自持
```

`poll=0` 的语义：本次 `poll()` 唯一消费的是内部 NOP/中断 CQE
（`io_uring_service::run()` 里 `ptr == this` 分支，仅 `io_uring_cqe_seen`
后丢弃），不产生任何用户完成回调，故 `io_context::poll()` 返回 0。

### 4.3 旧 4 假设逐条证伪

| 旧假设 | 证伪依据 |
|--------|---------|
| 1. `io_uring_cqe_seen` 不回写共享 head | 与现象无关：`cqe_seen`→`cq_advance` 一直 store-release 写共享 `*khead`（这是 mmap CQ 环的设计）。真正让 ring_fd 持续可读的是**不断新生的 NOP CQE**，非 head 不推进 |
| 2. eventfd + ring_fd 双唤醒 | **不存在 eventfd**（事实 1：`ASIO_HAS_IO_URING_AS_DEFAULT` 把整段编译掉） |
| 3. `event_fd_read_op` repeat-op 自激 | 同上，`event_fd_read_op` 整类被 `#if` 编译掉，运行期不存在 |
| 4. “修复前后差异”方向对、机理错 | 方向正确（修复后才暴露），但机理不是“隐藏的 ring_fd 管理问题”，而是事实 2–6 的 NOP/电平触发/日志闭环 |

### 4.4 为何 commit 3e6939a 后才爆

修复前：Worker 出向 S2S 连接在未连接 fd 上 `SubmitRead` 同步拿到 ENOTCONN
→ `DestroyConnect`，asio_uring 上几乎无长期存活异步 op，事实 3 的
post→interrupt→NOP 路径极少被走。修复后：Interface↔Logic 连接建立成功，
持久 `async_read_some` 长期挂起，持续 S2S 流量不断重新喂 4.2 闭环 →
永不收敛 → 纯空转 + fflush 风暴。

---

## 五、修复方案（待评审后实施 — 仅改 `AsioUringIoBackend.cpp`）

### 5.A ring_fd 监听改为按需驱动（断环关键）

仅当存在 ≥1 个真实挂起异步 op 时 `ev_io_start(m_ringWatcher)`；无挂起 op
时 `ev_io_stop`。在 `SubmitRead`/`SubmitWrite` 入口确保已 start；在
`OnCheck` 末尾若 `m_fds` 全部 idle 则 stop。

理由：无真实挂起 op 时纯 NOP/中断 CQE 对 Thunder 无意义，正是空转根源；
`ev_prepare`/`ev_check` 每轮仍 `poll()`，SQE 提交与 CQE 收割最多延后一轮
ev 迭代（亚毫秒），不丢完成事件。

### 5.B `diag_log` 全部受环境变量开关（默认关）

`THUNDER_ASIO_URING_DIAG=1` 才输出，否则 `diag_log` 首行直接 return。
消除 19GB 日志与每次唤醒的 `fflush`（CPU 主要去向）。无论 5.A 是否生效，
生产都绝不能无条件写该日志。

### 5.C 删除 `SubmitRead`/`SubmitWrite` 内再入 `m_ioCtx.poll()`

`AsioUringIoBackend.cpp:195` / `:231`。SQE 由紧接着的 `OnPrepare`
（`ev_prepare`，在 `epoll_wait` 前）统一 flush；去掉再入 poll 让 SQE 批量
提交、消除“每次读/写一个 NOP”的放大。延迟影响 ≤ 一轮 ev 迭代。

> 5.A + 5.C 从源头断开反馈，5.B 兜底封住放大面。

### 5.D 确认型埋点（属于 5.B，env 门控）

`io_uring_service::run()` 的调用处包一层薄计数，每秒输出一次：
NOP/中断（`ptr==this`）、timeout、真实 op、`poll()==0` 空唤醒 各自计数。
修复前后对比，空唤醒率应坍塌。

### 5.E 兜底（非主方案）

若修复后高负载仍异常：将 `deploy/Interface/conf/Interface.json`、
`deploy/Logic/conf/Logic.json` 的 `io_backend` 由 `asio_uring` 改 `ev`
解锁 E2E，再迭代 asio_uring。

### 5.F 修复后验证步骤

1. `cmake --build build -j1 && cmake --install build`
2. 重建并起 docker，等健康检查。
3. `MODE=external python3 -m pytest tests/e2e/test_interface_chain.py -v --mode=external -s`
   → 期望 5/5。
4. `MODE=external ./tests/run_all.sh e2e` → 目标 28/29。
5. `Interface_robot_W0` 稳态 CPU < 5%；默认无 `/tmp/asio_uring_diag.log`。
6. 回归护栏：某节点设 `io_backend:"ev"`，chain 测试仍通过。
7. 按 CLAUDE.md「测试后必须清理」收尾。

---

## 六、相关代码文件索引

| 文件:行号 | 说明（根因证据） |
|-----------|------------------|
| `CMakeLists.txt:38` | `ASIO_DISABLE_EPOLL` + `ASIO_HAS_IO_URING`（事实 1 起点） |
| `code/3party/asio/include/asio/detail/config.hpp:951-955` | 推导出 `ASIO_HAS_IO_URING_AS_DEFAULT`（事实 1） |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:541-560` | eventfd 创建/注册，被 `#if` 编译掉（事实 1） |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:563-617` | `event_fd_read_op` + `register_with_reactor()`，整段编译掉（事实 1） |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:519-528` | `interrupt()` → `io_uring_prep_nop(data==this)`（事实 2） |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:283-308` | `start_op()` 每个异步 op 都 post（事实 3） |
| `code/3party/asio/include/asio/detail/impl/io_uring_service.ipp:730-742` | `post_submit_sqes_op()` → `post_immediate_completion`（事实 3） |
| `code/3party/asio/include/asio/detail/impl/scheduler.ipp:669-681` | `wake_one_thread_and_unlock` → `interrupt()`（事实 3） |
| `code/Net/src/labor/AsioUringIoBackend.cpp:266-271` | `OnRingReady` 无条件 `diag_log`（事实 5） |
| `code/Net/src/labor/AsioUringIoBackend.cpp:22-39` | `diag_log` 内 `fflush`（事实 5；5.B 改动点） |
| `code/Net/src/labor/AsioUringIoBackend.cpp:195,231` | `SubmitRead`/`SubmitWrite` 再入 `poll()`（事实 6；5.C 改动点） |
| `code/Net/src/labor/Worker.cpp` | ENOTCONN 修复（5 处编辑，commit 3e6939a，触发本回归） |
| `docs/io_uring_concurrency_model.md` | 并发模型完整分析（三路驱动、ENOTCONN 等） |
