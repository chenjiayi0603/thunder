# StepCo20 与线程池协作设计

本文是**设计说明**：描述在本仓库事件驱动模型下，**C++20 协程（`StepCo20`）与线程池**如何安全协作、何时使用、何时不要用。  
**已实现**：`Labor::PostToEventLoop`、`PoolOffloadAwaiter` / `RunOnThreadPool`（[ThreadPoolAwaitable.hpp](../code/Net/include/coro/ThreadPoolAwaitable.hpp)）、全局池 `ThunderWorkerThreadPool`（[WorkerThreadPool.hpp](../code/Net/include/labor/WorkerThreadPool.hpp)）；设计中的 **`ScheduleBackToWorker`** 伪名与 **`PostToEventLoop`** 对应。

相关背景见 [StepCo20-coroutine-migration.md](StepCo20-coroutine-migration.md)。

---

## 1. 背景与问题定义

Thunder Net 侧请求处理以 **单 Worker / 事件循环（libev）** 为主：`StepCo20` 在 **`Emit` → `StepAsync`（`AsyncTask`）** 中运行，通过 **`HttpRespAwaiter`**、`co_await HttpGetAsync` / `SendToInternalAsync` 等把「等待 IO」挂起，在 **`Callback` 中 `m_coroHandle.resume()`** 恢复。

**问题**：若协程体内执行 **长时间 CPU 计算** 或 **阻塞式同步 API**（阻塞 IO、带锁休眠的第三方库），会 **长时间占用事件线程**，拖慢同进程其它连接与定时器。

**目标**：在**不破坏**「Step / Labor / 连接上下文仅在事件线程使用」的前提下，把 **可隔离的重活** 挪到 **线程池**，算完或阻塞结束后 **再回到事件线程** 继续协程。

**定位**：**协程 ≠ 多线程**。协程是**异步编排语法**；并行与卸载由**显式线程池 + 显式回到事件循环**完成。

---

## 2. 当前模型约束（设计前提）

以下内容以现有代码为准，协作方案**必须**兼容这些约束。

| 组件 | 路径 | 约束摘要 |
|------|------|----------|
| `StepCo20` | [code/Net/include/coro/StepCo20.hpp](../code/Net/include/coro/StepCo20.hpp)、[code/Net/src/coro/StepCo20.cpp](../code/Net/src/coro/StepCo20.cpp) | 挂起点将当前帧句柄写入 **`m_coroHandle`**；**`Callback` / 定时器 trampoline** 在同 Worker 事件路径上 **`resume()`**。 |
| `HttpRespAwaiter` | 同上 | 与 **`Task::continuation_`** 独立；等待的是 **Worker 回调链**，不是通用调度器。 |
| `NotifyEmitCoroutineSuccess` | `StepCo20.cpp` | 成功收尾时 **不得**在协程栈上 `ExecStep` 重入 `Emit`（注释已说明，否则可能 **destroy 正在运行的帧**）。 |
| `CoSleepAwaiter` | `StepCo20.cpp` | 通过 **`GetLabor()->AddEvent`** 注册 **ev_timer**，仍在事件线程侧触发 `resume`。 |
| `StepCo20Func` / `LaunchCo` | [code/Net/include/coro/StepCo20Func.hpp](../code/Net/include/coro/StepCo20Func.hpp) | 用户协程体与上述 **`m_coroHandle` 恢复模型**一致。 |
| 线程池 | [code/Util/src/thread/threadpool.h](../code/Util/src/thread/threadpool.h) | **`std::threadpool`**：`commit(f, args...)` → **`std::future`**；工作线程执行 `f`，与 **libev 循环线程分离**。 |

**推论**：

1. **`m_coroHandle.resume()` 应在持有该 Step 的 Worker 事件线程上执行**（与现有 `Callback`、`CoSleepTimerTrampoline` 一致）。从线程池 **直接** `resume()` 通常 **不安全**（除非整个 Step 模型改为线程安全，本仓库未采用）。
2. 线程池任务内 **禁止** 调用 `GetLabor()`、`SendTo*`、操作 `StepCo20*` 成员、改 `MsgHead`/`MsgBody` 等与请求上下文绑定的 API，除非后续有明确的「仅事件线程」队列封装。

