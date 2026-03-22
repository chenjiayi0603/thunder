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
| 上下文 | `coro/Awaitable.hpp` 中 `CoroutineContext::state` 类型改为 `StepCo20*` |
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

Interface 插件中 **`TestStepHttpRequestCo` / GenKey / VerifyKey** 等已改为 **`net::StepCo20Func` + lambda**（见 `ModuleInterface.cpp`）；Hello 仍保留独立类 **`StepHttpRequestCo`**。联调脚本见 `deploy/tests/test_interface_http_co20.sh`。

## StepState 已删除

**`StepState` 已从代码库移除**（2025–2026 重构）。`MysqlStep` 直接继承 `Step`，超时/错误字段在类内维护；`net::Register(MysqlStep*, …)` 仅用于注册未执行的 MySQL 异步步骤。协程 HTTP 仍用 `StepCo20` + `Launch` / `SetTimeoutParams`。

## 相关文件索引

- 协程 Task / AsyncTask：`code/Net/include/coro/Coroutine20.hpp`
- 协程 Step 基类：`code/Net/include/coro/StepCo20.hpp`、`code/Net/src/step/StepCo20.cpp`
- 通用 Awaitable：`code/Net/include/coro/Awaitable.hpp`
- Redis 协程封装：`code/Net/include/coro/RedisAwaitable.hpp`、`code/Net/src/step/RedisAwaitable.cpp`

---

## `HttpGetAsync` 返回 `Task<bool>`：字符流程图（图中标注 `Task` / `promise`）

[`Coroutine20.hpp`](code/Net/include/coro/Coroutine20.hpp) · [`StepCo20.cpp`](code/Net/src/step/StepCo20.cpp)

```
  父协程  StepCo20::CoroutineMain()  (Task<void>)
           |
           |  co_await HttpGetAsync(url)
           v
  +------------------------+
  | Task<bool>::co_await   |  task_awaiter::await_suspend(父句柄 h)
  | continuation_ <- 父 h   |  return 子 coro_  ->  runtime resume(子)
  +------------------------+
           |
           v
  +------------------------+
  | 子协程 HttpGetAsync    |  get_return_object / initial_suspend 后进体
  +------------------------+
           |
           v
      HttpGet(url) / SentTo(...)
           |
     +-----+-----+
     |           |
     v           v
  返回 false   返回 true（已入队）
     |           |
     |           v
     |      co_await HttpRespAwaiter
     |           |  await_suspend: m_coroHandle = 当前子帧
     |           v
     |      [Worker 发送 / 收包 … 异步]
     |           |
     |           v
     |      Callback(...) -> m_coroHandle.resume()
     |           |
     |           v
     |      await_resume -> 得到 bool，继续子协程体
     +-----+-----+
           |
           v
  co_return <bool>  ->  return_value -> final_suspend
           |
           v
  final_awaiter: return continuation_(父)  ->  resume(父)
           |
           v
  父协程在 co_await 点:  task_awaiter::await_resume -> result() -> bool
```

说明（与上图对应）：`Task` 父子衔接用 `promise.continuation_`；等待 HTTP 响应用 Step 侧 `m_coroHandle`，与 `continuation_` 无关。帧生命周期见 `~Task` / `coro_.destroy()`，细节仍以源码为准。
 

### 与「LazyCoroutine + 手动 resume」的对比（选型备忘）

| 方式 | 适用场景 |
|------|----------|
| **LazyCoroutine**：`initial_suspend` + 外部 `handle.resume()` | 教学、生成器、**与事件循环无关**的步调控制。 |
| **HttpRespAwaiter + Worker::Callback** | 响应由 **libev / Worker** 在任意时刻到达，必须把 **`resume` 接到既有回调链**；自定义 awaiter 是合理做法。 |

因此在本框架里**不能**只靠 `std::suspend_always` 或 Lazy 风格替代 **`HttpRespAwaiter`**，除非把整个 HTTP 完成通知改到同一套驱动模型里。

---

## C++20 协程：从零认识四个角色

> 本节面向初学者，用图和比喻说清「协程/promise/Task/awaiter」各是什么、谁调用谁。

---

### 概念速览：四个角色是什么

```
  你写的源码                    编译器/运行时看到的
  ──────────                    ────────────────────
  Task<bool> HttpGetAsync() {   ← 协程函数，编译器把它变成一台「状态机」
      ...
      co_return result;
  }

  ┌─────────────────────────────────────────────────┐
  │  堆上的「协程帧」（编译器生成）                    │
  │  ┌─────────────┐  ┌──────────────────────────┐  │
  │  │  promise    │  │  局部变量 / 挂起点编号     │  │
  │  │  (生命周期  │  │  state = 0,1,2…           │  │
  │  │   钩子集合) │  └──────────────────────────┘  │
  │  └─────────────┘                                │
  └─────────────────────────────────────────────────┘
           ▲                          ▲
           │                          │
    promise 负责                  句柄（handle）指向这里
    存结果/异常                        │
    决定挂不挂起                   Task 只是一个
                               「持有句柄的壳子」，
                               给调用方拿着用
```

