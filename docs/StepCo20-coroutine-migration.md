# StepCo20 协程基类迁移说明

本文记录 **保留 `StepCo20`、移除 `CoroutineState`** 的决策与落地变更，便于后续维护与 Code Review。

## 结论

统一使用 **`net::StepCo20`**（继承 `HttpStep`）作为 C++20 协程型 Step 的基类；**`CoroutineState` 与 `StepState` 均已删除**，协程基类不再与旧状态机叠层。

## 为何选 StepCo20 而非 CoroutineState

1. **外层 `AsyncTask` 生命周期**  
   旧 `CoroutineState::Emit` 曾以临时对象启动协程，易导致挂起后帧被析构、回调 `resume` 时崩溃（如 curl 52）。`StepCo20` 使用成员 **`std::optional<AsyncTask> m_oAsyncBootstrap`** 延长外层任务生命周期。

2. **等待模型**  
   `StepCo20` 通过 **`HttpRespAwaiter`** 直接挂起等待一次 HTTP/二进制回调；旧实现经 `WaitForAsync()` 多一层协程间接。

3. **继承链**  
   `CoroutineState` 继承 `StepState`，会把状态机相关成员带进协程类；`StepCo20` 直接继承 `HttpStep`，职责更清晰。

4. **能力**  
   `StepCo20` 提供内部协议异步发送（`SendToInternalAsync` / `ByIdentify` / `ByNodeType`）、超时重试、`ResponseToClient`、`GetLastRspMsgHead/Body/HttpMsg` 等。

## 已完成的代码变更（摘要）

| 类别 | 说明 |
|------|------|
| 删除 | `code/Net/include/step/CoroutineState.hpp`、`code/Net/src/step/CoroutineState.cpp` |
| Redis | `RedisAwaitable` / `RedisCoHelper` 由 `CoroutineState*` 改为 `StepCo20*`；`StepCo20` 对 `RedisAwaitable` 声明 `friend`；实现见 `code/Net/src/step/RedisAwaitable.cpp` |
| 上下文 | `Awaitable.hpp` 中 `CoroutineContext::state` 类型改为 `StepCo20*` |
| Hello 示例 | `HttpRequestCo` 继承 `StepCo20`，入口为 `CoroutineMain()`，返回 `net::Task<>` |
| Interface 注释 | `Interface.hpp`：`Register`/`Init` 针对 `MysqlStep`；协程 Step 多用 `Launch`，超时用 `SetTimeoutParams` |

`code/Net/CMakeLists.txt` 对 `step/*.cpp` 使用 GLOB，删除 `CoroutineState.cpp` 后无需再显式维护该文件名。

## StepCo20 后续改进（可靠性）

- **`HttpRespAwaiter`**：`co_await HttpGetAsync` 等返回的 `bool` 现根据 **`GetLastRspHttpMsg().status_code()`** 判断：仅当 `type == HTTP_RESPONSE` 且状态码在 **[200, 400)** 时为 `true`；内部二进制回调仍视为成功（响应写在 `m_oResMsgHead/Body`）。
- **`RedisAwaitable`**：通过内部 **`RedisStepBridge`（`RedisStep`）** 调用 **`GetLabor()->AutoRedisCmd`**，命令经 **`AppendRawCmd(m_strCommand)`** 走 Worker 既有 hiredis 路径；回调里将 **`redisReply`** 转为 **`RedisReply`** 并 **`resume`** 协程。`AutoRedisCmd` 失败、空命令、无 `StepCo20` 时会立即填错误 `RedisReply` 并恢复协程。

## 示例类与 JSON 选项（Hello / Interface）

- **`StepHttpRequestCo`**（原 `StepHttpRequestCo20`）：多站点串行 `HttpGetAsync` 演示（Hello）。  
  - JSON **`option`**：`TestStepHttpRequestCo`（避免与下面一项重名）。
- **`HttpRequestCo`**：另一路协程 HTTP 演示（含更多站点 / 不同 JSON 字段）。  
  - JSON **`option`**：`TestHttpRequestCo`。

Interface 插件中同样使用 **`StepHttpRequestCo`** + **`TestStepHttpRequestCo`**；联调脚本见 `deploy/tests/test_interface_http_co20.sh`。

## StepState 已删除

**`StepState` 已从代码库移除**（2025–2026 重构）。`MysqlStep` 直接继承 `Step`，超时/错误字段在类内维护；`net::Register(MysqlStep*, …)` 仅用于注册未执行的 MySQL 异步步骤。协程 HTTP 仍用 `StepCo20` + `Launch` / `SetTimeoutParams`。

## 相关文件索引

- 协程 Task / AsyncTask：`code/Net/include/step/Coroutine20.hpp`
- 协程 Step 基类：`code/Net/include/step/StepCo20.hpp`、`code/Net/src/step/StepCo20.cpp`
- 通用 Awaitable：`code/Net/include/step/Awaitable.hpp`
- Redis 协程封装：`code/Net/include/step/RedisAwaitable.hpp`、`code/Net/src/step/RedisAwaitable.cpp`
