# Thunder 项目介绍与 FAQ

> 本文档用于对外介绍 Thunder 项目核心设计，以及常见问答。

---

## 一、项目概述

Thunder 是一个**高性能 C++20 网络框架**，采用**单线程事件循环 + 协程 + 线程池**架构，
适用于高并发、低延迟的服务端场景（网关、微服务、游戏后端）。

### 核心特性

| 特性 | 说明 |
|------|------|
| 事件驱动 | 基于 libev / io_uring 的事件循环模型 |
| C++20 协程 | 用 `co_await` 替代传统状态机，业务代码线性化 |
| 线程池 offload | CPU 密集 / 阻塞 IO 任务卸载到线程池，不阻塞事件循环 |
| 多进程 | 单机多 Worker 进程，每个进程独立事件循环 |
| 编解码层 | 12 种 codec 可插拔（HTTP/HTTPS/WSS/Protobuf/自定义）|
| 注册中心 | 基于 etcd 的服务注册与发现（CAS slot 分配 + lease 保活）|

### 架构图

```
                        ┌─────────────────────────┐
                        │     etcd 注册中心        │
                        │  /thunder/slot/{0~255}   │
                        │  /thunder/registry/      │
                        └──────────┬──────────────┘
                                   │
     ┌──────────┐     ┌────────────┴────────────┐     ┌──────────┐
     │ Worker 1 │ ──→ │        Center            │ ←── │ Worker N │
     │ (进程)   │     │    (节点发现/路由)        │     │ (进程)   │
     └────┬─────┘     └─────────────────────────┘     └────┬─────┘
          │                                                 │
     ┌────┴─────────────────────────────────────────────────┴────┐
     │                  事件循环 (libev / io_uring)                │
     │  ┌─────────────────────────────────────────────────────┐  │
     │  │  协程 co_await                                      │  │
     │  │  ├── RedisCoHelper (异步 Redis)                      │  │
     │  │  ├── MySqlAwaitable (异步 MySQL)                     │  │
     │  │  └── MakePoolOffloadAwaiter (线程池 offload)         │  │
     │  └─────────────────────────────────────────────────────┘  │
     └───────────────────────────────────────────────────────────┘
```

---

## 二、核心设计问答

### Q1: 为什么用单线程事件循环，而不是多线程？

**因为事件循环避免了并发竞争。**

Thunder 的每个 Worker 进程是**单线程**事件循环。所有网络 IO、协程调度、
消息派发都在一个线程里完成，不需要锁、不需要考虑数据竞争。

| 方案 | 优点 | 缺点 |
|------|------|------|
| 单线程事件循环 | 无锁、无竞争、简单 | 不能利用多核（用多进程解决）|
| 多线程事件循环 | 可利用多核 | 锁竞争、调试复杂、并发 bug |
| 多进程事件循环 | 无锁 + 多核 | 进程间通信成本 |

Thunder 的选择：**单进程单线程 + 多进程水平扩展**。

---

### Q2: 协程解决了什么问题？

**传统的异步编程有两种方式：**

1. **回调**：代码碎片化，逻辑分散在不同回调函数里，难以维护
2. **状态机**：每个异步点需要记录状态 + switch 跳转，新增逻辑要改动状态枚举

**协程让异步代码像同步一样写：**

```cpp
// 传统状态机：5 个状态函数，逻辑跳转靠 SetNextState
// 协程：1 个线性函数
net::AsyncTask AsyncBody(net::StepCo20& st) {
    co_await self.HttpGetAsync("http://baidu.com");   // 挂起，不阻塞
    co_await self.HttpGetAsync("http://sogou.com");   // 恢复，继续
    co_await self.HttpGetAsync("http://qq.com");      // 线性执行
    co_return;
}
```

**协程 vs 线程**：

| | 协程 | 线程 |
|--|------|------|
| 调度 | 用户态（co_await/co_return）| 内核态（OS 调度器）|
| 切换开销 | 纳秒级（函数调用）| 微秒级（上下文切换）|
| 并发 | 单线程内数百万 | 受限于线程数 |
| 数据竞争 | 无（单线程）| 需要锁 |

---

### Q3: `net::AsyncTask` 是什么？为什么协程函数必须返回它？

`AsyncTask` 是 Thunder 的 C++20 协程返回类型，**桥接了 C++20 协程和 StepCo20 状态机**：

```cpp
struct AsyncTask {
    struct promise_type {
        StepCo20* stepAutoNotify_;  // 持有 StepCo20 指针

        promise_type(StepCo20& step, ...) : stepAutoNotify_(&step) {}
        // 从 co_await 表达式第一个参数拿到 step

        std::suspend_never initial_suspend() { return {}; }
        // 协程立即执行（不懒启动）

        void return_void() { stepAutoNotify_->NotifyEmitCoroutineSuccess(); }
        // 协程完成时通知框架

        std::suspend_always final_suspend() { return {}; }
        // 结束时暂停，由 AsyncTask 析构销毁协程帧
    };
};
```

协程函数的签名必须统一为 `AsyncTask XxxCo(StepCo20& step, ...)`，因为 `promise_type` 从第一个参数拿 `StepCo20&`。

