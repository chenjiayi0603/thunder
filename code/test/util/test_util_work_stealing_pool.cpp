/**
 * WorkStealingPool 集成测试
 *
 * 用例与 test_util_threadpool.cpp 对齐，验证：
 *   - 公开 API 语义与 util::threadpool 完全一致
 *   - work-stealing 特有行为：任务无丢失、无重复执行
 *   - global_q 溢出路径正常工作
 *   - 缩容 drain 不丢任务
 */
#include "thread/work_stealing_pool.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <vector>
#include <atomic>

using Pool = util::WorkStealingPool;

// ─── API 兼容性（对标 threadpool 同名用例）───────────────────────

TEST(WorkStealingPool, DefaultConstruction)
{
    Pool pool(2);
    EXPECT_EQ(2, pool.thrCount());
}

TEST(WorkStealingPool, DefaultOneThread)
{
    Pool pool;
    EXPECT_EQ(1, pool.thrCount());
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(WorkStealingPool, CommitReturnsFuture)
{
    Pool pool(1);
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(WorkStealingPool, MultipleTasks)
{
    Pool pool(4);
    auto f1 = pool.commit([] { return 1; });
    auto f2 = pool.commit([] { return 2; });
    auto f3 = pool.commit([] { return 3; });
    EXPECT_EQ(1, f1.get());
    EXPECT_EQ(2, f2.get());
    EXPECT_EQ(3, f3.get());
}

TEST(WorkStealingPool, TaskWithArgs)
{
    Pool pool(1);
    EXPECT_EQ(30, pool.commit([](int a, int b) { return a + b; }, 10, 20).get());
}

TEST(WorkStealingPool, VoidTask)
{
    Pool pool(1);
    std::atomic<int> flag{0};
    pool.commit([&flag] { flag.store(1); }).get();
    EXPECT_EQ(1, flag.load());
}

TEST(WorkStealingPool, IdleCount)
{
    Pool pool(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.idlCount(), 0);
}

TEST(WorkStealingPool, DestructionJoinsThreads)
{
    {
        Pool pool(4);
        for (int i = 0; i < 100; ++i)
            pool.commit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    }
    SUCCEED();
}

TEST(WorkStealingPool, ConcurrentTasks)
{
    Pool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 20; ++i)
        futures.push_back(pool.commit([i] { return i * i; }));
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(i * i, futures[i].get());
}

TEST(WorkStealingPool, BackpressureQueueMax)
{
    Pool pool(1, 1);
    EXPECT_EQ(1u, pool.maxQueueSize());
    auto barrier = std::make_shared<std::promise<void>>();
    auto fut = pool.commit([barrier] { barrier->get_future().wait(); });
    EXPECT_THROW(pool.commit([] {}), std::runtime_error);
    barrier->set_value();
    fut.get();
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(WorkStealingPool, MaxQueueSizeDefault)
{
    Pool pool(4);
    EXPECT_EQ(256u, pool.maxQueueSize());
    Pool small(2);
    EXPECT_EQ(128u, small.maxQueueSize());
    Pool custom(4, 100);
    EXPECT_EQ(100u, custom.maxQueueSize());
}

TEST(WorkStealingPool, QueueSizeTracking)
{
    Pool pool(4, 16);
    EXPECT_EQ(0u, pool.queueSize());
    auto f1 = pool.commit([] { return 1; });
    auto f2 = pool.commit([] { return 2; });
    auto f3 = pool.commit([] { return 3; });
    EXPECT_LE(pool.queueSize(), pool.maxQueueSize());
    EXPECT_EQ(1, f1.get()); EXPECT_EQ(2, f2.get()); EXPECT_EQ(3, f3.get());
}

TEST(WorkStealingPool, ResizeDynamic)
{
    Pool pool(1);
    EXPECT_EQ(1, pool.thrCount());
    pool.resize(3);
    EXPECT_EQ(3, pool.thrCount());
    std::atomic<int> counter{0};
    auto f1 = pool.commit([&counter] { counter++; });
    auto f2 = pool.commit([&counter] { counter++; });
    auto f3 = pool.commit([&counter] { counter++; });
    f1.get(); f2.get(); f3.get();
    EXPECT_EQ(3, counter.load());
    pool.resize(1);
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(WorkStealingPool, ResizeDuringWork)
{
    Pool pool(4);
    std::atomic<int> done{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 8; ++i)
        futs.push_back(pool.commit([&done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            done++;
        }));
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    pool.resize(2);
    for (auto& f : futs) f.get();
    EXPECT_EQ(8, done.load());
}

TEST(WorkStealingPool, ExceptionPropagation)
{
    Pool pool(2);
    auto fut = pool.commit([]() -> int {
        throw std::runtime_error("task error");
    });
    EXPECT_THROW(fut.get(), std::runtime_error);
}

// ─── Work Stealing 特有：任务无丢失 ─────────────────────────────

TEST(WorkStealingPool, NoTaskLost)
{
    // 大量短任务，验证 work stealing 下无任务丢失
    Pool pool(8);
    constexpr int N = 10000;
    std::atomic<int> count{0};
    std::vector<std::future<void>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i)
        futs.push_back(pool.commit([&count] { count.fetch_add(1, std::memory_order_relaxed); }));
    for (auto& f : futs) f.get();
    EXPECT_EQ(N, count.load());
}

// ─── global_q 溢出路径：强制填满所有 deque ──────────────────────

TEST(WorkStealingPool, GlobalQOverflowPath)
{
    // 1 个 worker，deque 容量 256，提交 512 个任务强制走 global_q
    // 任务里加 sleep 确保 deque 会满
    Pool pool(1, 1024);
    constexpr int N = 512;
    std::atomic<int> count{0};
    // 先提交一个 blocker 占住 worker，让 deque 积压
    auto barrier = std::make_shared<std::promise<void>>();
    pool.commit([barrier] { barrier->get_future().wait(); });

    std::vector<std::future<void>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i)
        futs.push_back(pool.commit([&count] { count++; }));

    barrier->set_value(); // 释放 blocker，让 worker 开始消费
    for (auto& f : futs) f.get();
    EXPECT_EQ(N, count.load());
}

// ─── 缩容 drain：resize 缩小后任务不丢 ──────────────────────────

TEST(WorkStealingPool, ResizeShrinkNoDrain)
{
    Pool pool(4);
    constexpr int N = 200;
    std::atomic<int> count{0};

    // 提交任务后立即缩容，验证 drain 到 global_q 后仍能执行完
    std::vector<std::future<void>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i)
        futs.push_back(pool.commit([&count] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            count++;
        }));
    pool.resize(1); // 缩到 1 个 worker，其他 worker drain 后退出
    for (auto& f : futs) f.get();
    EXPECT_EQ(N, count.load());
}
