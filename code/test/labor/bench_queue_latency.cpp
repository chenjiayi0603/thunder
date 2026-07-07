/*
 * 队列纯开销 + 不同 payload 大小 + 端到端延迟分布 基准
 *
 * 对比：
 *   LF — util::threadpool（1 个 moodycamel::ConcurrentQueue，所有 worker 争同一队列头）
 *   WS — util::WorkStealingPool（_submit_deques + _local_deques + global_q）
 *
 * LF 结构：
 *   commit() → moodycamel::ConcurrentQueue (lock-free MPMC, 链表式 block 管理)
 *            → N 个 worker 全部 try_dequeue 同一个队列 head
 *            → 空时 yield() 自旋
 *
 * WS 结构：
 *   commit() → P2C 选 _submit_deques[i].push()
 *            → worker[i] dequeue() 或 thief steal_into() → local_deques
 *            → 空时 steal → global_q → cv.wait
 *
 * 测试变量：
 *   - payload 大小：0B, 64B, 256B, 1KB, 4KB（lambda 捕获的数据量）
 *   - worker 数量：1, 4, 8
 *
 * 测量指标：
 *   - 端到端延迟：commit 前 → worker 开始执行，逐任务记录，输出 P50/P99/avg
 *   - 吞吐：ns/op, Mop/s
 *   - 任务执行时间固定为 0（只测队列开销）
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
#include <cstring>

using Clock = std::chrono::steady_clock;

// ── 延迟统计 ────────────────────────────────────────────────────────
struct LatencyStats {
    double p50_ns, p99_ns, avg_ns, max_ns;
    double mops;
};

static LatencyStats compute(std::vector<long long>& latencies, long long total_ns) {
    std::sort(latencies.begin(), latencies.end());
    int n = (int)latencies.size();
    return {
        (double)latencies[n / 2],
        (double)latencies[(int)(n * 0.99)],
        (double)total_ns / n,
        (double)latencies.back(),
        (double)n / total_ns * 1000.0
    };
}

// ── bench runner：逐任务记录端到端延迟 ──────────────────────────────
template<typename Pool>
LatencyStats run_latency(int workers, int total_tasks, int payload_bytes)
{
    std::vector<long long> latencies(total_tasks);
    std::vector<char> payload(payload_bytes > 0 ? payload_bytes : 1, 'x');
    std::atomic<int> done{0};

    size_t maxq = static_cast<size_t>(total_tasks) * 2;
    if (maxq < 1024) maxq = 1024;
    Pool pool(static_cast<unsigned short>(workers), maxq);

    auto bench_start = Clock::now();

    for (int j = 0; j < total_tasks; ++j) {
        auto t0 = Clock::now();
        // payload 按值捕获——其大小直接影响 lambda/function 的内存分配
        pool.commit([&done, &latencies, j, t0, payload]() mutable {
            auto t1 = Clock::now();
            latencies[j] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (done.load(std::memory_order_acquire) < total_tasks)
        std::this_thread::yield();

    auto bench_end = Clock::now();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(bench_end - bench_start).count();

    auto stats = compute(latencies, total_ns);
    return stats;
}

// ── 场景定义 ────────────────────────────────────────────────────────
struct Scene {
    const char* name;
    int  workers;
    int  total_tasks;
    int  payload_bytes;
};

static const Scene kScenes[] = {
    // 不同 worker 数（payload=0）
    { "1 worker,   0B",  1,  20000, 0   },
    { "4 worker,   0B",  4,  20000, 0   },
    { "8 worker,   0B",  8,  20000, 0   },
    // 不同 payload 大小（4 worker）
    { "4 worker,  64B",  4,  10000, 64  },
    { "4 worker, 256B",  4,  10000, 256 },
    { "4 worker,  1KB",  4,  10000, 1024},
    { "4 worker,  4KB",  4,   5000, 4096},
};

// ── 主测试 ──────────────────────────────────────────────────────────
TEST(BenchQueueLatency, Compare)
{
    printf("\nLF = util::threadpool (1× moodycamel MPMC, N worker 争同一 head)\n");
    printf("WS = util::WorkStealingPool (submit_deques + local_deques + global_q)\n");
    printf("端到端延迟 = commit() 调用前 → worker 取出任务开始执行\n");
    printf("payload 按值捕获在 lambda 中\n\n");

    printf("%-16s  %4s  %8s  %8s  %8s  %8s\n",
           "场景", "池", "P50 ns", "P99 ns", "avg ns", "Mop/s");
    printf("%s\n", std::string(70, '-').c_str());

    for (const auto& s : kScenes)
    {
        LatencyStats lf{}, ws{};
        try { lf = run_latency<util::threadpool>      (s.workers, s.total_tasks, s.payload_bytes); }
        catch (const std::exception& e) { printf("LF EXCEPTION: %s\n", e.what()); continue; }
        try { ws = run_latency<util::WorkStealingPool>(s.workers, s.total_tasks, s.payload_bytes); }
        catch (const std::exception& e) { printf("WS EXCEPTION: %s\n", e.what()); continue; }

        double speedup_avg = lf.avg_ns / ws.avg_ns;

        printf("%-16s  %4s  %8.0f  %8.0f  %8.0f  %8.2f\n",
               s.name, "LF", lf.p50_ns, lf.p99_ns, lf.avg_ns, lf.mops);
        printf("%-16s  %4s  %8.0f  %8.0f  %8.0f  %8.2f\n",
               "", "WS", ws.p50_ns, ws.p99_ns, ws.avg_ns, ws.mops);
        printf("  → avg %.2fx  P50 %.2fx  P99 %.2fx\n\n",
               speedup_avg, lf.p50_ns / ws.p50_ns, lf.p99_ns / ws.p99_ns);
    }

    printf("注：commit() 单线程调用（匹配 Thunder 事件循环）\n");
    printf("    延迟 = commit() 入队 + 队列等待 + worker 出队，不含任务体执行\n\n");
}
