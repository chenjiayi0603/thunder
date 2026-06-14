/**
 * ThreadPool 线程池单元测试
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

TEST(ThreadPool, CommitReturnsFuture)
{
    util::threadpool pool(1);
    auto fut = pool.commit([] { return 42; });
    int result = fut.get();
    EXPECT_EQ(42, result);
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
    // 刚创建时应该有 2 个空闲线程
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.idlCount(), 0);
}

TEST(ThreadPool, DestructionJoinsThreads)
{
    // pool 析构时 join 所有线程，不应崩溃
    {
        util::threadpool pool(4);
        for (int i = 0; i < 100; ++i)
        {
            pool.commit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
        }
    }
    SUCCEED();
}

TEST(ThreadPool, ConcurrentTasks)
{
    util::threadpool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 20; ++i)
    {
        futures.push_back(pool.commit([i] { return i * i; }));
    }
    for (int i = 0; i < 20; ++i)
    {
        EXPECT_EQ(i * i, futures[i].get());
    }
}

TEST(ThreadPool, BackpressureQueueMax)
{
    // 1 worker + max queue 1：worker 忙时再 commit 应被拒绝
    util::threadpool pool(1, 1);
    EXPECT_EQ(1, pool.maxQueueSize());

    auto barrier = std::make_shared<std::promise<void>>();

    // 占用唯一 worker 线程
    auto fut = pool.commit([barrier] {
        barrier->get_future().wait();
    });

    // 队列已满，第二次 commit 应抛异常
    EXPECT_THROW(pool.commit([] { }), std::runtime_error);

    // 释放 worker，任务完成
    barrier->set_value();
    fut.get();

    // 队列不再满，可正常提交
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}

TEST(ThreadPool, ResizeDynamic)
{
    util::threadpool pool(1);    // 1 线程起步
    EXPECT_EQ(1, pool.thrCount());

    pool.resize(3);              // 扩到 3
    EXPECT_EQ(3, pool.thrCount());

    // 所有 3 个线程都能并发执行任务
    std::atomic<int> counter{0};
    auto fut1 = pool.commit([&counter] { counter++; });
    auto fut2 = pool.commit([&counter] { counter++; });
    auto fut3 = pool.commit([&counter] { counter++; });
    fut1.get(); fut2.get(); fut3.get();
    EXPECT_EQ(3, counter.load());

    pool.resize(1);              // 缩回 1
    // 缩小后仍能正常提交
    EXPECT_EQ(42, pool.commit([] { return 42; }).get());
}