---

## 3. 推荐协作模式：受控 offload

推荐公式：

```text
事件线程（协程）: 解析请求、发起异步 IO、编排步骤
        |
        |  仅把「纯函数式」重活提交线程池（入参拷贝，出参通过 future / 回调带回）
        v
线程池: CPU 密集 / 阻塞调用（不碰 Step/Labor/连接）
        |
        |  结果就绪后，通过「投递回 Worker」在事件线程 resume 协程（设计目标；需自研 awaiter 或等价机制）
        v
事件线程: 继续协程，写响应、NotifyEmitCoroutineSuccess 等
```

### 3.1 理想 API 形态（设计层，非现成代码）

在 `StepCo20` 协程体内：

```cpp
// 伪代码：未来可实现为 ThreadPoolAwaiter(step, pool, []{ ... })
auto result = co_await RunOnThreadPool(step, pool, [&] {
    return HeavyCompute(input_copy);
});
// 恢复点保证在事件线程，可安全使用 step / GetLabor()
```

实现要点（实现阶段参考，本文不绑定具体类名）：

- **`await_suspend`**：保存当前协程句柄到 **Step 侧槽位**（与 `HttpRespAwaiter` 类似，注意与嵌套 `co_await Task` 的 **`m_coroHandle` 覆盖**问题，见下文「风险」）。
- **线程池**：`commit([input_copy, h, loop](){ ...; schedule_resume_on_worker(); })`。
- **回到事件线程**：使用与现有 **`CoSleepAwaiter`** 相同思路——**`GetLabor()->AddEvent` / `ev_async` / 已有「主线程投递」** 能力（以 Worker 实际提供的为准）投递一个 **一次性回调**，在回调里 **`resume`**。

### 3.2 落地 API 对照（Worker）

| 设计伪名 / 概念 | 代码位置 |
|------------------|----------|
| `ScheduleBackToWorker` | **`Labor::PostToEventLoop(fn)`**：`ev_async` + 互斥队列；`InitPostToEventLoop` 在 Worker/Manager/Loader 的 `CreateEvents` 末尾调用，`StopPostToEventLoop` 在 `ev_loop_destroy` 之前调用。 |
| `g_threadpool` | **`net::ThunderWorkerThreadPool()`**；**`InitThunderWorkerThreadPool`** 在 Worker `Init` 中按 **`custom.worker_thread_pool_size`**（默认 4，上限 `THREADPOOL_MAX_NUM`）调用。 |
| `PoolOffloadAwaiter` | **[ThreadPoolAwaitable.hpp](../code/Net/include/coro/ThreadPoolAwaitable.hpp)** 中模板 **`PoolOffloadAwaiter` / `MakePoolOffloadAwaiter`**；池尾 **`PostToEventLoop`** 内 **`Worker::IsRegisteredStep`** 校验后再 **`resume`**。 |
| `RunOnThreadPool`（返回值型） | 同头文件 **`RunOnThreadPool(step, pool, f)`**（`f` 无参、有返回值，非 `void`）。 |
| Hello 演示 | HTTP JSON **`option`**: **`TestHelloPoolCpu`**（池内累加校验大缓冲区）、**`TestHelloPoolBlock`**（池内 `sleep_for` 模拟阻塞 SDK）。 |

---

## 4. 使用方式清单（可执行）

1. **默认**：协程内只使用 **`HttpGetAsync` / `SendToInternalAsync` / `RedisAwaitable` / `CoSleepAwaiter`** 等已有异步原语，**不**接线程池。
2. **需要线程池时**：
   - 把任务写成 **无状态或仅捕获值拷贝** 的 lambda / 函数：`[input = std::move(input)]() -> Output { ... }`。
   - 在线程池内 **不** 访问 `StepCo20&`、`this`（业务 Step 成员）、`GetLabor()`、fd、map 迭代器。
   - 结果用 **`std::future`、消息队列、或「投递到 Worker」** 带回；**仅在事件线程** 读取结果并 `resume`。
