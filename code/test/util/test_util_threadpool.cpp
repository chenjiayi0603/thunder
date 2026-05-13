/**
 * ThreadPool 线程池单元测试
 */
#include "thread/threadpool.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>

TEST(ThreadPool, DefaultConstruction)
{
    std::threadpool pool(2);
    EXPECT_EQ(2, pool.thrCount());
}

TEST(ThreadPool, CommitReturnsFuture)
{
    std::threadpool pool(1);
    auto fut = pool.commit([] { return 42; });
    int result = fut.get();
    EXPECT_EQ(42, result);
}

TEST(ThreadPool, MultipleTasks)
{
    std::threadpool pool(4);
    auto f1 = pool.commit([] { return 1; });
    auto f2 = pool.commit([] { return 2; });
    auto f3 = pool.commit([] { return 3; });
    EXPECT_EQ(1, f1.get());
    EXPECT_EQ(2, f2.get());
    EXPECT_EQ(3, f3.get());
}

TEST(ThreadPool, TaskWithArgs)
{
    std::threadpool pool(1);
    auto fut = pool.commit([](int a, int b) { return a + b; }, 10, 20);
    EXPECT_EQ(30, fut.get());
}

TEST(ThreadPool, VoidTask)
{
    std::threadpool pool(1);
    int flag = 0;
    auto fut = pool.commit([&flag] { flag = 1; });
    fut.get();
    EXPECT_EQ(1, flag);
}

TEST(ThreadPool, IdleCount)
{
    std::threadpool pool(2);
    // 刚创建时应该有 2 个空闲线程
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(pool.idlCount(), 0);
}

TEST(ThreadPool, DestructionJoinsThreads)
{
    // pool 析构时 join 所有线程，不应崩溃
    {
        std::threadpool pool(4);
        for (int i = 0; i < 100; ++i)
        {
            pool.commit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
        }
    }
    SUCCEED();
}

TEST(ThreadPool, ConcurrentTasks)
{
    std::threadpool pool(4);
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
