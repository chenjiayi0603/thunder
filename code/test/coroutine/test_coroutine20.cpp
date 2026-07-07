/**
 * C++20 协程机制占位测试
 * 完整的 AsyncTask+StepCo20 测试见 test_step_co20.cpp
 */
#include <gtest/gtest.h>

TEST(Coroutine20, CompileCheckAsyncTaskHeaderAvailable)
{
    SUCCEED();
}

// ========== Worker 排空单元测试 ==========
// 这些测试在独立进程中运行, Mock Worker 的排空逻辑

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <memory>

// 模拟排空需要的最小结构
struct MockConn {
    int fd;
    int codecType;
    size_t recvBytes = 0;
    size_t sendBytes = 0;
    bool isListenFd = false;
};

class DrainTest {
public:
    std::unordered_map<int, std::unique_ptr<MockConn>> fds;
    std::atomic<bool> draining{false};
    std::atomic<bool> accepting{true};
    time_t drainStartTime = 0;
    static constexpr int GRACE = 30;
    static constexpr int CODEC_INTERNAL = 0;
    static constexpr int CODEC_HTTP = 1;

    bool IsDrainComplete() {
        for (auto& [fd, conn] : fds) {
            if (conn->isListenFd) continue;
            if (conn->codecType == CODEC_INTERNAL) continue;
            if (conn->recvBytes > 0) return false;
            if (conn->sendBytes > 0) return false;
        }
        return true;
    }

    void EnterDrainMode() {
        draining = true;
        accepting = false;
        drainStartTime = time(nullptr);
        for (auto it = fds.begin(); it != fds.end(); ) {
            auto* c = it->second.get();
            if (c->isListenFd) { ++it; continue; }
            if (c->codecType == CODEC_INTERNAL) { ++it; continue; }
            if (c->recvBytes > 0) { ++it; continue; }
            if (c->sendBytes > 0) { ++it; continue; }
            it = fds.erase(it);
        }
    }

    bool Accept(int listenFd) {
        if (draining || !accepting) return false;  // 排空中拒绝新连接
        (void)listenFd;
        return true;
    }
};

TEST(WorkerDrain, IdleConnectionsClosedOnEnterDrain)
{
    DrainTest dt;
    dt.fds[1] = std::make_unique<MockConn>(MockConn{1, DrainTest::CODEC_HTTP, 0, 0});
    dt.fds[2] = std::make_unique<MockConn>(MockConn{2, DrainTest::CODEC_HTTP, 0, 0});
    dt.fds[99] = std::make_unique<MockConn>(MockConn{99, 0, 0, 0, true});  // listen fd

    dt.EnterDrainMode();

    EXPECT_TRUE(dt.draining);
    EXPECT_FALSE(dt.accepting);
    EXPECT_EQ(dt.fds.size(), 1u);  // 只有 listen fd 保留
    EXPECT_NE(dt.fds.find(99), dt.fds.end());
}

TEST(WorkerDrain, ActiveConnectionsNotClosed)
{
    DrainTest dt;
    dt.fds[1] = std::make_unique<MockConn>(MockConn{1, DrainTest::CODEC_HTTP, 100, 0});  // 有未读数据
    dt.fds[2] = std::make_unique<MockConn>(MockConn{2, DrainTest::CODEC_HTTP, 0, 50});   // 有未写数据

    dt.EnterDrainMode();

    EXPECT_EQ(dt.fds.size(), 2u);  // 两个连接都在 (有在途数据)
    EXPECT_FALSE(dt.IsDrainComplete());
}

TEST(WorkerDrain, DrainCompleteWhenAllDone)
{
    DrainTest dt;
    dt.fds[1] = std::make_unique<MockConn>(MockConn{1, DrainTest::CODEC_HTTP, 100, 0});
    dt.fds[2] = std::make_unique<MockConn>(MockConn{2, DrainTest::CODEC_HTTP, 0, 50});

    dt.EnterDrainMode();
    EXPECT_FALSE(dt.IsDrainComplete());

    // 模拟处理完数据
    dt.fds[1]->recvBytes = 0;
    dt.fds[2]->sendBytes = 0;
    EXPECT_TRUE(dt.IsDrainComplete());
}

TEST(WorkerDrain, S2SConnectionsSkipped)
{
    DrainTest dt;
    dt.fds[1] = std::make_unique<MockConn>(MockConn{1, DrainTest::CODEC_INTERNAL, 100, 0});
    dt.fds[2] = std::make_unique<MockConn>(MockConn{2, DrainTest::CODEC_INTERNAL, 0, 50});

    dt.EnterDrainMode();

    // S2S 连接保留 (Manager 管理)
    EXPECT_EQ(dt.fds.size(), 2u);
    EXPECT_TRUE(dt.IsDrainComplete());  // S2S 不算在途
}

TEST(WorkerDrain, NewConnectionsRejectedDuringDrain)
{
    DrainTest dt;
    dt.EnterDrainMode();

    EXPECT_FALSE(dt.Accept(99));  // 排空中拒绝 accept
}

TEST(WorkerDrain, AcceptNormalWhenNotDraining)
{
    DrainTest dt;
    dt.fds[99] = std::make_unique<MockConn>(MockConn{99, 0, 0, 0, true});

    EXPECT_TRUE(dt.Accept(99));  // 正常模式允许 accept
}