3. **超时**：线程池 `commit` 返回的 `future` **不会自动**与 `StepCo20::Timeout` 联动；需在 Step 层维护「任务已提交未回调」状态，超时则 **丢弃结果**（用 `std::shared_ptr<std::atomic<bool>> cancelled` 等）并结束协程，避免晚到回调 **`resume` 已销毁的 Step**。
4. **异常**：线程池内异常应 **在池内捕获** 并转为 `expected`/`optional`/错误码传到事件线程；避免让异常跨越线程边界未定义行为。
5. **完成**：正常路径仍需在适当时机 **`NotifyEmitCoroutineSuccess()`**（与 [StepCo20-coroutine-migration.md](StepCo20-coroutine-migration.md) 一致）。

### 4.1 举例说明（怎么用）

下面用「大请求体算哈希」说明一条完整链路；「从池里回到 Worker 再 resume」请用 **`Labor::PostToEventLoop`** 或 **`co_await MakePoolOffloadAwaiter`**（见 §3.2）。

例一：事件线程发起，池里只算，回到事件线程再回包

1. 在 StepCo20 协程里（事件线程）从请求里拷出一份 body 字符串或字节向量，不要传 Step 指针进池子。
2. 准备一个放结果的容器，例如 shared_ptr 包一层结构体，里面有算完的摘要和错误码，初始清空。
3. 调线程池 commit，lambda 只按值捕获 body 副本和结果容器的 shared_ptr。lambda 里只对 body 做哈希或解压等 CPU/阻塞工作，把结果写进容器；不要 GetLabor、不要 SendTo、不要碰 Step。
4. commit 会返回 future。不要在事件线程上对 future 阻塞 get。应在池里任务末尾用「投递到当前 Worker」的办法（思路同 CoSleepAwaiter 用 GetLabor AddEvent 挂一次性回调），让回调跑在事件线程上。
5. 一次性回调里（事件线程）：读出摘要，ResponseToClient 或填 MsgBody，再 resume 之前挂起的协程句柄（若用 awaiter，就是 awaiter 里存的那条）。
6. 协程继续跑，最后 NotifyEmitCoroutineSuccess。

伪代码（例一）前先约定两个名字，避免读着悬空：

- work：不是关键字，而是 PoolOffloadAwaiter 成员里保存的「池内要跑的那一段」，即 Body 里 co_await PoolOffloadAwaiter 的最后一个参数（那个 lambda，只做 CPU/阻塞、写 out）。await_suspend 里从 work_ 移到局部变量 work，再被 commit 的 lambda 捕获；池线程里 work(body, out) 就是调用这段用户逻辑。
- ScheduleBackToWorker：仓库里没有这个函数名，是伪名，表示「把传入的 lambda 投递到跑 libev 的 Worker 线程上执行一次」。实现可类比 StepCo20.cpp 里 CoSleepAwaiter 用 GetLabor AddEvent 挂一次性回调；也可用 ev_async，只要回调在事件线程跑、且不在池线程里直接 resume。

```cpp
// --- 协程体（事件线程）：只负责准备数据 + co_await ---
AsyncTask Body(StepCo20& step) {
    std::string body = CopyRequestBody(step);
    auto out = std::make_shared<HashResult>();

    co_await PoolOffloadAwaiter(&step, g_threadpool, body, out,
        [](std::string const& b, std::shared_ptr<HashResult> o) {
            o->digest = Sha256(b);
            o->err = 0;
        });

    step.NotifyEmitCoroutineSuccess();
    co_return;
}

// --- PoolOffloadAwaiter::await_suspend(h) 里展开写清三件事分别在哪个线程 ---
// h = 当前协程句柄（编译器传入 await_suspend）
void PoolOffloadAwaiter::await_suspend(std::coroutine_handle<> h) {
    auto weakStep = /* step 的 weak 或取消令牌，防 Step 已销毁 */;
    std::string body = std::move(body_);
    auto out = out_;
    // work：即 Body 传入的 lambda（Sha256 等），仅在线程池里执行
    auto work = std::move(work_);

    pool_.commit([body, out, work]() {
        // —— 线程池线程 —— 只算、只写 out，不 Response、不 resume
        work(body, out); // 等价于用户写的 o->digest = Sha256(b) 等
        // ScheduleBackToWorker：伪 API，见上文说明；内部 = 往 Worker 投一次性回调
        ScheduleBackToWorker([weakStep, out, h]() {
            // —— 事件线程（Worker）—— 这里才是「读 out、回包、再 resume」
            if (auto p = weakStep.lock()) {
                if (out->err == 0)
                    p->ResponseToClient(200, out->digest); // 读 out
                else
                    p->ResponseToClient(500, "fail");
            }
            if (h && !h.done())
                h.resume(); // 协程从 co_await 下一行继续
        });
    });
}
// await_resume()：在 h.resume() 之后、协程继续执行时被调用，Body 里才走到 NotifyEmitCoroutineSuccess
```

