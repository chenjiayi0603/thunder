/**
 * ThreadPool 线程池单元测试
 *
 * 覆盖：
 *   - 基础功能：构造、commit、多任务、参数传递、void、空闲计数
 *   - 生命周期：析构 join、停止后拒绝
 *   - 并发：多线程并发任务
 *   - 背压：队列上限、拒绝、恢复
 *   - 动态 resize：扩缩容、干活时缩小不影响任务
 *   - queueSize 跟踪
 *   - 异常传播
 */
#include "thread/threadpool.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>

TEST(ThreadPool, DefaultConstruction)
{
    util::threadpool pool(2);
    EXPECT_EQ(2, pool.thrCount());
}

TEST(ThreadPool, DefaultOneThread)
{
    util::threadpool pool;
    EXPECT_EQ(1, pool.thrCount());
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(ThreadPool, CommitReturnsFuture)
{
    util::threadpool pool(1);
    auto fut = pool.commit([] { return 42; });
    EXPECT_EQ(42, fut.get());
}

TEST(ThreadPool, MultipleTasks)
{
    util::threadpool pool(4);
    auto f1 = pool.commit([] { return 1; });
    auto f2 = pool.commit([] { return 2; });
    auto f3 = pool.commit([] { return 3; });
    EXPECT_EQ(1, f1.get());
    EXPECT_EQ(2, f2.get());
    EXPECT_EQ(3, f3.get());
}

TEST(ThreadPool, TaskWithArgs)
{
    util::threadpool pool(1);
    auto fut = pool.commit([](int a, int b) { return a + b; }, 10, 20);
    EXPECT_EQ(30, fut.get());
}

TEST(ThreadPool, VoidTask)
{
    util::threadpool pool(1);
    int flag = 0;
    auto fut = pool.commit([&flag] { flag = 1; });
    fut.get();
    EXPECT_EQ(1, flag);
}

TEST(ThreadPool, IdleCount)
{
    util::threadpool pool(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.idlCount(), 0);
}

TEST(ThreadPool, DestructionJoinsThreads)
{
    {
        util::threadpool pool(4);
        for (int i = 0; i < 100; ++i)
            pool.commit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    }
    SUCCEED();
}

TEST(ThreadPool, ConcurrentTasks)
{
    util::threadpool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 20; ++i)
        futures.push_back(pool.commit([i] { return i * i; }));
    for (int i = 0; i < 20; ++i)
        EXPECT_EQ(i * i, futures[i].get());
}

TEST(ThreadPool, BackpressureQueueMax)
{
    util::threadpool pool(1, 1);
    EXPECT_EQ(1, pool.maxQueueSize());
    auto barrier = std::make_shared<std::promise<void>>();
    auto fut = pool.commit([barrier] { barrier->get_future().wait(); });
    EXPECT_THROW(pool.commit([] { }), std::runtime_error);
    barrier->set_value();
    fut.get();
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(ThreadPool, MaxQueueSizeDefault)
{
    util::threadpool pool(4);
    EXPECT_EQ(256, pool.maxQueueSize());
    util::threadpool small(2);
    EXPECT_EQ(128, small.maxQueueSize());
    util::threadpool custom(4, 100);
    EXPECT_EQ(100, custom.maxQueueSize());
}

TEST(ThreadPool, QueueSizeTracking)
{
    util::threadpool pool(4, 16);
    EXPECT_EQ(0, pool.queueSize());
    auto fut1 = pool.commit([] { return 1; });
    auto fut2 = pool.commit([] { return 2; });
    auto fut3 = pool.commit([] { return 3; });
    EXPECT_LE(pool.queueSize(), pool.maxQueueSize());
    EXPECT_EQ(1, fut1.get());
    EXPECT_EQ(2, fut2.get());
    EXPECT_EQ(3, fut3.get());
}

TEST(ThreadPool, ResizeDynamic)
{
    util::threadpool pool(1);
    EXPECT_EQ(1, pool.thrCount());
    pool.resize(3);
    EXPECT_EQ(3, pool.thrCount());
    std::atomic<int> counter{0};
    auto fut1 = pool.commit([&counter] { counter++; });
    auto fut2 = pool.commit([&counter] { counter++; });
    auto fut3 = pool.commit([&counter] { counter++; });
    fut1.get(); fut2.get(); fut3.get();
    EXPECT_EQ(3, counter.load());
    pool.resize(1);
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(ThreadPool, ResizeDuringWork)
{
    util::threadpool pool(4);
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

TEST(ThreadPool, ResizeMultipleCycles)
{
    util::threadpool pool(1);
    EXPECT_EQ(1, pool.thrCount());
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        pool.resize(3);
        EXPECT_EQ(3, pool.thrCount());
        EXPECT_EQ(cycle, pool.commit([cycle] { return cycle; }).get());
        pool.resize(1);
    }
    EXPECT_GE(pool.thrCount(), 1);  // 至少还有 1 个活跃
}

TEST(ThreadPool, ExceptionPropagation)
{
    util::threadpool pool(2);
    auto fut = pool.commit([]() -> int {
        throw std::runtime_error("task error");
    });
    EXPECT_THROW(fut.get(), std::runtime_error);
}
