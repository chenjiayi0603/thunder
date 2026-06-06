/*******************************************************************************
* Project:  Thunder
* @file     test_shm_queue.cpp
* @brief    Unit + fork-based E2E tests for ShmRingQueue
* @author   cjy
* @date:    2026-05-10
******************************************************************************/
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include "labor/types/ShmRingQueue.hpp"

// 具名常量,替代硬编码 4096/128 (#4: 尺寸参数去重复)
static constexpr uint32_t kSlotSize = ShmRingQueue::kDefaultSlotSize;
static constexpr uint32_t kSlotCnt  = ShmRingQueue::kDefaultSlotCount;

// ==========================================================================
// Unit tests — single-process, multi-thread SPSC correctness
// ==========================================================================

TEST(ShmRingQueueUnit, CreateAndDestroy)
{
    ShmRingQueue* q = ShmRingQueue::Create(kSlotCnt, kSlotSize);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->ctrl.magic, 0x53484D51u);
    EXPECT_EQ(q->ctrl.slot_size, kSlotSize);
    EXPECT_EQ(q->ctrl.slot_count, kSlotCnt);
    EXPECT_TRUE(q->IsEmpty());
    EXPECT_FALSE(q->IsFull());
    EXPECT_EQ(q->Count(), 0u);
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, EnqueueDequeueSingle)
{
    ShmRingQueue* q = ShmRingQueue::Create(8, kSlotSize);
    ASSERT_NE(q, nullptr);

    const char* msg = "hello shm";
    uint32_t body_len = static_cast<uint32_t>(strlen(msg));
    ASSERT_LE(body_len, q->MaxBodySize());

    EXPECT_TRUE(q->TryEnqueue(100, 42, msg, body_len));
    EXPECT_FALSE(q->IsEmpty());
    EXPECT_EQ(q->Count(), 1u);

    uint32_t cmd = 0, seq = 0, out_len = 0;
    char buf[4096] = {0};
    EXPECT_TRUE(q->TryDequeue(cmd, seq, buf, out_len));
    EXPECT_EQ(cmd, 100u);
    EXPECT_EQ(seq, 42u);
    EXPECT_EQ(out_len, body_len);
    EXPECT_STREQ(buf, "hello shm");

    EXPECT_TRUE(q->IsEmpty());
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, EmptyDequeueReturnsFalse)
{
    ShmRingQueue* q = ShmRingQueue::Create(8, kSlotSize);
    ASSERT_NE(q, nullptr);

    uint32_t cmd, seq, out_len;
    char buf[256];
    EXPECT_FALSE(q->TryDequeue(cmd, seq, buf, out_len));
    EXPECT_EQ(out_len, 0u);

    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, FullQueueRejectsEnqueue)
{
    constexpr uint32_t kSlots = 4;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, kSlotSize);
    ASSERT_NE(q, nullptr);

    char body[1024] = {};
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        EXPECT_TRUE(q->TryEnqueue(i, i * 10, body, sizeof(body)));
    }

    EXPECT_TRUE(q->IsFull());
    EXPECT_FALSE(q->TryEnqueue(999, 1, body, sizeof(body)));

    uint32_t cmd, seq, out_len;
    EXPECT_TRUE(q->TryDequeue(cmd, seq, body, out_len));
    EXPECT_FALSE(q->IsFull());
    EXPECT_TRUE(q->TryEnqueue(999, 1, body, sizeof(body)));

    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, CreateDestroyNonDefaultSize)
{
    // #4 验证: 非默认尺寸 Create → Destroy 从 ctrl 读尺寸正确 munmap,不崩溃不泄漏
    constexpr uint32_t kNonDefSlots = 16;
    constexpr uint32_t kNonDefSize  = 2048;
    ShmRingQueue* q = ShmRingQueue::Create(kNonDefSlots, kNonDefSize);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->ctrl.slot_count, kNonDefSlots);
    EXPECT_EQ(q->ctrl.slot_size,  kNonDefSize);
    EXPECT_NE(q->ctrl.slot_size,  kSlotSize);  // 证明不是默认值
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, BodyTooLargeRejected)
{
    constexpr uint32_t kTinySlotSize = 128;
    ShmRingQueue* q = ShmRingQueue::Create(8, kTinySlotSize);
    ASSERT_NE(q, nullptr);

    char big[200] = {};
    EXPECT_FALSE(q->TryEnqueue(1, 1, big, sizeof(big)));

    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, SingleProducerSingleConsumerThreaded)
{
    constexpr uint32_t kSlots = 128;
    constexpr uint32_t kMsgs  = 100000;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, kSlotSize);
    ASSERT_NE(q, nullptr);

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};

    std::thread producer([&]() {
        char body[256];
        for (uint32_t i = 0; i < kMsgs; ++i)
        {
            uint32_t seq = i * 7 + 1;
            snprintf(body, sizeof(body), "msg-%u", i);
            while (!q->TryEnqueue(i % 1000, seq, body, static_cast<uint32_t>(strlen(body))))
            {
                std::this_thread::yield();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint32_t cmd, seq, out_len;
        char buf[256];
        uint64_t local_consumed = 0;
        while (local_consumed < kMsgs)
        {
            if (q->TryDequeue(cmd, seq, buf, out_len))
            {
                EXPECT_EQ(cmd, (local_consumed % 1000u));
                EXPECT_EQ(seq, local_consumed * 7u + 1u);
                ++local_consumed;
                consumed.store(local_consumed, std::memory_order_relaxed);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), kMsgs);
    EXPECT_EQ(consumed.load(), kMsgs);
    EXPECT_TRUE(q->IsEmpty());

    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, EventFdCreateClose)
{
    int efd = ShmRingQueue::CreateEventFd();
    EXPECT_GE(efd, 0);

    uint64_t val = 1;
    EXPECT_EQ(write(efd, &val, sizeof(val)), static_cast<ssize_t>(sizeof(val)));

    uint64_t out = 0;
    EXPECT_EQ(read(efd, &out, sizeof(out)), static_cast<ssize_t>(sizeof(out)));
    EXPECT_EQ(out, 1u);

    ShmRingQueue::CloseEventFd(efd);
    EXPECT_EQ(efd, -1);
}

// ==========================================================================
// E2E test — fork-based Manager↔Worker simulation with shm queue
// ==========================================================================

TEST(ShmRingQueueE2E, ForkedProducerConsumer)
{
    constexpr uint32_t kSlots = 64;
    constexpr uint32_t kMsgs  = 5000;

    ShmRingQueue* q_mgr_to_wkr = ShmRingQueue::Create(kSlots, kSlotSize);
    ShmRingQueue* q_wkr_to_mgr = ShmRingQueue::Create(kSlots, kSlotSize);
    int efd_mgr_to_wkr = ShmRingQueue::CreateEventFd();
    int efd_wkr_to_mgr = ShmRingQueue::CreateEventFd();

    ASSERT_NE(q_mgr_to_wkr, nullptr);
    ASSERT_NE(q_wkr_to_mgr, nullptr);
    ASSERT_GE(efd_mgr_to_wkr, 0);
    ASSERT_GE(efd_wkr_to_mgr, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0)
    {
        // ====== Child: simulates Worker ======
        close(efd_wkr_to_mgr);

        uint32_t cmd, seq, out_len;
        char buf[4096];
        uint64_t received = 0;

        while (received < kMsgs)
        {
            while (q_mgr_to_wkr->TryDequeue(cmd, seq, buf, out_len))
            {
                ++received;
                char rsp[4096];
                int rsp_len = snprintf(rsp, sizeof(rsp), "echo-%s-ack", buf);
                while (!q_wkr_to_mgr->TryEnqueue(cmd + 1, seq, rsp, static_cast<uint32_t>(rsp_len)))
                {
                    while (q_mgr_to_wkr->TryDequeue(cmd, seq, buf, out_len))
                    {
                        ++received;
                        rsp_len = snprintf(rsp, sizeof(rsp), "echo-%s-ack", buf);
                        if (q_wkr_to_mgr->TryEnqueue(cmd + 1, seq, rsp, static_cast<uint32_t>(rsp_len)))
                            break;
                    }
                }
                ShmRingQueue::NotifyEventFd(efd_wkr_to_mgr);
            }

            uint64_t ev;
            ssize_t ret = read(efd_mgr_to_wkr, &ev, sizeof(ev));
            if (ret > 0) {} // consumed
            else if (errno != EAGAIN) break;
        }

        while (!q_wkr_to_mgr->TryEnqueue(0xFFFF, static_cast<uint32_t>(received), "done", 4)) {}
        ShmRingQueue::NotifyEventFd(efd_wkr_to_mgr);

        ShmRingQueue::Destroy(q_mgr_to_wkr);
        ShmRingQueue::Destroy(q_wkr_to_mgr);
        ShmRingQueue::CloseEventFd(efd_mgr_to_wkr);
        ShmRingQueue::CloseEventFd(efd_wkr_to_mgr);
        _exit(0);
    }
    else
    {
        // ====== Parent: simulates Manager ======
        close(efd_mgr_to_wkr);

        for (uint32_t i = 0; i < kMsgs; ++i)
        {
            char body[256];
            snprintf(body, sizeof(body), "req-%u", i);
            while (!q_mgr_to_wkr->TryEnqueue(i % 500, i, body, static_cast<uint32_t>(strlen(body))))
            {
                uint32_t cmd, seq, out_len;
                char rsp[4096];
                while (q_wkr_to_mgr->TryDequeue(cmd, seq, rsp, out_len)) {}
            }
            ShmRingQueue::NotifyEventFd(efd_mgr_to_wkr);
        }

        uint64_t responses = 0;
        uint32_t cmd, seq, out_len;
        char rsp[4096];
        while (responses < kMsgs + 1)
        {
            while (q_wkr_to_mgr->TryDequeue(cmd, seq, rsp, out_len))
            {
                ++responses;
                if (cmd == 0xFFFF)
                {
                    EXPECT_EQ(seq, kMsgs);  // all messages received by child
                    responses = kMsgs + 1;
                    break;
                }
                EXPECT_GT(out_len, 5u);
            }

            uint64_t ev;
            ssize_t ret = read(efd_wkr_to_mgr, &ev, sizeof(ev));
            if (ret < 0 && errno != EAGAIN) break;
        }

        EXPECT_EQ(responses, kMsgs + 1u);

        int status;
        waitpid(pid, &status, 0);
        EXPECT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);

        ShmRingQueue::Destroy(q_mgr_to_wkr);
        ShmRingQueue::Destroy(q_wkr_to_mgr);
        ShmRingQueue::CloseEventFd(efd_mgr_to_wkr);
        ShmRingQueue::CloseEventFd(efd_wkr_to_mgr);
    }
}

TEST(ShmRingQueueE2E, FallbackWhenQueueFull)
{
    constexpr uint32_t kSlots = 2;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, kSlotSize);
    ASSERT_NE(q, nullptr);

    char body[1024] = {};
    EXPECT_TRUE(q->TryEnqueue(1, 100, body, sizeof(body)));
    EXPECT_TRUE(q->TryEnqueue(2, 200, body, sizeof(body)));
    EXPECT_TRUE(q->IsFull());
    EXPECT_FALSE(q->TryEnqueue(3, 300, body, sizeof(body)));

    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueE2E, WorkerRestartSimulation)
{
    constexpr uint32_t kSlots = 8;

    ShmRingQueue* q1 = ShmRingQueue::Create(kSlots, kSlotSize);
    ASSERT_NE(q1, nullptr);
    q1->TryEnqueue(1, 1, "old", 3);
    ShmRingQueue::Destroy(q1);

    ShmRingQueue* q2 = ShmRingQueue::Create(kSlots, kSlotSize);
    ASSERT_NE(q2, nullptr);
    EXPECT_TRUE(q2->IsEmpty());
    EXPECT_EQ(q2->Count(), 0u);

    q2->TryEnqueue(2, 2, "new", 3);
    uint32_t cmd, seq, out_len;
    char buf[256] = {};
    EXPECT_TRUE(q2->TryDequeue(cmd, seq, buf, out_len));
    EXPECT_EQ(cmd, 2u);
    EXPECT_EQ(out_len, 3u);
    EXPECT_EQ(memcmp(buf, "new", 3), 0);

    ShmRingQueue::Destroy(q2);
}

// ==========================================================================
// Performance benchmarks — SPSC throughput + latency
// ==========================================================================

TEST(ShmRingQueuePerf, Throughput_1M_Messages)
{
    constexpr uint32_t kSlots = 256;
    constexpr uint32_t kMsgs  = 1000000;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, ShmRingQueue::kDefaultSlotSize);
    ASSERT_NE(q, nullptr);

    std::atomic<bool> start{false};
    std::atomic<uint64_t> produced{0}, consumed{0};

    std::thread producer([&]() {
        char body[256] = {};
        while (!start.load(std::memory_order_acquire)) {}
        for (uint32_t i = 0; i < kMsgs; ++i) {
            while (!q->TryEnqueue(1, i, body, 128)) { std::this_thread::yield(); }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&]() {
        uint32_t cmd, seq, out_len; char buf[256];
        while (!start.load(std::memory_order_acquire)) {}
        uint64_t local = 0;
        while (local < kMsgs) {
            if (q->TryDequeue(cmd, seq, buf, out_len)) { ++local; }
            else { std::this_thread::yield(); }
        }
        consumed.store(local, std::memory_order_relaxed);
    });

    auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    auto t1 = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    double mps = (kMsgs / 1000000.0) / (ms / 1000.0);  // million msgs/sec
    EXPECT_EQ(produced.load(), kMsgs);
    EXPECT_EQ(consumed.load(), kMsgs);
    std::cout << "[perf] SPSC 1M msgs: " << ms << "ms, " << mps << " M msg/s" << std::endl;
    EXPECT_GT(mps, 0.5);  // at least 0.5M msg/s (conservative)
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueuePerf, Latency_SingleMessage)
{
    constexpr uint32_t kSlots = 8;
    constexpr uint32_t kRounds = 100000;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, ShmRingQueue::kDefaultSlotSize);
    ASSERT_NE(q, nullptr);

    auto t0 = std::chrono::steady_clock::now();
    char body[128] = {};
    for (uint32_t i = 0; i < kRounds; ++i) {
        while (!q->TryEnqueue(1, i, body, 64)) {}
        uint32_t cmd, seq, out_len; char buf[256];
        while (!q->TryDequeue(cmd, seq, buf, out_len)) {}
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double avg_ns = static_cast<double>(ns) / kRounds;
    std::cout << "[perf] single-msg latency: " << avg_ns << " ns/round (" << kRounds << " rounds)" << std::endl;
    EXPECT_LT(avg_ns, 5000.0);  // < 5us per round-trip
    ShmRingQueue::Destroy(q);
}

// ==========================================================================
// Performance benchmarks — 不同包大小吞吐+QPS
// ==========================================================================

static void RunThroughputTest(uint32_t bodySize, const char* label)
{
    constexpr uint32_t kSlots = 256;
    constexpr uint32_t kMsgs  = 500000;
    ShmRingQueue* q = ShmRingQueue::Create(kSlots, ShmRingQueue::kDefaultSlotSize);
    ASSERT_NE(q, nullptr);

    std::vector<char> body(bodySize, 'x');
    std::atomic<bool> start{false};
    std::atomic<uint64_t> produced{0}, consumed{0};

    std::thread producer([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (uint32_t i = 0; i < kMsgs; ++i) {
            while (!q->TryEnqueue(1, i, body.data(), bodySize)) {
                std::this_thread::yield();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&]() {
        uint32_t cmd, seq, out_len;
        std::vector<char> buf(bodySize + 256);
        while (!start.load(std::memory_order_acquire)) {}
        uint64_t local = 0;
        while (local < kMsgs) {
            if (q->TryDequeue(cmd, seq, buf.data(), out_len)) { ++local; }
            else { std::this_thread::yield(); }
        }
        consumed.store(local, std::memory_order_relaxed);
    });

    auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    auto t1 = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    double qps = (kMsgs / 1000000.0) / (ms / 1000.0);       // M msgs/sec
    double bw  = (qps * 1e6 * bodySize) / (1024*1024);      // MB/s

    EXPECT_EQ(produced.load(), kMsgs);
    EXPECT_EQ(consumed.load(), kMsgs);
    std::cout << "[perf] " << label << " " << bodySize << "B: "
              << ms << "ms, " << qps << " M QPS, " << bw << " MB/s" << std::endl;
    EXPECT_GT(qps, 1.0);  // > 1M QPS
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueuePerf, QPS_64B)   { RunThroughputTest(64,   "QPS"); }
TEST(ShmRingQueuePerf, QPS_256B)  { RunThroughputTest(256,  "QPS"); }
TEST(ShmRingQueuePerf, QPS_1K)    { RunThroughputTest(1024, "QPS"); }
TEST(ShmRingQueuePerf, QPS_4K)    { RunThroughputTest(4096, "QPS"); }

TEST(ShmRingQueueUnit, Destroy_Nullptr_NoCrash)
{
    ShmRingQueue::Destroy(nullptr);  // 应该不崩溃
    SUCCEED();
}

TEST(ShmRingQueueUnit, CloseEventFd_Invalid)
{
    { int invalid=-1; ShmRingQueue::CloseEventFd(invalid); }
    SUCCEED();
}

TEST(ShmRingQueueUnit, EventFd_SemaphoreMode)
{
    int efd = ShmRingQueue::CreateEventFd();
    ASSERT_GE(efd, 0);
    // 信号量模式: 写2次, 读2次(每次-1)
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    write(efd, &val, sizeof(val));
    uint64_t out;
    EXPECT_EQ(read(efd, &out, sizeof(out)), (ssize_t)sizeof(out));
    EXPECT_EQ(out, 1u);
    EXPECT_EQ(read(efd, &out, sizeof(out)), (ssize_t)sizeof(out));
    EXPECT_EQ(out, 1u);
    ShmRingQueue::CloseEventFd(efd);
}

TEST(ShmRingQueueUnit, MultipleCreateDestroy)
{
    // 反复创建销毁, 验证无泄漏(手工验证 valgrind)
    for (int i = 0; i < 100; ++i) {
        ShmRingQueue* q = ShmRingQueue::Create(32, 4096);
        ASSERT_NE(q, nullptr);
        q->TryEnqueue(i, 100, "ok", 2);
        ShmRingQueue::Destroy(q);
    }
    SUCCEED();
}

TEST(ShmRingQueueUnit, MaxBodySize)
{
    ShmRingQueue* q = ShmRingQueue::Create(8, 4096);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->MaxBodySize(), 4096u);
    ShmRingQueue::Destroy(q);
}

TEST(ShmRingQueueUnit, Count_TracksMessages)
{
    ShmRingQueue* q = ShmRingQueue::Create(8, 4096);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->Count(), 0u);
    const char* msg = "test";
    EXPECT_TRUE(q->TryEnqueue(1, 1, msg, 4));
    EXPECT_EQ(q->Count(), 1u);
    EXPECT_TRUE(q->TryEnqueue(2, 2, msg, 4));
    EXPECT_EQ(q->Count(), 2u);
    uint32_t cmd, seq, out_len; char buf[256];
    EXPECT_TRUE(q->TryDequeue(cmd, seq, buf, out_len));
    EXPECT_EQ(q->Count(), 1u);
    ShmRingQueue::Destroy(q);
}
