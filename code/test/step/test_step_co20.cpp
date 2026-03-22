#include "step/StepCo20.hpp"
#include "step/StepCo20Func.hpp"

#include <gtest/gtest.h>

namespace
{

class TestableStepCo20OneAwait : public net::StepCo20
{
public:
    bool awaiterResult = false;

    net::Task<> CoroutineMain() override
    {
        net::HttpRespAwaiter awaiter(this);
        awaiterResult = co_await awaiter;
        co_return;
    }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

    void OnCoroutineError(int /*iErrno*/, const std::string& /*strErrMsg*/) override {}
};

class TestableStepCo20TwoAwait : public net::StepCo20
{
public:
    int phase = 0;

    net::Task<> CoroutineMain() override
    {
        {
            net::HttpRespAwaiter a1(this);
            co_await a1;
        }
        ++phase;
        {
            net::HttpRespAwaiter a2(this);
            co_await a2;
        }
        ++phase;
        co_return;
    }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

    void OnCoroutineError(int /*iErrno*/, const std::string& /*strErrMsg*/) override {}
};

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

    net::StepCo20Func step(shell, req, [&](net::StepCo20&) -> net::Task<> {
        ran = true;
        co_return;
    });

    (void)step.Emit();
    EXPECT_TRUE(ran);
}

TEST(StepCo20Func, lambda_single_await)
{
    bool afterAwait = false;
    net::tagMsgShell shell{};
    HttpMsg req;

    net::StepCo20Func step(shell, req, [&](net::StepCo20& self) -> net::Task<> {
        net::HttpRespAwaiter awaiter(&self);
        (void)co_await awaiter;
        afterAwait = true;
        co_return;
    });

    ASSERT_EQ(step.Emit(), net::STATUS_CMD_RUNNING);
    EXPECT_FALSE(afterAwait);

    HttpMsg rsp;
    rsp.set_type(HTTP_RESPONSE);
    rsp.set_status_code(200);

    EXPECT_EQ(step.Callback(shell, rsp), net::STATUS_CMD_COMPLETED);
    EXPECT_TRUE(afterAwait);
}
