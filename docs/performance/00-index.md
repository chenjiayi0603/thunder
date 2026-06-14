# 性能设计与分析

> 本文档索引 performance/ 目录下所有性能相关的设计和实测数据。

---

## 文档索引

| 文档 | 内容 | 验证方式 |
|------|------|:--------:|
| [`20-threadpool-analysis.md`](20-threadpool-analysis.md) | ThreadPool 架构分析、问题诊断、改进路线 | 代码审查 |
| [`03-threadpool-queue-bench.md`](03-threadpool-queue-bench.md) | `std::queue+mutex` vs `ConcurrentQueue` 对比 + 实施记录 | ✅ 实测 |
| [`05-perf-analysis.md`](05-perf-analysis.md) | 全量性能对比（TBB、Redis、MySQL、协程） | ✅ 实测 |

---

## 核心设计决策

### 1. 为什么用 threadpool 而不是 parallel_for

| | ThreadPool | parallel_for |
|--|:----------:|:------------:|
| 场景 | 任务并行（请求随机抵达）| 数据并行（已知迭代空间）|
| Thunder 匹配度 | ✅ 完全匹配 | ❌ 不匹配 |
| 调度 | 共享队列，谁闲谁取 | work-stealing |
| 依赖 | 单头文件 | TBB / 标准库 |

### 2. 为什么用 moodycamel::ConcurrentQueue

- 多生产者 + 多消费者场景（多个协程同时 commit + 多个 worker 消费）
- lock-free，不入内核，无 futex 竞争
- 实测比 `std::queue+mutex` 快 2.5~3.7x
- 来源：https://github.com/cameron314/concurrentqueue

### 3. 为什么协程 IO 比同步 IO 好

| | 同步（阻塞）| 协程（非阻塞）|
|--|:----------:|:------------:|
| 事件循环 | ❌ 阻塞 | ✅ 不阻塞 |
| 并发吞吐 | 固定 | 随并发线性增长 |
| 单请求延迟 | 低 | 略高（resume 开销可忽略）|
| 代码可读性 | 阻塞代码直观 | 线性协程代码 |

### 4. 为什么 Redis 协程比同步慢（当前）

不是因为协程慢，是因为 `AutoRedisCmd` 每次 `co_await` 都新建 TCP 连接。
加连接池后应接近或超过同步性能。

---

## 性能数据速览

| 场景 | 测量值 | 对比 |
|------|:------:|:----:|
| Echo 空响应 | 124k QPS | HTTP 基线 |
| Redis 协程（HelloCoRedisCo）| 12k QPS | 32 并发，无连接池 |
| MySQL 同步（HelloCoMysqlCo）| 460 QPS | 同步 DBI，阻塞事件循环 |
| ThreadPool 入队（ConcurrentQueue）| 7.8 Mop/s | 4P-4C，比 mutex 快 2.5x |
| ThreadPool vs TBB（CPU 密集）| 等同 | 均跑满 20 核 |
| 异步 Redis（hiredis async）| 29M QPS | 流水线化 282x vs 同步 |
