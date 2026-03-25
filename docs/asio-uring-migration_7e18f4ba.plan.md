---
name: asio-uring-migration
overview: 评估并制定“彻底移除 libev，改用 Boost.Asio+io_uring 作为事件后端”的迁移计划：涵盖网络 IO、定时器/协程 sleep、redis hiredis adapter、以及 mysql 异步驱动。
todos:
  - id: asio-deps
    content: 在 CMake/3party 层引入 Boost（或 standalone Asio），确保 `BOOST_ASIO_HAS_IO_URING` 可用；明确最低 Boost/编译选项与平台约束。
    status: pending
  - id: eventloop-abstraction
    content: 重构 `Labor` 事件抽象：去掉 `ev_loop/ev_io/ev_timer/ev_signal`，改为 `boost::asio::io_context` + timer + signal，并提供线程安全的 `PostToEventLoop`（用 `io_context.post`）。
    status: pending
  - id: core-entities-timers
    content: 替换 `tagConnectionAttr/Step/Session` 中对 `ev_*` 的成员：改为保存 Asio 的 timer/descriptor 相关句柄；实现取消/刷新/销毁时的安全性与幂等检查。
    status: pending
  - id: manager-network-migration
    content: 迁移 `Manager`：把监听 accept、S2S/C2S 的连接建立、`IoRead/IoWrite/IoTimeout` 改为 Asio 的异步读写与 per-connection 状态机（仍复用现有 `util::CBuffer` + pb codec 解析逻辑）。
    status: pending
  - id: worker-network-migration
    content: 迁移 `Worker`：把 Manager IPC（socketpair）与与外部连接 fd 的 `IoRead/IoWrite/IoTimeout` 机制换成 Asio 异步 IO；保持与现有 Step/Session 回调链路一致（同事件线程 resume）。
    status: pending
  - id: coroutine-sleep-migration
    content: 迁移 `StepCo20` 的 `CoSleepAwaiter`（libev ev_timer trampoline）到 Asio `steady_timer`：确保 resume 在同一事件线程、并正确处理协程提前完成/销毁的取消语义。
    status: pending
  - id: redis-adapter-migration
    content: 实现/替换 hiredis-vip 的 redis adapter：新增 Asio 版本的 attach（用 `redisAsyncHandleRead/Write` 驱动），并将 `code/Net/include/storage/RedisClusterLibevAttach.hpp` 改为 Asio attach 路径。
    status: pending
  - id: mysql-async-migration
    content: 迁移 `MysqlAsyncConn`：用 Asio 的 fd readiness（或等价机制）驱动现有非阻塞 MySQL 状态机，去掉 `ev_io` watcher 依赖。
    status: pending
  - id: regression-tests
    content: 完成后做编译+回归：至少跑现有 smoke（如 `deploy/docker/test_interfaceserver_smoke.sh`）与基本联调路径；对比吞吐/延迟与现有 libev 版本。
    status: pending
isProject: false
---

## 背景与结论（先给可行性判断）
`thunder` 当前的事件模型以 `libev` 为中心：
- `code/Net/src/labor/Labor.cpp` 用 `ev_async` 做跨线程 `PostToEventLoop`，并用 `ev_timer/ev_io/ev_signal` 驱动 IO、定时器、信号。
- `code/Net/src/labor/Manager.cpp` / `code/Net/src/labor/Worker.cpp` 以 `ev_loop_new` 创建 loop，`ev_run` 驱动回调。
- 协程恢复强依赖同一事件线程：`code/Net/src/coro/StepCo20.cpp` 的 `CoSleepAwaiter` 通过 `ev_timer` trampoline 在事件线程 `resume()`。
- `hiredis-vip` 的适配器与 `libev` 绑定（`code/Net/include/storage/RedisClusterLibevAttach.hpp` -> `code/3party/hiredis-vip/adapters/libev.h`）。
- `MysqlAsyncConn` 也用 `libev` `ev_io` watcher 驱动非阻塞 MySQL 状态机（`code/Util/src/dbi/MysqlAsyncConn.h/.cpp`）。

因此，“彻底改成 Boost.Asio io_uring（全 Asio）”在架构上是可行的，但改动量非常大：不仅要替换网络 IO，还要重写定时器与协程 sleep、以及 redis/mysql 两个第三方异步适配层。

## 与 bench 的对照点（为什么看得通）
`parallel-lib/io-model/bench_io_asio.cpp` 与 `bench_qps_asio.cpp` 都是典型 Boost.Asio 风格：`io_context.run()` + `tcp::async_accept/async_read_some/async_write`，并通过 `BOOST_ASIO_HAS_IO_URING` 在支持条件下启用 io_uring 后端。
在迁移后，`thunder` 的 Manager/Worker 需要把“当前以 `ev_io` 回调读写 fd”的逻辑，映射为“以 Asio 的异步读写/描述符事件驱动处理同一套 pb codec、缓冲区累积与解析”。

