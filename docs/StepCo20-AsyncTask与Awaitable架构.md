# StepCo20：AsyncTask、Task 与 Awaitable 架构说明

汇总 C++20 协程与 HttpStep / Worker 调度相关类型、职责，以及 coroutine_handle 由谁保存、由谁 resume。偏重类关系与句柄归属，操作步骤见其它 StepCo20 文档。

---

## 1. 分层

| 层次 | 作用 | 典型符号 |
|------|------|-----------|
| 最外层协程帧 | Emit 返回 RUNNING 后根帧不能因临时对象析构而销毁 | AsyncTask、m_oAsyncBootstrap |
| 父子 Task | 体内 co_await Task 子协程，结束回到外层 | continuation_、task_awaiter |
| 本 Step 回调 | HTTP / 内部 PB 回到本 HttpStep::Callback，恢复当前挂起点 | HttpRespAwaiter、m_coroHandle、Callback |
| 独立 I/O | MySQL、Redis 等另一条 RegisterCallback 与子 Step | MySqlAwaitable + MySqlStepBridge 等 |
| 线程池回环 | 池内算完 PostToEventLoop 再 resume | PoolOffloadAwaiter |

要点：AsyncTask 不是每次 co_await 都存 handle，只长期持有最外层 StepAsync 那一帧。单次等待由 m_coroHandle、桥接 Step、或 Task 的 continuation_ 等分担。

---

## 2. 主要类（路径相对 code/Net）

协程骨架（include/coro/Coroutine20.hpp）

- Task / Task void：可 co_await；initial_suspend 恒挂起；父帧在 continuation_；子结束时 final_suspend 对称回父；帧寿命由 Task 对象移动与析构管理。
- AsyncTask：不可 co_await；initial_suspend 不挂起；final_suspend 恒挂起；return_void 经 async_task_promise_notify_if 调 NotifyEmitCoroutineSuccess；帧由 AsyncTask 析构时 destroy；协程首参须为 StepCo20&。

步骤（include/coro/StepCo20.hpp、src/coro/StepCo20.cpp）

- StepCo20：继承 HttpStep；StepAsync 返回 AsyncTask；Emit 里 m_oAsyncBootstrap.emplace(StepAsync())。
- m_oAsyncBootstrap：optional AsyncTask，持外层帧，避免 Emit 返回后根帧被提前销毁（见 bugfix-use-after-free-iocallback.md）。
- m_coroHandle：当前等待 Http/PB 回调或 CoSleep、线程池卸载等时，要恢复的那条帧（多为 Task 子帧）。
- NotifyEmitCoroutineSuccess：正常结束时置位；不在此 ExecStep 重入 Emit。
- HttpRespAwaiter：await_suspend 写 pStep->m_coroHandle；Callback 填响应后 resume。
- CoSleepAwaiter：定时器回调里 resume m_coroHandle。

封装（include/coro/StepCo20Func.hpp）

- StepCo20Func：StepAsync 为 return m_fn(*this)，用户返回的 AsyncTask 直接被 emplace。
- LaunchCo：声明在此，实现在 Interface.cpp。

桥接 Awaitable（概要）

- MySqlAwaitable + MySqlStepBridge：见下文专节。
- RedisAwaitable + RedisStepBridge：与 MySQL 同思路，独立 Redis Step 路径。（coro/RedisAwaitable.hpp）
- PoolOffloadAwaiter：await_suspend 写 step_->m_coroHandle；PostToEventLoop 里 resume，并校验 IsRegisteredStep。（ThreadPoolAwaitable.hpp）

---

## 2.1 MySQL：MysqlStep、MySqlAwaitable、桥

继承与角色

- Step：通用步骤基类（seq、注册、Worker 调度）。
- MysqlStep：继承 Step；持 util::tagDbConnInfo 派生的连接参数、m_strCmd / m_uiCmdType、MysqlResSet；异步 SQL 完成后进 Callback(MysqlAsyncConn*, SqlTask*, MYSQL_RES*)，基类里再 Init 结果集并调 Emit()。
- 传统用法：MysqlStep::Launch 内 net::Register（定时）+ GetLabor()->RegisterCallback(pStep)（把任务交给 MySQL 异步连接侧）；或已注册后 AppendTask + RegisterCallback。
- CustomMysqlHandler：挂在连接上的 util::MysqlHandler；on_execsql / on_query 里调 Worker::Dispose(c, task, pResultSet)，把结果路由回对应 MysqlStep 的 Callback。（MysqlStep.cpp）

协程侧

- MySqlAwaitable：在 StepCo20 协程里 co_await；持 StepCo20*、dbConn、SQL、命令类型、超时参数；await_resume 返回 MySqlReply（errNo、errMsg、result 深拷贝）。头文件 coro/MySqlAwaitable.hpp，实现 coro/MySqlAwaitable.cpp。
- MySqlStepBridge：仅在该 cpp 内定义的 MysqlStep 子类；构造时绑定 MySqlAwaitable* 与 coroutine_handle；Emit 重写为恒返回 STATUS_CMD_RUNNING（避免走基类「空完成」路径）；Callback 把错误或结果集写入 m_pAwaitable->m_reply 后 resume m_handle，返回 STATUS_CMD_COMPLETED；Timeout 超次后填超时错误并 resume，返回 STATUS_CMD_FAULT。
- await_suspend 流程：new MySqlStepBridge，Init(uiTimeOutMax, uiTimeOutRetry)，SetTask( sql, cmdType )；先 RegisterCallback(pBridge, dTimeout)（Step* 侧超时），再 RegisterCallback(static_cast<MysqlStep*>(pBridge))（MySQL 任务队列）；任一失败则 DeleteCallback 或 delete 桥并当场 resume 填错因。

