#include "coro/StepCo20.hpp"
#include "coro/StepCo20Func.hpp"

#include <gtest/gtest.h>

namespace
{

class TestableStepCo20OneAwait;
class TestableStepCo20TwoAwait;

net::AsyncTask OneAwaitCo(net::StepCo20& st);
net::AsyncTask TwoAwaitCo(net::StepCo20& st);

class TestableStepCo20OneAwait : public net::StepCo20
{
public:
    bool awaiterResult = false;

    net::AsyncTask StepAsync() override { return OneAwaitCo(*this); }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

    void OnCoroutineError(int /*iErrno*/, const std::string& /*strErrMsg*/) override {}
};

class TestableStepCo20TwoAwait : public net::StepCo20
{
public:
    int phase = 0;

    net::AsyncTask StepAsync() override { return TwoAwaitCo(*this); }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

    void OnCoroutineError(int /*iErrno*/, const std::string& /*strErrMsg*/) override {}
};

net::AsyncTask OneAwaitCo(net::StepCo20& st)
{
    auto& self = static_cast<TestableStepCo20OneAwait&>(st);
    net::HttpRespAwaiter awaiter(&self);
    self.awaiterResult = co_await awaiter;
    co_return;
}

net::AsyncTask TwoAwaitCo(net::StepCo20& st)
{
    auto& self = static_cast<TestableStepCo20TwoAwait&>(st);
    {
        net::HttpRespAwaiter a1(&self);
        co_await a1;
    }
    ++self.phase;
    {
        net::HttpRespAwaiter a2(&self);
        co_await a2;
    }
    ++self.phase;
    co_return;
}

net::AsyncTask FuncExecCo(net::StepCo20& /*st*/, bool* ranOut)
{
    *ranOut = true;
    co_return;
}

net::AsyncTask SingleAwaitCo(net::StepCo20& st, bool* afterOut)
{
    net::HttpRespAwaiter awaiter(&st);
    (void)co_await awaiter;
    *afterOut = true;
    co_return;
}

} // namespace

TEST(StepCo20, HttpRespAwaiter_http_200_returns_true)
{
    TestableStepCo20OneAwait step;
    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(200);
    net::tagMsgShell shell{};

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
    EXPECT_TRUE(step.awaiterResult);
}

TEST(StepCo20, HttpRespAwaiter_http_404_returns_false)
{
    TestableStepCo20OneAwait step;
    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(404);
    net::tagMsgShell shell{};

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
    EXPECT_FALSE(step.awaiterResult);
}

TEST(StepCo20, HttpRespAwaiter_binary_callback_returns_true)
{
    TestableStepCo20OneAwait step;
    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);

    net::tagMsgShell shell{};
    MsgHead head;
    MsgBody body;

    EXPECT_EQ(step.Callback(shell, head, body), net::STATUS_CMD_COMPLETED);
    EXPECT_TRUE(step.awaiterResult);
}

TEST(StepCo20, StepCo20_Callback_returns_completed_when_done)
{
    TestableStepCo20OneAwait step;
    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(200);
    net::tagMsgShell shell{};

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
}

TEST(StepCo20, StepCo20_Callback_returns_running_when_suspended)
{
    TestableStepCo20TwoAwait step;
    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);
    EXPECT_EQ(step.phase, 0);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(200);
    net::tagMsgShell shell{};

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_RUNNING);
    EXPECT_EQ(step.phase, 1);

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
    EXPECT_EQ(step.phase, 2);
}

TEST(StepCo20Func, lambda_executes)
{
    bool ran = false;
    net::tagMsgShell shell{};
    HttpMsg req;

    net::StepCo20Func step(shell, req, [&ran](net::StepCo20& st) -> net::AsyncTask {
        return FuncExecCo(st, &ran);
    });

    (void)step.Emit();
    EXPECT_TRUE(ran);
}

TEST(StepCo20Func, lambda_single_await)
{
    bool afterAwait = false;
    net::tagMsgShell shell{};
    HttpMsg req;

    net::StepCo20Func step(shell, req, [&afterAwait](net::StepCo20& st) -> net::AsyncTask {
        return SingleAwaitCo(st, &afterAwait);
    });

    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);
    EXPECT_FALSE(afterAwait);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(200);

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
    EXPECT_TRUE(afterAwait);
}