PoolOffloadAwaiter 与 g_threadpool 内部（伪代码，模板参数仅为示意，落地时可改成固定 Body 类型或 std::function）

```cpp
// g_threadpool：进程内一个线程池实例，类型见 Util/src/threadpool.h（命名空间为 std，此处写作 threadpool）
threadpool g_threadpool(4); // 线程数按配置

// 可作为 awaiter 直挂 co_await；WorkFn 签名与 Body 里传入的 lambda 一致
template <class BodyT, class OutT, class WorkFn>
struct PoolOffloadAwaiter {
    StepCo20* step_;
    threadpool& pool_;
    BodyT body_;
    std::shared_ptr<OutT> out_;
    WorkFn work_;

    PoolOffloadAwaiter(StepCo20* s, threadpool& p, BodyT b,
                        std::shared_ptr<OutT> o, WorkFn w)
        : step_(s), pool_(p), body_(std::move(b)), out_(std::move(o)), work_(std::move(w)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        // 伪：weakStep 防 Step 已析构；也可改成取消令牌、代数 id 查表
        auto weakStep = MakeWeakStep(step_);

        BodyT body = std::move(body_);
        std::shared_ptr<OutT> out = out_;
        WorkFn work = std::move(work_);

        // 内部使用线程池：pool_ 通常即 g_threadpool，仅在此调 commit
        // commit 实现见 Util/src/thread/threadpool.h：队列 + 互斥锁 + 条件变量（非无锁队列）
        std::future<void> fut = pool_.commit([body = std::move(body), out, work, weakStep, h]() mutable {
            work(std::move(body), out); // 池线程：由工作线程从 _tasks 取出后执行

            ScheduleBackToWorker([weakStep, out, h]() {
                if (auto p = weakStep.lock()) {
                    if (out->err == 0)
                        p->ResponseToClient(200, out->digest);
                    else
                        p->ResponseToClient(500, "fail");
                }
                if (h && !h.done())
                    h.resume();
            });
        });

        (void)fut; // 事件线程不要 get 阻塞；靠投递回调里 resume
    }

    void await_resume() noexcept {
        // 协程已从 h.resume() 恢复后调用；此处一般无需再做事
    }
};

// 若编译器要求，可写：auto operator co_await() { return *this; } 或包一层 Task
```

要点：g_threadpool 只在 await_suspend 里通过 pool_.commit 使用；commit 返回的 future 不在事件线程上 get；pool_ 引用可来自全局 g_threadpool 或成员注入，语义相同。

与本仓库 threadpool::commit 相关的实现（有锁队列 + 条件变量，不是无锁队列）：入队时用 lock_guard 锁住 mutex，把封装好的任务 emplace 进 std::queue，再 condition_variable::notify_one 唤醒一个工作线程；工作线程里 unique_lock 配合 _task_cv.wait，在队列非空或池已停止时醒来，pop 队首后解锁再执行 task。详见 code/Util/src/thread/threadpool.h 成员 _tasks、_lock、_task_cv。

例二：以后若有 RunOnThreadPool 式 awaiter

先拷好入参，再 co_await RunOnThreadPool（池，lambda 只捕获入参并在池内返回计算结果）。恢复点必须在事件线程，再 ResponseToClient 和 Notify。实现上仍是池内算完、投递回 Worker、再 resume，不能在池线程里 resume。

伪代码（例二，外观；内部实现仍须例一的池 + 投递）：

```cpp
// 伪代码
AsyncTask Body(StepCo20& step) {
    std::string body = CopyRequestBody(step);
    HashResult r = co_await RunOnThreadPool(g_threadpool, [body]() {
        return Sha256(body); // 仅在池线程执行
    });
    // 下面已在事件线程
    step.ResponseToClient(200, r);
    step.NotifyEmitCoroutineSuccess();
    co_return;
}
```