---

### Q4: ThreadPool 的设计？为什么不直接用 parallel_for？

**Thunder 的 offload 是任务并行，不是数据并行。**

```
parallel_for（数据并行）:
  for (int i = 0; i < N; i++) process(arr[i]);  // 已知迭代空间，均匀分割

ThreadPool offload（任务并行）:
  请求 A → threadpool.commit(workA)   // 不知下一个何时到
  请求 B → threadpool.commit(workB)   // 各请求独立
  请求 C → threadpool.commit(workC)
```

**核心区别：**

| | parallel_for | ThreadPool |
|--|:-----------:|:----------:|
| 输入 | 已知大小的数组 | 随时到达的请求 |
| 调度策略 | work-stealing | 共享队列 |
| 适用 | 数据并行（渲染、矩阵） | 任务并行（后端服务）|
| 依赖 | TBB / 标准库 | 单头文件 |

**ThreadPool 设计要点：**

1. `moodycamel::ConcurrentQueue` — lock-free MPMC，多生产者不串行
2. 队列上限 + 背压 — 超限抛异常，防止 OOM，不破坏 FIFO
3. `resize(n)` 动态扩缩容 — 增大直接建线程，缩小空闲 worker 退出
4. 1 线程起步 — 多进程不超订 CPU

---

### Q5: 为什么 AsyncTask 要 suspend_always final_suspend？

因为 AsyncTask 是**栈对象**，协程帧（coroutine frame）的释放由 AsyncTask 的析构函数控制：

```cpp
~AsyncTask() { if (coro_) coro_.destroy(); }
```

如果 `final_suspend` 返回 `suspend_never`，协程结束时会自动释放协程帧，
但 AsyncTask 可能还在栈上，再调用 destroy 就是 double free。

---

### Q6: 异步 Redis / MySQL 的协程实现

```cpp
// Redis 协程
net::RedisCoHelper r(&step, "127.0.0.1", 6379);
const net::RedisReply rsp = co_await r.Set("key", "val");

// 内部实现：await_suspend 中调用 redisAsyncConnect → redisAsyncCommand
// 回复到达 → OnRedisCmdResult → h.resume() → 协程继续
```

当前 RedisCoHelper 每次 `co_await` 都新建连接（无连接池）。
这是已知优化点，加连接池后可大幅提升 QPS。

MySQL 目前使用同步 DBI，无协程版。`MySqlAwaitable` 存在但未在业务层使用。

---

### Q7: 性能数据

| 场景 | 数据 | 对比 |
|------|:----:|:----:|
| Echo 空响应（/hello/raw）| 133~141k QPS | HTTPS，1 Worker 绑核，wrk c100，见报告 §10 |
| Redis 协程 | 12k QPS | 单连接，无连接池 |
| ThreadPool 队列 vs mutex | 2.5x~3.7x | ConcurrentQueue 优势 |
| ThreadPool vs TBB CPU密集 | 等同 | 两者均满 20 核 |
| 异步 Redis(hiredis async) | 29M QPS | 流水线化，282x vs 同步 |

---

## 三、项目亮点

1. **C++20 协程落地实践**：`AsyncTask` + `PoolOffloadAwaiter` 等模板，展示了 C++20 协程在工业级网络框架中的应用
2. **无锁线程池**：`moodycamel::ConcurrentQueue` 替代 `queue + mutex`，多生产者性能提升 2.5~3.7x
3. **协程 + 线程池 offload**：解决了"事件循环不能阻塞"的问题，使同步 SDK 也能融入异步框架
4. **模块化 codec**：12 种编解码器可插拔，支持 HTTP/HTTPS/WSS/Protobuf 等协议
5. **etcd 注册中心**：CAS slot 分配 + lease 保活，支持服务发现与健康检查

---

## 四、FAQ 问答

### Q: suspend_never / suspend_always 的区别？

| | `suspend_never` | `suspend_always` |
|--|:--------------:|:---------------:|
| initial_suspend | 协程启动后立即执行 | 协程启动后挂起，需要手动 resume |
| final_suspend | 协程结束自动销毁帧 | 协程结束挂起，由持有者销毁帧 |

`AsyncTask` 的 `initial_suspend = suspend_never`（立即执行），
`final_suspend = suspend_always`（避免 double free）。

### Q: 协程帧什么时候分配？什么时候释放？

- **分配**：首次调用协程函数时，在堆上分配协程帧（含 promise_type + 局部变量 + 形参副本）
- **释放**：`~AsyncTask` → `coro_.destroy()`，或协程运行到 `co_return` 且 `final_suspend = suspend_never`

### Q: co_await 的执行过程

```
co_await awaiter:
  1. awaiter.await_ready() → true? 直接拿结果
  2. false → awaiter.await_suspend(h) → 挂起协程
  3. 异步操作完成 → 调 h.resume() → 恢复协程
  4. awaiter.await_resume() → 返回结果
```

### Q: 多进程间怎么通信？

Thunder 进程间不直接通信。通过 etcd 做服务发现，Center 节点负责路由。
Worker 之间通过 protobuf 消息 + 内部 TCP 连接转发。
