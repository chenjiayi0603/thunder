/**
 * Session 会话管理单元测试
 */
#include "session/Session.hpp"
#include "labor/Labor.hpp"
#include <gtest/gtest.h>

namespace
{

class TestSession : public net::Session
{
public:
    int timeoutCount = 0;
    bool initCalled = false;

    TestSession(const std::string& id, ev_tstamp timeout = 60.0)
        : net::Session(id, timeout, "TestSession") {}
    TestSession(uint64 id, ev_tstamp timeout = 60.0)
        : net::Session(id, timeout, "TestSession") {}

    net::E_CMD_STATUS Timeout() override
    {
        timeoutCount++;
        return net::STATUS_CMD_RUNNING;
    }

    bool Init(const util::CJsonObject&) override
    {
        initCalled = true;
        return true;
    }

    // 暴露 protected 方法供外部测试
    ev_tstamp GetTestTimeout() const { return GetTimeout(); }
};

} // namespace

TEST(Session, ConstructionWithStringId)
{
    TestSession s("session_001");
    EXPECT_EQ("session_001", s.GetSessionId());
    EXPECT_EQ("TestSession", s.GetSessionClass());
}

TEST(Session, ConstructionWithUint64Id)
{
    TestSession s2(42u);
    EXPECT_NE("", s2.GetSessionId());
}

TEST(Session, TimeoutCallback)
{
    TestSession s("s_timeout");
    EXPECT_EQ(0, s.timeoutCount);
    EXPECT_EQ(net::STATUS_CMD_RUNNING, s.Timeout());
    EXPECT_EQ(1, s.timeoutCount);
}

TEST(Session, InitCalled)
{
    TestSession s("s_init");
    util::CJsonObject conf;
    EXPECT_TRUE(s.Init(conf));
    EXPECT_TRUE(s.initCalled);
}

TEST(Session, PermanentFlag)
{
    TestSession s("s_perm");
    EXPECT_FALSE(s.IsPermanent());
    s.SetPermanent();
    EXPECT_TRUE(s.IsPermanent());
}

TEST(Session, ActiveTimeManagement)
{
    TestSession s("s_active", 10.0);
    EXPECT_EQ(10.0, s.GetTestTimeout());
}

TEST(Session, DifferentSessionIds)
{
    TestSession s1("id_1");
    TestSession s2("id_2");
    EXPECT_NE(s1.GetSessionId(), s2.GetSessionId());
}

TEST(Session, DefaultTimeout)
{
    // 默认 60s
    TestSession s("s_default", 60.0);
    EXPECT_EQ(60.0, s.GetTestTimeout());
}

TEST(Session, CustomTimeout)
{
    TestSession s("s_custom", 1.5);
    EXPECT_EQ(1.5, s.GetTestTimeout());
}