反例

在 commit 的 lambda 里捕获 this、改 Step 成员、GetLabor SendTo、或对协程句柄 resume，容易跨线程或悬空，不要这样写。

伪代码（反例）：

```cpp
// 错误示范：不要这样写
g_threadpool.commit([this]() {           // 捕获 this，池里可能晚于 Step 析构
    GetLabor()->SendTo(...);            // Labor 非线程安全
    this->m_coroHandle.resume();        // 在池线程 resume，破坏事件线程模型
});
```

### 4.2 重 CPU 或同步外调：协程 + 线程池怎么用、要不要用、好处

与下面「例 A / 例 B」同内容的伪代码另有一份，放在 Hello 插件 ModuleHello 目录旁，便于和协程示例对照：[code/Hello/src/ModuleHello/StepCo20-threadpool-examples.md](../code/Hello/src/ModuleHello/StepCo20-threadpool-examples.md)。

下面两个例子都沿用 §4.1：事件线程里的协程只拷参数、co_await 等池子干完并投回 Worker；真正费时的逻辑只在线程池里跑，再回到事件线程回包和 Notify。

例 A：很吃 CPU 的纯函数

例如对大缓冲区做压缩、复杂校验、大规模运算，单次可能几十毫秒以上。

- 用法：StepAsync 里把输入拷成 string 或 vector，co_await PoolOffloadAwaiter（或等价封装）；池内 lambda 只调用 HeavyCpuFn(拷贝)，结果写进 out；投回事件线程后 ResponseToClient，再 NotifyEmitCoroutineSuccess。
- 避免在协程里直接在事件线程上跑 HeavyCpuFn，否则这段时间 reactor 一直被占。

伪代码（例 A：重 CPU，与 §4.1 同一 awaiter 形态）：

```cpp
// 假设 HeavyCompress 是同步、纯 CPU、可能很慢
AsyncTask BodyCpu(StepCo20& step) {
    std::vector<uint8_t> raw = CopyRequestBytes(step);
    auto out = std::make_shared<CpuJobResult>();

    co_await PoolOffloadAwaiter(&step, g_threadpool, std::move(raw), out,
        [](std::vector<uint8_t> buf, std::shared_ptr<CpuJobResult> o) {
            o->bytes = HeavyCompress(buf); // 仅池线程执行
            o->err = 0;
        });

    step.NotifyEmitCoroutineSuccess();
    co_return;
}

// 反例：不要这样，事件线程会被占死
AsyncTask BodyCpuBad(StepCo20& step) {
    std::vector<uint8_t> raw = CopyRequestBytes(step);
    auto packed = HeavyCompress(raw); // 错：在事件线程跑重 CPU
    step.ResponseToClient(200, packed);
    step.NotifyEmitCoroutineSuccess();
    co_return;
}
```

例 B：只能同步调用的外界 API

例如某 SDK 只有阻塞式 HTTP、阻塞 ODBC，没有异步接口。

- 用法：协程里拷好 url、请求体；commit 的 lambda 里只调 BlockingApi(拷贝)，结果写 out；ScheduleBackToWorker 里 ResponseToClient 与 resume。占满的是池里某一个工作线程，跑 libev 的线程仍可处理其它连接。

伪代码（例 B：同步外调，池内只调阻塞 SDK，回包仍在 ScheduleBackToWorker 里，见 §4.1 await_suspend 展开）：

```cpp
// 假设 ThirdPartyBlockingFetch 会阻塞等网络，无异步接口
AsyncTask BodySyncApi(StepCo20& step) {
    std::string url = CopyUrl(step);
    std::string postBody = CopyPostBody(step);
    auto out = std::make_shared<SyncApiResult>();

    co_await PoolOffloadAwaiter(&step, g_threadpool, url, postBody, out,
        [](std::string u, std::string body, std::shared_ptr<SyncApiResult> o) {
            o->response = ThirdPartyBlockingFetch(u, body); // 仅池线程阻塞
            o->err = 0;
        });

    step.NotifyEmitCoroutineSuccess();
    co_return;
}

// PoolOffloadAwaiter 内部仍是：池线程里 work(url, postBody, out)；
// 末尾 ScheduleBackToWorker 里读 out、ResponseToClient、resume(h)
```