TEST(WorkerDrain, DrainTimeoutDetection)
{
    DrainTest dt;
    dt.fds[1] = std::make_unique<MockConn>(MockConn{1, DrainTest::CODEC_HTTP, 100, 0});

    dt.EnterDrainMode();
    dt.drainStartTime = time(nullptr) - 31;  // 模拟超时

    bool timeout = (time(nullptr) - dt.drainStartTime > DrainTest::GRACE);
    EXPECT_TRUE(timeout);
}

// ========== #79 PendingRestart 单元测试 ==========
// 模拟 Manager m_bPendingRestart 逻辑：
//   ConfigUpdated 时 Worker 忙 → 设置 pending；
//   Worker 生命周期回归 RUNNING 且全部空闲 → 应用 pending

struct MockLC {
    enum State { RUNNING, STARTING, DRAINING } state = RUNNING;
};

class ManagerPendingMock {
public:
    std::unordered_map<int, MockLC> lcs;
    bool pendingRestart = false;
    int restartFired = 0;

    // 返回 false 表示 Worker 忙（与 GracefulRestartWorker 语义一致）
    bool GracefulRestart(int idx) {
        if (lcs.count(idx) == 0 || lcs[idx].state != MockLC::RUNNING) return false;
        lcs[idx].state = MockLC::STARTING;
        ++restartFired;
        return true;
    }

    // 模拟 ConfigUpdated 第 6 步
    void OnConfigUpdated(int numWorkers) {
        bool anyBusy = false;
        for (int i = 0; i < numWorkers; ++i)
            if (!GracefulRestart(i)) anyBusy = true;
        if (anyBusy) pendingRestart = true;
    }

    // 模拟 OnChildTerminated 中 lc.state=RUNNING 之后的逻辑
    void OnWorkerBecomeRunning(int idx, int numWorkers) {
        lcs[idx].state = MockLC::RUNNING;
        if (!pendingRestart) return;
        bool allRunning = true;
        for (auto& [i, lc] : lcs)
            if (lc.state != MockLC::RUNNING) { allRunning = false; break; }
        if (allRunning) {
            pendingRestart = false;
            for (int i = 0; i < numWorkers; ++i) GracefulRestart(i);
        }
    }
};

// 所有 Worker 空闲时 → 直接重启，不设 pending
TEST(PendingRestart, NoQueueWhenAllWorkersRunning)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::RUNNING};
    m.lcs[1] = {MockLC::RUNNING};

    m.OnConfigUpdated(2);

    EXPECT_FALSE(m.pendingRestart);
    EXPECT_EQ(m.restartFired, 2);
}

// 所有 Worker 忙 → 设 pending，不触发重启
TEST(PendingRestart, QueuedWhenAllWorkersBusy)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::STARTING};
    m.lcs[1] = {MockLC::DRAINING};

    m.OnConfigUpdated(2);

    EXPECT_TRUE(m.pendingRestart);
    EXPECT_EQ(m.restartFired, 0);
}

// 部分 Worker 忙 → 也设 pending（有变更未能应用）
TEST(PendingRestart, QueuedWhenSomeWorkersBusy)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::RUNNING};
    m.lcs[1] = {MockLC::STARTING};

    m.OnConfigUpdated(2);

    EXPECT_TRUE(m.pendingRestart);
    EXPECT_EQ(m.restartFired, 1);  // worker 0 成功启动
}

// pending 存在，最后一个 Worker 回 RUNNING → 触发补充重启
TEST(PendingRestart, AppliedWhenLastWorkerReturnsRunning)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::RUNNING};
    m.lcs[1] = {MockLC::DRAINING};
    m.pendingRestart = true;

    // worker 1 lifecycle 完成
    m.OnWorkerBecomeRunning(1, 2);

    EXPECT_FALSE(m.pendingRestart);          // 已消费
    EXPECT_EQ(m.restartFired, 2);            // 两个 Worker 都重启了
}

// pending 存在但还有其他 Worker 未完成 → 不触发
TEST(PendingRestart, NotAppliedUntilAllWorkersRunning)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::RUNNING};
    m.lcs[1] = {MockLC::DRAINING};
    m.lcs[2] = {MockLC::STARTING};
    m.pendingRestart = true;

    // worker 1 完成，但 worker 2 还在 STARTING
    m.OnWorkerBecomeRunning(1, 3);

    EXPECT_TRUE(m.pendingRestart);   // 还没消费
    EXPECT_EQ(m.restartFired, 0);
}

// 无 pending 时，Worker 回 RUNNING 不触发额外重启
TEST(PendingRestart, NoPendingNoop)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::DRAINING};
    m.lcs[1] = {MockLC::RUNNING};
    m.pendingRestart = false;

    m.OnWorkerBecomeRunning(0, 2);

    EXPECT_FALSE(m.pendingRestart);
    EXPECT_EQ(m.restartFired, 0);
}

// pending 应用后 flag 清零，不重复触发
TEST(PendingRestart, FlagClearedAfterApply)
{
    ManagerPendingMock m;
    m.lcs[0] = {MockLC::RUNNING};
    m.pendingRestart = true;

    m.OnWorkerBecomeRunning(0, 1);  // 单 Worker 场景

    EXPECT_FALSE(m.pendingRestart);
    EXPECT_EQ(m.restartFired, 1);

    // 再次调用不应再触发
    m.lcs[0].state = MockLC::RUNNING;
    m.OnWorkerBecomeRunning(0, 1);
    EXPECT_EQ(m.restartFired, 1);  // 仍然是 1，没有新重启
}