## 迁移后的关键差异（libev vs Asio io_uring）
1. **编程模型**：libev 是显式 watcher（read/write/timer/signal）+ 回调；Asio 是 proactor（async operation）+ completion handlers；网络 IO 的完成回调与“何时继续读/写”需要用连接状态机显式调度。
2. **定时语义**：libev 的 `ev_timer` 既负责 step/session timeout，也负责 `CoSleepAwaiter`；Asio 需要统一改成 `steady_timer`（并实现“刷新/取消/重置”语义）。
3. **生命周期与取消**：libev 代码里存在“pending/回调执行中的资源释放”的 bugfix 认知（见 `docs/bugfix-use-after-free-iocallback.md`）；Asio 同样要处理“取消/销毁 socket/timer 后 handler 何时触发”，需要新的 UAF 防护策略（通常用 shared state + handler 检查）。
4. **第三方适配器**：
   - `hiredis` / `mysql` 目前以“read/write 可读/可写 readiness”方式触发非阻塞状态机；若用 Asio `posix::stream_descriptor::async_wait`，通常仍是 readiness 驱动（不一定完全由 io_uring 提交读写）。
   - 这不会阻碍“全 Asio 替代 libev”，但可能限制部分场景的 io_uring 收益上限。

## 目标态（建议的集成边界）
由于你选择了“彻底移除 libev、全 Asio”，目标态应是：
- Manager/Worker/Loader：统一使用 `boost::asio::io_context`（在支持编译条件下走 io_uring 后端）。
- Step/Session timeout 与协程 sleep：统一使用 Asio timer。
- Redis hiredis adapter：替换 libev adapter，提供 Asio/descriptor 的 attach/read/write/del 行为。
- MysqlAsyncConn：替换 libev ev_io 驱动，改为 Asio 对 socket 的 readiness 事件驱动现有 MySQL 非阻塞状态机。

## 大致改动量（按模块粗估）
- `Labor` 抽象层与事件类型迁移：
  - `code/Net/include/labor/Labor.hpp`
  - `code/Net/src/labor/Labor.cpp`
- `tagConnectionAttr/Step/Session` 的 ev_* 指针字段替换为 Asio 句柄：
  - `code/Net/include/labor/Attribution.hpp`
  - `code/Net/src/labor/Attribution.hpp`
  - `code/Net/include/step/Step.hpp`
  - `code/Net/include/session/Session.hpp`
- `Manager/Worker/Loader` loop 创建与网络/定时调度：
  - `code/Net/src/labor/Manager.cpp`
  - `code/Net/src/labor/Worker.cpp`
  - `code/Net/src/labor/Loader.cpp`
- 协程 sleep 与 StepCo20：
  - `code/Net/src/coro/StepCo20.cpp`
  - `code/Net/include/coro/StepCo20.hpp`
- Redis：
  - `code/Net/include/storage/RedisClusterLibevAttach.hpp`
  - `code/3party/hiredis-vip/adapters/libev.h` 相关逻辑需要替换/新增 Asio adapter
- MySQL：
  - `code/Util/src/dbi/MysqlAsyncConn.h/.cpp`

由于这些模块强耦合于 `libev` 的类型系统（ev_loop/ev_io/ev_timer/ev_tstamp），整体迁移通常是“几千行到上万行”级别，且要投入足够的回归测试。

## 优缺点总结
优点：
- 在 io_uring 可用的环境下，有机会降低系统调用/事件分发开销，改善高并发连接的吞吐与尾延迟。
- 事件后端统一（全 Asio），减少 libev watcher 生态与跨库适配成本。

缺点/风险：
- 大重构：`Labor/Step/Session` 的事件句柄与超时调度全要换。
- 生命周期取消语义：Asio handler 触发时机与 libev 的 pending/clear_pending 不同，UAF/重复回调风险需要新的防护方案。
- redis/mysql 的收益可能不“纯 io_uring”：很可能是 readiness 驱动而非 io_uring 的直接 read/write 提交。
- Boost.Asio/io_uring 支持对 Boost 版本、编译参数、内核特性敏感；同时还需要把 Boost 以 `code/3party`（你选择了 b）落地。

## 计划落地方式（建议的实施分阶段，但在本轮文档先给总规划）
先做“可编译”的后端替换壳层（io_context/timer/post），再迁移 Manager/Worker 的网络读写，最后迁移 Step/Session 定时器与协程 sleep，最后迁移 redis/mysql。

## 迁移完成后的验收建议（不在本轮执行）
- 编译通过：`Net/Hello/Center` 等关键目标都能构建。
- 运行 smoke：`deploy/docker/test_interfaceserver_smoke.sh`（以及现有联调脚本）。
- 基准对照：对比 libev 版本在同等连接数/消息大小下的吞吐/延迟（可参考你已有 io-model/bench 思路）。

## 目标态架构示意
```mermaid
graph TD
  A[Manager/Worker 进程] --> B[Boost.Asio io_context(io_uring后端可选)]
  B --> C[网络 IO: async_accept/async_read_some/async_write]
  B --> D[定时器: steady_timer (Step/Session/周期任务)]
  B --> E[跨线程调度: io_context.post]
  C --> F[沿用现有 pb codec/缓冲区解析]
  F --> G[Step/Session 回调 -> m_coroHandle.resume()]
  D --> G
  E --> G
  G --> H[业务 Step 状态机继续]
```

