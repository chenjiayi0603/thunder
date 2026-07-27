/*
 * Work Stealing vs 单全局队列 性能对比基准
 *
 * 场景设计说明：
 *   Thunder 的 commit() 由单一事件循环线程调用（per-worker 进程单线程 coroutine）。
 *   WorkerDeque::push() 设计为「单 tail 写者」，不支持并发 push()。
 *   因此本 bench 只用 1 个 producer 线程（模拟事件循环），
 *   workers 数量对应线程池 worker 数量。
 *
 * 对比方案：
 *   LF  —— util::threadpool（moodycamel 单全局队列，当前生产实现）
 *   WS  —— util::WorkStealingPool（_submit_deques + _local_deques + global_q，#109）
 *
 * 测试场景：
 *   1P-1C   最简基线（vs LF 应相近）
 *   1P-2C   2 worker
 *   1P-4C   Thunder 典型配置
 *   1P-8C   8 worker
 *   1P-16C  最大 worker 数
 *   task=10μs   有耗时任务，负载均衡效果
 *   task=100μs  长任务，steal 能否拉平不均衡
 */

#include <gtest/gtest.h>
#include "thread/threadpool.h"
#include "thread/work_stealing_pool.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

static double ns_per_op(Clock::time_point t0, Clock::time_point t1, long long n)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(ns) / n;
}

static double mops(Clock::time_point t0, Clock::time_point t1, long long n)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(n) / ns * 1000.0;
}

static void spin_us(int us)
{
    auto deadline = Clock::now() + std::chrono::microseconds(us);
    while (Clock::now() < deadline) {}
}

// ── bench runner：单生产者（模拟 Thunder 事件循环） ─────────────────
struct Result { double ns; double mops_val; };

template<typename Pool>
Result run_once(int workers, long long total_tasks, int task_us)
{
    std::atomic<long long> done{0};

    Pool pool(static_cast<unsigned short>(workers));

    auto t0 = Clock::now();

    // 单生产者：事件循环线程顺序 commit，队列满时背压重试
    for (long long j = 0; j < total_tasks; ++j)
    {
        pool.commit([&done, task_us]{
            if (task_us > 0) spin_us(task_us);
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (done.load(std::memory_order_acquire) < total_tasks)
        std::this_thread::yield();

    auto t1 = Clock::now();
    return { ns_per_op(t0, t1, total_tasks), mops(t0, t1, total_tasks) };
}

template<typename Pool>
Result run_median(int workers, long long total_tasks, int task_us, int runs = 3)
{
    std::vector<double> ns_v(runs), mops_v(runs);
    for (int r = 0; r < runs; ++r) {
        auto res = run_once<Pool>(workers, total_tasks, task_us);
        ns_v[r]   = res.ns;
        mops_v[r] = res.mops_val;
    }
    std::sort(ns_v.begin(), ns_v.end());
    std::sort(mops_v.begin(), mops_v.end());
    return { ns_v[runs / 2], mops_v[runs / 2] };
}

// ── 场景定义 ──────────────────────────────────────────────────────
struct Scene {
    const char* name;
    int  workers;
    long long total_tasks;
    int  task_us;
};

static const Scene kScenes[] = {
    { "1P-1C  (最简基线)",         1,   500000, 0   },
    { "1P-2C  (2 worker)",         2,   500000, 0   },
    { "1P-4C  (Thunder 典型)",     4,   500000, 0   },
    { "1P-8C  (8 worker)",         8,   500000, 0   },
    { "1P-16C (16 worker)",       16,   500000, 0   },
    { "1P-4C  (task=10us)",        4,    10000, 10  },
    { "1P-4C  (task=100us)",       4,     1000, 100 },
};

// ── 主测试 ────────────────────────────────────────────────────────
TEST(BenchWorkStealing, Compare)
{
    printf("\n%-22s  %10s  %10s  %10s  %10s  %8s\n",
           "场景", "LF ns/op", "WS ns/op", "LF Mop/s", "WS Mop/s", "加速比");
    printf("%s\n", std::string(80, '-').c_str());

    for (const auto& s : kScenes)
    {
        Result lf{}, ws{};
        try { lf = run_median<util::threadpool>      (s.workers, s.total_tasks, s.task_us); }
        catch (const std::exception& e) { printf("LF EXCEPTION: %s\n", e.what()); FAIL(); }
        try { ws = run_median<util::WorkStealingPool>(s.workers, s.total_tasks, s.task_us); }
        catch (const std::exception& e) { printf("WS EXCEPTION: %s\n", e.what()); FAIL(); }

        double speedup = lf.ns / ws.ns;
        const char* tag = (speedup >= 1.0) ? "+" : "-";

        printf("%-22s  %10.1f  %10.1f  %10.2f  %10.2f  %6.2fx %s\n",
               s.name, lf.ns, ws.ns, lf.mops_val, ws.mops_val, speedup, tag);

        EXPECT_GT(lf.mops_val, 0.0);
        EXPECT_GT(ws.mops_val, 0.0);
    }

    printf("\nLF = util::threadpool (moodycamel 单全局队列)\n");
    printf("WS = util::WorkStealingPool (_submit_deques + _local_deques + global_q)\n");
    printf("注：commit() 由单线程调用（匹配 Thunder 事件循环单线程设计）\n\n");
}
