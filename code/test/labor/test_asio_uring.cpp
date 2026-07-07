#include <gtest/gtest.h>
#include "util/CBuffer.hpp"

#ifdef THUNDER_IO_ASIO_URING
#include "labor/io/AsioUringIoBackend.hpp"
#include "libev/ev.h"
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

// 回调计数
static int g_completed = 0;
static void CompletionCb(int fd, uint32_t seq, net::IoOp op, int result, void*) {
    g_completed++;
}

class AsioUringTest : public ::testing::Test {
protected:
    struct ev_loop* loop = nullptr;
    net::AsioUringIoBackend backend;

    void SetUp() override {
        loop = ev_loop_new(EVFLAG_AUTO);
        g_completed = 0;
    }
    void TearDown() override {
        backend.Destroy();
        if (loop) ev_loop_destroy(loop);
    }
};

TEST_F(AsioUringTest, InitAndDestroy)
{
    EXPECT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    EXPECT_STREQ(backend.Name(), "asio_uring");
}

TEST_F(AsioUringTest, CreateListenSocket)
{
    ASSERT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    int fd = backend.CreateListenSocket("127.0.0.1", 19999, false, 5);
    EXPECT_GE(fd, 0);
    if (fd >= 0) backend.CloseFd(fd);
}

TEST_F(AsioUringTest, SubmitRead_InvalidFd)
{
    ASSERT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    auto buf = std::make_shared<util::CBuffer>();
    EXPECT_TRUE(backend.SubmitRead(-1, buf, 1));  // io_uring 延后验证fd
}

TEST_F(AsioUringTest, CancelFd_NotFound)
{
    ASSERT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    backend.CancelFd(99999);  // 不崩溃即可
}

TEST_F(AsioUringTest, HasPending_NotExist)
{
    ASSERT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    EXPECT_FALSE(backend.HasPending(99999));
}

TEST_F(AsioUringTest, SubmitReadWrite_Pipe)
{
    ASSERT_TRUE(backend.Init(loop, CompletionCb, nullptr));
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    auto buf = std::make_shared<util::CBuffer>();
    buf->Write("hello", 5);

    // 写端: 提交写
    EXPECT_TRUE(backend.SubmitWrite(fds[1], buf, 1));
    // 读写完成后 need epoll loop 收割, 简化测试只验证提交成功
    EXPECT_TRUE(backend.HasPending(fds[1]));

    backend.CancelFd(fds[0]);
    backend.CancelFd(fds[1]);
    close(fds[0]); close(fds[1]);
}

#else  // !THUNDER_IO_ASIO_URING

TEST(AsioUringDisabled, Skipped)
{
    GTEST_SKIP() << "io_uring 未启用 (cmake -DENABLE_ASIO_URING=ON)";
}

#endif