用生活比喻来说：

- **协程帧** = 快递仓库里的一个格子，存着这单货（局部变量/进度）
- **promise** = 格子里的快递单，记录「装了什么、有没有出错、谁在等」
- **Task** = 快递单号，调用方拿着它可以查进度，也可以销毁（析构时清空格子）
- **awaiter** = 一张「等待牌」，放在等待队列里，告诉运行时：「我在等这件事，好了叫我」

---

### 一条协程从生到死

```
调用方写：  Task<bool> t = HttpGetAsync(url);
               │
               │ 1. 编译器先在堆上分配「协程帧」（含 promise）
               │ 2. 调 promise.get_return_object()  →  Task 对象返回给调用方
               │ 3. 调 promise.initial_suspend()
               │       suspend_always → 先挂起，等人来 resume
               │       suspend_never  → 直接进协程体（AsyncTask 用这个）
               ▼
         ┌──────────────┐
         │  状态：挂起   │  ← 调用方拿着 Task，协程暂停在入口
         └──────────────┘
               │
               │ 某人（父协程）co_await t  →  resume(子帧)
               ▼
         ┌──────────────┐
         │  状态：运行   │  ← 协程体在跑
         └──────────────┘
               │
          遇到 co_await 别的东西
               │
               ▼
         ┌──────────────┐
         │  状态：挂起   │  ← 保存进度，把执行权交出去
         └──────────────┘
               │
          外部事件（网络回调）resume 回来
               ▼
         ┌──────────────┐
         │  状态：运行   │  ← 从挂起点继续
         └──────────────┘
               │
          co_return result
               │
               ▼
         promise.return_value(result)   ← 结果存进 promise
               │
         promise.final_suspend()        ← 最后一次挂起
               │
          通知父协程「我跑完了」（对称转移）
               │
          Task 析构  →  coro_.destroy()  ← 释放帧
```

---

### `co_await` 怎么工作：三步展开

遇到 `co_await X`，编译器生成的逻辑（人类可读版）：

```
Step 1  取 awaiter
        ──────────
        若 X 有 operator co_await()
            awaiter = X.operator co_await()    ← Task 走这里，返回 task_awaiter
        否则
            awaiter = X                         ← HttpRespAwaiter 本身就是 awaiter

Step 2  问「已经好了吗？」
        ────────────────
        if (awaiter.await_ready() == true)
            跳到 Step 3，不挂起              ← 子协程已结束时走这条
        else
            保存当前协程进度到帧
            awaiter.await_suspend(我的句柄)   ← 把我的句柄交给 awaiter
            ↓
            await_suspend 返回「下一个要跑的句柄」
            运行时切换过去（对称转移，不叠栈）

Step 3  被 resume 后取结果
        ──────────────────
        result = awaiter.await_resume()       ← co_await 表达式的值就是这个
```

关键点：**`await_suspend` 接收的是「当前正在 co_await 的我自己的句柄」**（父帧），不是子帧。子帧放在 awaiter 的成员里（`coro_`）。

---

### 父协程 co_await 子协程：数据流动全图

以 `CoroutineMain co_await HttpGetAsync` 为例：

```
  ┌──────────────────────────────────────────────────────────────────┐
  │  父协程帧（CoroutineMain）                                        │
  │  promise.continuation_ = noop（初始）                            │
  └──────────────────────────────────────────────────────────────────┘
          │
          │ co_await child_task（child_task 是 Task<bool>）
          │
          │  ① child_task.operator co_await()
          │       返回 task_awaiter{ 子帧句柄 }
          │
          │  ② task_awaiter.await_ready() → false（子还没跑完）
          │
          │  ③ task_awaiter.await_suspend(父帧句柄 h)
          │       子帧.promise.continuation_ = h   ← 记住父帧，回来用
          │       return 子帧句柄                  ← 对称转移：切到子帧
          │
          ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  子协程帧（HttpGetAsync）                   状态：运行            │
  │                                                                  │
  │  HttpGet(url) ──── false → co_return false                       │
  │                │                                                 │
  │                └── true → co_await HttpRespAwaiter               │
  │                               │                                  │
  │                               │ await_suspend(子帧句柄)           │
  │                               │   Step.m_coroHandle = 子帧句柄   │
  │                               │   子帧挂起，控制权回 Worker       │
  │                               ▼                                  │
  │                          [等待 IO…]                              │
  │                               │                                  │
  │                         Callback() → m_coroHandle.resume()       │
  │                               │                                  │
  │                         await_resume() 得到 bool                 │
  │  co_return result                                                 │
  │      → promise.return_value(result)  ← 存结果                   │
  │      → promise.final_suspend()                                   │
  │           final_awaiter.await_suspend(子帧句柄)                  │
  │               return continuation_  ← 就是之前存的父帧句柄       │
  └──────────────────────────────────────────────────────────────────┘
          │
          │ 对称转移回父帧，父协程继续执行
          ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  父协程帧（CoroutineMain）继续                                    │
  │  ④ task_awaiter.await_resume()                                   │
  │       return 子帧.promise.result()  ← 取出 bool，重抛异常（若有）│
  │  父协程拿到 bool，继续往下跑                                      │
  └──────────────────────────────────────────────────────────────────┘
```

