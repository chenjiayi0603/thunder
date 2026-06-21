#include <gtest/gtest.h>
#include "thread/worker_deque.h"

#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <set>
#include <functional>

using Task = std::function<void()>;
using Deque = util::WorkerDeque<Task>;

// ─── 基础功能 ───────────────────────────────────────────────────

TEST(WorkerDeque, InitiallyEmpty)
{
    Deque d;
    EXPECT_EQ(d.size(), 0u);
    EXPECT_FALSE(d.dequeue().has_value());
}

TEST(WorkerDeque, PushAndDequeue)
{
    Deque d;
    int val = 0;
    EXPECT_TRUE(d.push([&]{ val = 1; }));
    EXPECT_EQ(d.size(), 1u);

    auto t = d.dequeue();
    ASSERT_TRUE(t.has_value());
    (*t)();
    EXPECT_EQ(val, 1);
    EXPECT_EQ(d.size(), 0u);
    EXPECT_FALSE(d.dequeue().has_value());
}

TEST(WorkerDeque, FIFOOrder)
{
    Deque d;
    std::vector<int> out;
    for (int i = 0; i < 8; i++)
        EXPECT_TRUE(d.push([&out, i]{ out.push_back(i); }));

    while (auto t = d.dequeue())
        (*t)();

    std::vector<int> expected{0,1,2,3,4,5,6,7};
    EXPECT_EQ(out, expected);
}

TEST(WorkerDeque, FullReturnsFalse)
{
    Deque d;
    for (uint32_t i = 0; i < Deque::CAPACITY; i++)
        EXPECT_TRUE(d.push([]{}));
    // 再 push 一个应失败
    EXPECT_FALSE(d.push([]{}));
}

TEST(WorkerDeque, WrapAround)
{
    // 验证 ring 的环绕：push CAP 个 → 消费掉 → 再 push CAP 个仍正常
    Deque d;
    auto fill_drain = [&]{
        for (uint32_t i = 0; i < Deque::CAPACITY; i++)
            EXPECT_TRUE(d.push([]{}));
        for (uint32_t i = 0; i < Deque::CAPACITY; i++)
            EXPECT_TRUE(d.dequeue().has_value());
    };
    fill_drain();
    fill_drain(); // 第二轮，head/tail 都绕过一圈
    EXPECT_EQ(d.size(), 0u);
}

// ─── steal_into ─────────────────────────────────────────────────

TEST(WorkerDeque, StealFromEmpty)
{
    Deque src, dst;
    EXPECT_EQ(dst.steal_into(src, 128), 0u);
}

TEST(WorkerDeque, StealHalf)
{
    Deque src, dst;
    for (int i = 0; i < 10; i++)
        src.push([]{}); // src 有 10 个

    uint32_t stolen = dst.steal_into(src, 128);
    // 偷一半：10 - 10/2 = 5
    EXPECT_EQ(stolen, 5u);
    EXPECT_EQ(src.size(), 5u);
    EXPECT_EQ(dst.size(), 5u);
}

TEST(WorkerDeque, StealRespectMaxN)
{
    Deque src, dst;
    for (int i = 0; i < 20; i++)
        src.push([]{}); // src 有 20 个

    uint32_t stolen = dst.steal_into(src, 3); // max_n = 3
    EXPECT_EQ(stolen, 3u);
    EXPECT_EQ(src.size(), 17u);
    EXPECT_EQ(dst.size(), 3u);
}

TEST(WorkerDeque, StealOrderPreserved)
{
    // steal 后 dst 里任务的执行顺序应与 src push 顺序一致（FIFO）
    Deque src, dst;
    std::vector<int> out;
    for (int i = 0; i < 10; i++)
        src.push([&out, i]{ out.push_back(i); });

    dst.steal_into(src, 128); // 偷走 5 个（i=0..4）

    while (auto t = dst.dequeue()) (*t)();
    EXPECT_EQ(out, (std::vector<int>{0,1,2,3,4}));
}

TEST(WorkerDeque, StealIntoFullDst)
{
    Deque src, dst;
    // dst 填满
    for (uint32_t i = 0; i < Deque::CAPACITY; i++)
        dst.push([]{});
    // src 有任务
    src.push([]{});

    EXPECT_EQ(dst.steal_into(src, 128), 0u); // dst 无空间
    EXPECT_EQ(src.size(), 1u);
}

