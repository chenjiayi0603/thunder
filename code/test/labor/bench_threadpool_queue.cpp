/*
 * ThreadPool 队列方案性能对比基准
 *
 * 测试场景：模拟 Thunder 的 offload 模式
 *   - 多个生产者线程同时 commit() 任务（对应多个协程同时发起 offload）
 *   - 固定数量消费者线程（对应线程池 worker）
 *   - 任务体：空操作（剔除任务本身耗时，只测队列吞吐）
 *
 * 对比方案：
 *   A. std::queue<Task> + std::mutex + std::condition_variable   （现有方案）
 *   B. moodycamel::ConcurrentQueue<Task> + std::atomic + wait    （候选方案）
 */

#include <gtest/gtest.h>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <vector>
#include <numeric>
#include <cstdio>
#include <sstream>

// moodycamel header（放在 3party/）
#include "../../3party/concurrentqueue.h"

// ─────────────────────────────────────────────
// 通用计时
// ─────────────────────────────────────────────
using Task = std::function<void()>;
using Clock = std::chrono::steady_clock;

static double ns_per_op(Clock::time_point t0, Clock::time_point t1, long long total_tasks)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(ns) / total_tasks;
}

static double throughput_mops(Clock::time_point t0, Clock::time_point t1, long long total_tasks)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(total_tasks) / ns * 1000.0; // M ops/s
}

// ─────────────────────────────────────────────
// 方案 A：std::queue + mutex + condvar
// ─────────────────────────────────────────────
struct MutexQueue
{
    std::queue<Task>        q;
    std::mutex              mu;
    std::condition_variable cv;
    std::atomic<bool>       stop{false};

    void push(Task t)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            q.push(std::move(t));
        }
        cv.notify_one();
    }

    // 返回 false 表示已停止且队列空
    bool pop(Task& out)
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this]{ return !q.empty() || stop.load(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop();
        return true;
    }

    void drain_stop()
    {
        stop.store(true);
        cv.notify_all();
    }
};

// ─────────────────────────────────────────────
// 方案 B：moodycamel::ConcurrentQueue（lock-free MPMC）
// ─────────────────────────────────────────────
struct LockFreeQueue
{
    moodycamel::ConcurrentQueue<Task> q;
    std::atomic<bool>                 stop{false};
    std::atomic<int>                  pending{0}; // 生产者已提交但未消费的计数

    void push(Task t)
    {
        pending.fetch_add(1, std::memory_order_relaxed);
        q.enqueue(std::move(t));
    }

    // 自旋等待直到有任务或停止
    bool pop(Task& out)
    {
        while (true)
        {
            if (q.try_dequeue(out))
            {
                pending.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            if (stop.load(std::memory_order_acquire) &&
                pending.load(std::memory_order_acquire) == 0)
                return false;
            // 短暂 yield，避免自旋烧满 CPU（Thunder 场景不是高实时延迟）
            std::this_thread::yield();
        }
    }

    void drain_stop() { stop.store(true, std::memory_order_release); }
};

// ─────────────────────────────────────────────
// 通用 bench runner
// ─────────────────────────────────────────────
struct BenchResult
{
    double ns_per_op;
    double mops;
};

template<typename Q>
BenchResult run_bench(int producers, int consumers, long long tasks_per_producer)
{
    Q queue;
    std::atomic<long long> consumed{0};
    const long long total = producers * tasks_per_producer;

    // 启动消费者
    std::vector<std::thread> workers;
    workers.reserve(consumers);
    for (int i = 0; i < consumers; ++i)
    {
        workers.emplace_back([&]{
            Task t;
            while (queue.pop(t))
            {
                t();
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 计时从生产者开始
    auto t0 = Clock::now();

    // 启动生产者
    std::vector<std::thread> prods;
    prods.reserve(producers);
    for (int i = 0; i < producers; ++i)
    {
        prods.emplace_back([&, tasks_per_producer]{
            for (long long j = 0; j < tasks_per_producer; ++j)
                queue.push([]{ /* no-op */ });
        });
    }
    for (auto& p : prods) p.join();

    // 等消费者处理完
    while (consumed.load(std::memory_order_acquire) < total)
        std::this_thread::yield();

    auto t1 = Clock::now();

    queue.drain_stop();
    for (auto& w : workers) w.join();

    return { ns_per_op(t0, t1, total), throughput_mops(t0, t1, total) };
}

// ─────────────────────────────────────────────
// 测试矩阵：场景 × 两种实现
// ─────────────────────────────────────────────
struct Scene
{
    const char* name;
    int producers;
    int consumers;
    long long tasks_per_producer;
};

static const Scene kScenes[] = {
    // Thunder 典型场景：4 个 Worker 线程并发 offload，4 个线程池线程消费
    { "4P-4C  (典型 offload)",   4,  4,  250000 },
    // 极端并发：16 个协程同时 commit
    { "16P-4C (高并发 commit)",  16, 4,  62500  },
    // 单生产者基线
    { "1P-4C  (单协程基线)",      1,  4,  1000000},
    // 生产消费对等
    { "8P-8C  (对等压力)",        8,  8,  125000 },
};

static void print_header()
{
    printf("\n%-22s  %10s  %10s  %10s  %10s  %8s\n",
           "场景", "Mutex ns/op", "LF ns/op", "Mutex Mop/s", "LF Mop/s", "加速比");
    printf("%s\n", std::string(80, '-').c_str());
}

TEST(BenchThreadpoolQueue, Compare)
{
    print_header();

    for (const auto& s : kScenes)
    {
        // 每个场景跑 3 次取中位数，排除首次 OS 调度抖动
        const int RUNS = 3;
        BenchResult ra[RUNS], rb[RUNS];
        for (int r = 0; r < RUNS; ++r)
        {
            ra[r] = run_bench<MutexQueue>  (s.producers, s.consumers, s.tasks_per_producer);
            rb[r] = run_bench<LockFreeQueue>(s.producers, s.consumers, s.tasks_per_producer);
        }

        // 取中位数
        auto median_ns = [&](BenchResult* arr) {
            double v[RUNS]; for (int i=0;i<RUNS;i++) v[i]=arr[i].ns_per_op;
            std::sort(v,v+RUNS); return v[RUNS/2];
        };
        auto median_mops = [&](BenchResult* arr) {
            double v[RUNS]; for (int i=0;i<RUNS;i++) v[i]=arr[i].mops;
            std::sort(v,v+RUNS); return v[RUNS/2];
        };

        double a_ns   = median_ns(ra),   b_ns   = median_ns(rb);
        double a_mops = median_mops(ra), b_mops = median_mops(rb);
        double speedup = a_ns / b_ns;

        printf("%-22s  %10.1f  %10.1f  %10.2f  %10.2f  %7.2fx\n",
               s.name, a_ns, b_ns, a_mops, b_mops, speedup);

        // 合理性检查：两种方案都能跑完
        EXPECT_GT(a_mops, 0.0);
        EXPECT_GT(b_mops, 0.0);
    }
    printf("\n说明：LF = moodycamel::ConcurrentQueue（lock-free MPMC）\n\n");
}
