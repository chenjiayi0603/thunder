#include <gtest/gtest.h>
#include "labor/EvIoBackend.hpp"
#include "libev/ev.h"
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <memory>
#include "util/CBuffer.hpp"

using namespace net;

static int g_done = 0;
static void CompletionCb(int, uint32_t, IoOp, int result, void*) {
    g_done++;
    if (g_done >= 2) { /* read+write both done */ }
}

struct BenchResult {
    double qps_m;    // M ops/sec
    double mb_s;     // MB/sec
    double ns_op;    // ns per op
};

static BenchResult BenchBackend(IoBackend* be, struct ev_loop* loop, int rounds, int pktSize)
{
    g_done = 0;
    be->Init(loop, CompletionCb, nullptr);

    // Pre-create pipe pairs
    const int N_PIPES = 100;
    int fds[N_PIPES][2];
    for (int i = 0; i < N_PIPES; ++i) pipe(fds[i]);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
        int p = i % N_PIPES;
        auto buf = std::make_shared<util::CBuffer>();
        buf->Write(std::string(pktSize, 'x').c_str(), pktSize);
        be->SubmitWrite(fds[p][1], buf, 1);
        auto rbuf = std::make_shared<util::CBuffer>();
        be->SubmitRead(fds[p][0], rbuf, 2);
        ev_run(loop, EVRUN_NOWAIT);
    }
    for (int i = 0; i < N_PIPES; ++i) {
        be->CancelFd(fds[i][0]); be->CancelFd(fds[i][1]);
        close(fds[i][0]); close(fds[i][1]);
    }
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    BenchResult r;
    r.qps_m  = (rounds / 1e6) / sec;
    r.mb_s   = (r.qps_m * 1e6 * pktSize) / (1024*1024);
    r.ns_op  = (sec / rounds) * 1e9;
    be->Destroy();
    return r;
}

TEST(BackendBench, EvIoBackend_Perf)
{
    auto* loop = ev_loop_new(EVFLAG_AUTO);
    EvIoBackend be;
    for (int sz : {64, 256, 1024}) {
        auto r = BenchBackend(&be, loop, 50000, sz);
        std::cout << "[bench] EvIo " << sz << "B: " << r.qps_m << " M/s, "
                  << r.mb_s << " MB/s, " << r.ns_op << " ns/op" << std::endl;
    }
    ev_loop_destroy(loop);
    SUCCEED();
}

#ifdef THUNDER_IO_ASIO_URING
#include "labor/AsioUringIoBackend.hpp"
TEST(BackendBench, AsioUring_Perf)
{
    auto* loop = ev_loop_new(EVFLAG_AUTO);
    AsioUringIoBackend be;
    for (int sz : {64, 256, 1024}) {
        auto r = BenchBackend(&be, loop, 50000, sz);
        std::cout << "[bench] AsioUring " << sz << "B: " << r.qps_m << " M/s, "
                  << r.mb_s << " MB/s, " << r.ns_op << " ns/op" << std::endl;
    }
    ev_loop_destroy(loop);
    SUCCEED();
}
#endif