与 StepCo20::m_coroHandle 的界限

- 协程 await MySQL 时不写 m_coroHandle；恢复只经桥内 m_handle。HTTP/PB 仍走 HttpRespAwaiter 与 StepCo20::Callback。

Util 层（code/Util）

- MysqlAsyncConn、SqlTask、MysqlHandler：实际连接与任务排队；Worker 在 Dispose 中与 MysqlStep::Callback 衔接。头文件示例 dbi/MysqlAsyncConn.h。

MySQL 协程路径（字符图）

```
StepCo20 协程体内 co_await MySqlAwaitable
              |
              v
       await_suspend(h)
              |
              v
    new MySqlStepBridge(awaitable, h, dbConn)
              |
              +-- RegisterCallback(桥, dTimeout)   // Step 定时
              +-- RegisterCallback(MysqlStep*)      // 投递 SQL
              |
              v
    MysqlAsyncConn / Worker ... 执行完成
              |
              v
    MySqlStepBridge::Callback(c, task, res)
         填写 MySqlReply
         m_handle.resume()
              |
              v
    await_resume()  return MySqlReply
```

---

## 3. 关系（字符流程图）

```
StepCo20::Emit
       |
       | emplace(StepAsync())
       v
+---------------------------+
| m_oAsyncBootstrap         |
| AsyncTask = 根帧 coro_    |
+-----------+---------------+
            |
            v
    StepAsync 协程体
            |
    +-------+--------+------------------+
    |                |                  |
    v                v                  v
co_await Task   co_await          co_await
(如 HttpGetAsync) HttpRespAwaiter  MySqlAwaitable
    |                |                  |
    |    continuation_ 链回外层          |
    |                |                  v
    +------+         |           MySqlStepBridge
           |         |           RegisterCallback
           v         |                  |
      Task 子帧      |                  v
           |         |             桥 Callback
           |         |             resume(桥内 m_handle)
           v         v
      await_suspend 写 StepCo20::m_coroHandle
           |
           v
    StepCo20::Callback (Http / PB)
           |
           v
    m_coroHandle.resume()
```

---

## 4. 句柄与 resume（速查）

| 场景 | handle 所在 | 谁 resume |
|------|-------------|-----------|
| 外层 StepAsync 帧 | AsyncTask（m_oAsyncBootstrap） | 内层 Task 链回到根；根停在 final_suspend；Step 清理时 ~AsyncTask destroy |
| co_await Task | 子帧在 Task；父在子 promise.continuation_ | 子 final_suspend 对称转移 |
| co_await HttpRespAwaiter | m_coroHandle | StepCo20::Callback |
| co_await CoSleepAwaiter | m_coroHandle | CoSleepTimerTrampoline |
| co_await PoolOffloadAwaiter | m_coroHandle（挂起期间） | PostToEventLoop 闭包内 h.resume |
| co_await MySqlAwaitable | MySqlStepBridge::m_handle | 桥 Callback / Timeout |

---

## 5. 误区

1. AsyncTask 不能等价替换 MySqlAwaitable + Bridge：前者持根帧，后者管单次 MySQL 路径；常见写法是根用 AsyncTask，体内再 co_await MySqlAwaitable。

2. 只要外层体内有 co_await 且 Emit 会先返回，就需要 m_oAsyncBootstrap 一类宿主；不限于「多次」co_await。

3. AsyncTask 使用 final_suspend 恒挂起 + 外部 RAII 持有；勿与 suspend_never 加外部 destroy 混用，避免双释放（见 bugfix-use-after-free-iocallback.md）。

---

## 6. 相关文档与源码

| 文档 | 内容 |
|------|------|
| StepCo20-coroutine-migration.md | 迁移、StepCo20Func、EmitSuccessGuard |
| StepCo20-threadpool-integration-design.md | 线程池与事件循环 |
| StepCo20-threadpool-examples.md | 示例 |
| 协程栈模型对比.md | 有栈 / 无栈 |
| bugfix-use-after-free-iocallback.md | AsyncTask 与 final_suspend |

源码：Coroutine20.hpp；StepCo20.hpp / StepCo20.cpp；StepCo20Func.hpp；coro/MySqlAwaitable.hpp、coro/MySqlAwaitable.cpp；src/step 或 include/step 下 MysqlStep.hpp、MysqlStep.cpp；ThreadPoolAwaitable.hpp；Util dbi/MysqlAsyncConn.h（及 MysqlAsyncConn.cpp）。

文档随 dev 分支实现更新；类型改名请以源码为准。
