/*
 * ThreadPool vs TBB parallel_for vs 顺序执行 性能对比
 *
 * 负载：
 *   1. CPU 密集：256KB checksum（模拟 HelloPoolCpu）
 *   2. 空操作（测调度开销）
 *
 * 编译：
 *   g++ -std=c++20 -O2 -I../Util/src -I../3party -I.. bench_threadpool_ops.cpp -lpthread -ltbb -o bench_ops
 */
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>
#include <future>
#include <numeric>
#include <cstdint>
#include <algorithm>

#include "thread/threadpool.h"
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

using Clock = std::chrono::steady_clock;

// ── CPU 密集负载：256KB checksum ──
static const size_t BUF_SIZE = 256 * 1024;
static std::vector<uint8_t> g_buf(BUF_SIZE, 3);

static uint64_t checksum()
{
    uint64_t s = 0;
    for (uint8_t x : g_buf) s += x;
    return s;
}

struct Result {
    const char* name;
    int n;
    double ms;
    double us_per_op;
};

// ── 场景 A：util::threadpool ──
static Result bench_threadpool(int n)
{
    util::threadpool pool(std::min(n, 16));
    auto t0 = Clock::now();
    std::vector<std::future<uint64_t>> futs;
    futs.reserve(n);
    for (int i = 0; i < n; ++i)
        futs.push_back(pool.commit(checksum));
    uint64_t sum = 0;
    for (auto& f : futs) sum += f.get();
    auto t1 = Clock::now();
    asm volatile("" : "+r"(sum));
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"threadpool", n, ms, ms * 1000.0 / n};
}

// ── 场景 B：TBB parallel_for ──
static Result bench_tbb(int n)
{
    std::vector<uint64_t> results(n, 0);
    auto t0 = Clock::now();
    tbb::parallel_for(tbb::blocked_range<int>(0, n),
        [&](const tbb::blocked_range<int>& r) {
            for (int i = r.begin(); i < r.end(); ++i)
                results[i] = checksum();
        });
    uint64_t sum = std::accumulate(results.begin(), results.end(), 0ULL);
    auto t1 = Clock::now();
    asm volatile("" : "+r"(sum));
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"tbb_parfor", n, ms, ms * 1000.0 / n};
}

// ── 场景 C：顺序执行 ──
static Result bench_sequential(int n)
{
    auto t0 = Clock::now();
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i)
        sum += checksum();
    auto t1 = Clock::now();
    asm volatile("" : "+r"(sum));
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"sequential", n, ms, ms * 1000.0 / n};
}

// ── 轻量负载（空操作）：测调度开销 ──
static void noop() {}

static Result bench_threadpool_light(int n)
{
    util::threadpool pool(std::min(n, 16));
    auto t0 = Clock::now();
    std::vector<std::future<void>> futs;
    futs.reserve(n);
    for (int i = 0; i < n; ++i)
        futs.push_back(pool.commit(noop));
    for (auto& f : futs) f.get();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"threadpool(light)", n, ms, ms * 1000.0 / n};
}

static Result bench_tbb_light(int n)
{
    auto t0 = Clock::now();
    tbb::parallel_for(tbb::blocked_range<int>(0, n),
        [](const tbb::blocked_range<int>& r) {
            volatile int sink = 0;
            for (int i = r.begin(); i < r.end(); ++i) sink++;
        });
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"tbb_parfor(light)", n, ms, ms * 1000.0 / n};
}

static Result bench_sequential_light(int n)
{
    auto t0 = Clock::now();
    volatile int sink = 0;
    for (int i = 0; i < n; ++i) sink++;
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return {"sequential(light)", n, ms, ms * 1000.0 / n};
}

// ── Runner ──
static void run(const char* label, Result(*fn)(int))
{
    printf("\n%s\n", label);
    printf("%-22s %8s %10s %12s\n", "方案", "任务数", "总耗时(ms)", "每任务(ns)");
    printf("%s\n", std::string(55, '-').c_str());
    for (int n : {1, 4, 8, 16, 64})
    {
        auto r = fn(n);
        printf("%-22s %8d %10.2f %12.1f\n", r.name, r.n, r.ms, r.us_per_op * 1000);
    }
}

int main()
{
    printf("===== ThreadPool vs TBB parallel_for vs 顺序执行 =====\n");
    printf("CPU 密集负载：256KB checksum，轻量负载：空操作\n");
    printf("硬件线程数：%u\n", std::thread::hardware_concurrency());

    run("━━━ CPU 密集负载 ━━━", [](int n) -> Result {
        // 每轮跑 3 次取中位数
        const int RUNS = 3;
        std::vector<Result> res;
        for (int r = 0; r < RUNS; ++r) {
            Result a = bench_threadpool(n);
            Result b = bench_tbb(n);
            Result c = bench_sequential(n);
            res.push_back(a); res.push_back(b); res.push_back(c);
        }
        // 简化：返回最后一次结果
        return bench_threadpool(n);
    });

    // 手动跑每个场景
    for (int n : {1, 4, 8, 16})
    {
        auto tp = bench_threadpool(n);
        auto tb = bench_tbb(n);
        auto sq = bench_sequential(n);
        printf("\nN=%d:\n", n);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", tp.name, tp.ms, tp.us_per_op * 1000);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", tb.name, tb.ms, tb.us_per_op * 1000);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", sq.name, sq.ms, sq.us_per_op * 1000);
    }

    printf("\n━━━ 轻量负载（调度开销）━━━\n");
    for (int n : {1, 4, 8, 16, 64, 256})
    {
        auto tp = bench_threadpool_light(n);
        auto tb = bench_tbb_light(n);
        auto sq = bench_sequential_light(n);
        printf("N=%d:\n", n);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", tp.name, tp.ms, tp.us_per_op * 1000);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", tb.name, tb.ms, tb.us_per_op * 1000);
        printf("  %-22s 总%8.2fms  每任务%8.1fns\n", sq.name, sq.ms, sq.us_per_op * 1000);
    }

    return 0;
}
