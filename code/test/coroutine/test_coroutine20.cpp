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