---

### `continuation_` 和 `m_coroHandle` 的区别

这两个都是「存句柄，用来 resume」，但用途完全不同：

```
  continuation_（存在子 promise 里）
  ────────────────────────────────
  用途：Task 父子链的「回路」
  谁写：父协程 co_await 子 Task 时，task_awaiter.await_suspend 写入
  谁读：子协程 final_suspend 时，通过它跳回父协程
  时机：子跑完之后自动触发，整个过程在同一个调用链里

  m_coroHandle（存在 StepCo20 成员里）
  ─────────────────────────────────────
  用途：等待外部 IO 事件的「挂钩」
  谁写：HttpRespAwaiter.await_suspend 写入（存的是「当时正等 IO 的那条 Task 帧」）
  谁读：Worker 事件循环收到 HTTP 响应后，Callback() 里 resume
  时机：异步，可能过很久，由 libev/Worker 触发

  两者独立，互不干扰：
  Callback resume(m_coroHandle) 后，子帧继续 → co_return → final_suspend
  → 通过 continuation_ 对称转移回父帧，一路传到 AsyncTask final_suspend。
```

---

### Task 和 AsyncTask 的区别（一句话）

```
  Task<T>      先挂起等人 co_await，才进协程体；可以被 co_await，用于子任务
  AsyncTask    创建即运行（不挂起），不能被 co_await，用于最外层「启动器」
               帧挂在 final_suspend 直到 Step 析构，防止提前 destroy
```

---

### 各个名字是干啥的（一眼版）

```
  promise_type     藏在本条协程堆帧里，管挂起、存结果、异常、收尾还给谁；不是 awaiter；父子各一份、不混用
  Task / Task<bool>  手里「小票」，一个句柄指向本条协程帧；别人可 co_await；析构则销毁帧
  task_awaiter     父 co_await 子 Task 时编译器临时造：挂起父、记下父、去跑子，完事在父侧取结果
  final_awaiter    子自己跑完、final_suspend 里用：按 continuation_ 把执行权还给父；和 task_awaiter 不是一回事
  HttpRespAwaiter  等业务回调时用：挂起当前协程，句柄写进 m_coroHandle，Callback 里 resume；与 continuation_ 无关
  AsyncTask        最外层启动：创建即进体，不能被 co_await；帧多活一阵，防早析构（见上文 StepCo20）

  为啥像有很多「隐藏调用」：co_await / co_return 背后编译器按标准自动调 promise、awaiter；不必背顺序。
  记三句：一条协程一个 promise；一次等待一个 awaiter；Task 只管自己那一帧的生死。
```

---

### awaiter 三个接口的调用时机（一次 `co_await`）

```
  对「co_await expr」求值时，先把 expr 变成 awaitable，再取 awaiter，然后按顺序：

  await_ready()
    当前协程已执行到 co_await 这一行，同步调用。
    true  → 不挂起：跳过 await_suspend，接着在同一次求值里调 await_resume()，其返回值即 co_await 的结果。
    false → 需要挂起：接着调 await_suspend。

  await_suspend( std::coroutine_handle<> h )
    仅在 await_ready 为 false 时调用。
    h 是「马上要因本次 co_await 而挂起」的这条协程帧（常用来登记父/续体，或交给调度器去跑别的）。
    从这里起本协程挂起，直到别处对 h（或对称转移返回的句柄）resume。

  await_resume()
    本协程被 resume 之后，在 co_await 的恢复点调用。
    返回值作为整个 co_await 表达式的结果；若有异常，按标准从 await_resume 传出。

  final_awaiter（协程收尾）
    用在 promise 的 final_suspend 里，不是普通「等 IO」awaiter。
    await_suspend 收到的 h 是「即将结束的这条子协程」；常返回 continuation_，把执行权对称转移回父协程。
```