有没有必要这么用

- 有必要：该调用在事件线程上会明显拉长占用（常见量级数毫秒级且 QPS 不低），已观测到别的请求变慢、定时/心跳不准。
- 不太有必要：调用极快；或已有异步 API（HttpGetAsync、SendToInternalAsync 等），应优先异步而不是再包池子。
- 更强隔离：重活也可放独立进程或服务，成本更高。

好处（相对在协程里直接调重同步函数）

1. 事件线程尽快回到 libev，其它连接、同 Worker 上其它 Step、定时器能继续跑，吞吐和尾延迟通常更好。
2. 业务仍可把「等池子」和「等 HTTP」写成顺序协程，多一个 co_await 卸载点即可。
3. 多个 CPU 任务可由线程池占多核（受池大小限制），单靠单线程事件循环做不到。

代价：多调度、拷贝、自研投递与超时/生命周期处理（见 §6）；池满要排队或拒绝。

---

## 5. 场景选择

### 5.1 适用场景（建议使用线程池 offload）

| 场景 | 说明 |
|------|------|
| **CPU 密集计算** | 大 JSON 解析/序列化、复杂校验、密码学计算、大批量 protobuf 处理等，耗时明显大于一次网络 RTT。 |
| **阻塞式第三方库** | 仅提供同步阻塞 API 的 SDK（同步 HTTP、阻塞文件、阻塞 DB 驱动），短期无法改为异步或另起进程。 |
| **可切分的批处理** | 可将输入输出 **完全拷贝** 为值类型，计算过程 **无共享可变状态**。 |

### 5.2 不适用场景（不建议或禁止用线程池「包一层」）

| 场景 | 说明 |
|------|------|
| **已有异步路径** | 已有 `HttpGetAsync`、`SendToInternalAsync`、`RedisAwaitable` 等；再丢线程池只会增加上下文切换与复杂度。 |
| **在池内操作连接/Step** | 在 worker 线程调用 `SendTo`、`ResponseToClient`、改 `m_oResMsgBody` 等 → **数据竞争 / UAF**。 |
| **轻量逻辑** | 几微秒级工作，线程池调度成本反而更高。 |
| **需要严格顺序且高 QPS** | 无界 `commit` 可能导致队列堆积；应配合 **队列深度限制、拒绝策略、监控**。 |

### 5.3 典型正例（2 个）

1. **图片/大文本校验**：请求体读入 `std::string`/`vector` 后，在线程池做哈希或解压，结果 `bool`/`string` 回事件线程写 HTTP 响应。
2. **同步遗留库**：`LegacyBlockingLookup(key)` 仅在线程池调用，`key`/`string` 拷贝传入，返回 `std::optional<Value>` 到事件线程再 `SendToInternal`。

### 5.4 典型反例（2 个）

1. **在线程池里 `co_await HttpGetAsync`**：协程帧在事件线程，**不能**在工作线程续跑同一套 `m_coroHandle` 模型。
2. **在线程池里 `GetLabor()->SendTo`**：`Labor`/连接非线程安全，必出问题。

---

## 6. 风险清单与守则

| 风险 | 说明 | 守则 |
|------|------|------|
| **`m_coroHandle` 覆盖** | 嵌套 `co_await`（先 `HttpRespAwaiter` 再线程池 awaiter）若共用同一槽位，后一次会覆盖前一次句柄。 | 实现 awaiter 时需 **链式保存**或 **禁止嵌套**，或统一用独立队列管理多个挂起点。 |
| **Step 已销毁仍 `resume`** | 线程池晚于 Step 生命周期完成。 | 使用 **`weak_ptr` / 代数 token / cancelled 标志**；池内回调先检查再 `resume`。 |
| **死锁** | 事件线程 `future.get()` 阻塞等待线程池，而池线程又等事件线程。 | 事件线程 **禁止**对 offload 的 `future` **同步 `.get()`**；只用异步回调链。 |
| **线程池耗尽** | 高并发大量 `commit`。 | 限制并发、队列长度、监控 `idlCount()`；必要时拒绝请求。 |

---