// ─── 多线程：1 pusher + 1 worker（dequeue）─────────────────────

TEST(WorkerDeque, ConcurrentPushDequeue)
{
    Deque d;
    constexpr int N = 100000;
    std::atomic<int> consumed{0};

    // pusher 线程
    std::thread pusher([&]{
        int pushed = 0;
        while (pushed < N)
        {
            if (d.push([&consumed]{ consumed.fetch_add(1, std::memory_order_relaxed); }))
                pushed++;
            else
                std::this_thread::yield(); // 满了让 worker 消费
        }
    });

    // worker 线程
    std::thread worker([&]{
        int done = 0;
        while (done < N)
        {
            if (auto t = d.dequeue())
            {
                (*t)();
                done++;
            }
            else
                std::this_thread::yield();
        }
    });

    pusher.join();
    worker.join();
    EXPECT_EQ(consumed.load(), N);
}

// ─── 多线程：1 pusher + 1 worker + 多个 thief（steal_into）─────

TEST(WorkerDeque, ConcurrentPushDequeueSteal)
{
    // 1 pusher → victim deque
    // 1 owner  → victim.dequeue()
    // 2 thiefs → steal_into(victim) → 消费各自 dst
    Deque victim, dst0, dst1;
    constexpr int N = 50000;
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    auto consume_deque = [&consumed](Deque& d){
        while (auto t = d.dequeue())
        {
            (*t)();
        }
    };

    std::thread pusher([&]{
        int pushed = 0;
        while (pushed < N)
        {
            if (victim.push([&consumed]{ consumed.fetch_add(1, std::memory_order_relaxed); }))
                pushed++;
            else
                std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    std::thread owner([&]{
        while (!done.load(std::memory_order_acquire) || victim.size() > 0)
        {
            if (auto t = victim.dequeue())
                (*t)();
            else
                std::this_thread::yield();
        }
    });

    std::thread thief0([&]{
        while (!done.load(std::memory_order_acquire) || victim.size() > 0)
        {
            dst0.steal_into(victim, 64);
            consume_deque(dst0);
            std::this_thread::yield();
        }
        consume_deque(dst0);
    });

    std::thread thief1([&]{
        while (!done.load(std::memory_order_acquire) || victim.size() > 0)
        {
            dst1.steal_into(victim, 64);
            consume_deque(dst1);
            std::this_thread::yield();
        }
        consume_deque(dst1);
    });

    pusher.join();
    owner.join();
    thief0.join();
    thief1.join();

    // 排干所有 deque 剩余
    consume_deque(victim);
    consume_deque(dst0);
    consume_deque(dst1);

    EXPECT_EQ(consumed.load(), N);
}

// ─── 多 thief 竞争：无任务丢失、无重复执行 ────────────────────

TEST(WorkerDeque, NoLostNoDoubleExecute)
{
    Deque src;
    // N 必须 <= CAP，deque 是固定容量，超出 push 返回 false 即正常溢出
    constexpr int N = 200;
    std::atomic<int> seen[N];
    for (int i = 0; i < N; i++) seen[i].store(0);

    for (int i = 0; i < N; i++)
    {
        bool ok = src.push([&seen, i]{ seen[i].fetch_add(1, std::memory_order_relaxed); });
        ASSERT_TRUE(ok) << "push failed at i=" << i; // N<=CAP，不应失败
    }

    // 4 个 thief 并发 steal + 消费
    constexpr int THIEFS = 4;
    std::vector<Deque> dsts(THIEFS);
    std::vector<std::thread> threads;
    threads.reserve(THIEFS);
    for (int t = 0; t < THIEFS; t++)
    {
        threads.emplace_back([&src, &dsts, t]{
            while (true)
            {
                uint32_t stolen = dsts[t].steal_into(src, 32);
                if (stolen == 0 && src.size() == 0)
                    break;
                while (auto task = dsts[t].dequeue())
                    (*task)();
                std::this_thread::yield();
            }
            while (auto task = dsts[t].dequeue())
                (*task)();
        });
    }
    for (auto& th : threads) th.join();

    // 排干 src 中剩余（可能有 dequeue 竞争留下的）
    while (auto task = src.dequeue()) (*task)();

    // 验证每个任务恰好执行一次
    for (int i = 0; i < N; i++)
        EXPECT_EQ(seen[i].load(), 1) << "task " << i << " executed "
                                     << seen[i].load() << " times";
}