## 7. 与 `std::threadpool` 的关系

仓库内线程池实现见 [code/Util/src/thread/threadpool.h](../code/Util/src/thread/threadpool.h)：

- **`commit`** 返回 **`std::future`**，适合在 **非事件线程** 跑任务。
- 与 `StepCo20` 对接时：**不要在事件线程阻塞 `.get()`**；应在池内任务末尾 **schedule 回 Worker** 再 `resume`。

注意：该头文件将类型置于 **`std` 命名空间**（历史封装），与标准库同名空间混用，新代码引用时需谨慎区分。

### 7.1 TBB、OpenMP 与协程能不能对接

能配合，但对接的是「流程」，不是让 OpenMP 每个线程去跑 co_await。

- **协程（StepCo20）**：仍然在事件线程上挂起、恢复；co_await 只和本框架的异步原语、以及你自研的「池 + 投回 Worker」配套。
- **TBB / OpenMP**：是在**某个 OS 线程**里，对一段**同步代码**做并行拆分（parallel_for、task_arena 等）。它和协程没有内置绑定；不要在 parallel 区域里对同一条业务协程做 co_await。

**推荐用法**：在事件线程的协程里，先按前文把重活 **offload 到线程池里的一个任务**；在这个任务函数**内部**再调用 TBB 的 parallel_for / flow，或包一层 **OpenMP parallel for**。输入输出仍是按值或可证明无数据竞争的切片；任务结束后把结果通过投递回 Worker，再在事件线程里 resume、回包。

**不要在事件线程上直接开大并行区**：若在 Step 协程里、仍在 libev 那条线程上调用 OpenMP/TBB 且并行区较重，调用线程往往会等到并行区结束（常见隐式 barrier），等价于长时间占死事件循环。

**资源注意**：线程池工作线程里再开 OpenMP，总并发 ≈ 池线程数 × OpenMP 线程数，容易过载；可在任务内限制 OpenMP 线程数，或与 TBB 的全局线程数/arena 配置对齐。

**反例**：在 OpenMP 的 for 体里对每个分片 co_await HttpGetAsync。子线程上没有本框架的 Step 恢复路径，模型不成立。

---

## 8. 迁移建议（从现有协程逐步引入）

1. **先度量**：在疑似热点加耗时日志，确认瓶颈是 CPU/阻塞而非 IO。
2. **先拷贝边界**：将线程池任务输入输出改为 **POD/可移动值**，避免共享指针除非生命周期清晰。
3. **先实现最小闭环**：单一路径试点（如一个 Interface 命令），验证无 **`resume` 后 Step 已释放**。
4. **再抽象**：提炼为通用 `ThreadPoolAwaiter` 或 `RunOnThreadPool(step, ...)`，统一超时与取消语义。
5. **文档与 Code Review**：任何新 awaiter 必须注明「是否占用 `m_coroHandle`、是否与 `HttpRespAwaiter` 互斥」。

---

## 9. 排错建议

| 现象 | 可能原因 | 排查方向 |
|------|----------|----------|
| 随机崩溃 / ASan 堆损坏 | 线程池内访问 Step / 跨线程 `resume` | 检查 offload 闭包捕获与 `resume` 线程 |
| 协程卡住不结束 | `future` 未回调、未投递回 Worker | 检查 `commit` 是否异常、回调是否执行 |
| 超时后仍收到 late 响应 | 未取消池任务或未忽略结果 | 增加 cancelled 标志，池内早退 |
| 嵌套 await 错乱 | `m_coroHandle` 被覆盖 | 减少嵌套或实现多挂起点队列 |

---

## 10. 小结

- **默认**：`StepCo20` 继续以 **事件循环 + 现有异步 API** 为主。
- **需要时**：仅对 **CPU 密集 / 阻塞同步库** 做 **受控线程池 offload**，且 **结果与 `resume` 必须回到事件线程**。
- **落地前**：需自研与 `HttpRespAwaiter` 同级别的 **awaiter + 回 Worker 投递**；并处理 **超时、取消、`m_coroHandle` 嵌套** 问题。

本文与 [StepCo20-coroutine-migration.md](StepCo20-coroutine-migration.md) 一并作为协程相关开发的**设计与审查依据**。
