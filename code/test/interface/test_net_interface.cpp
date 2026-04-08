#include "Interface.hpp"
#include "coro/StepCo20Func.hpp"
#include "labor/Labor.hpp"
#include "step/MysqlStep.hpp"

#include <libev/ev.h>
#include <fstream>
#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// labor/Labor.cpp
extern net::Labor* g_pLabor;

namespace
{

/** 协程体放在具名函数中（与 test_step_co20 一致），避免「lambda + co_return」在部分编译选项下无法生成协程。 */
net::AsyncTask CoOnlyReturn(net::StepCo20& st)
{
    (void)st;
    co_return;
}

/**
 * 满足 Labor 纯虚函数的最小桩，并用 gmock 覆盖 Launch / Register 所触发的 ExecStep、RegisterCallback(Step*, double)。
 */
class LaborInterfaceStub : public net::Labor
{
public:
    LaborInterfaceStub()
    {
        m_strWorkPath = "/tmp/thunder_net_interface_ut";
        // GetFileData 失败路径会打 LOG4_ERROR，需可用 Logger（与 GetConfig 缺失文件走 cerr 不同）
        m_oLogger = log4cplus::Logger::getInstance("thunder_net_interface_ut");
        m_bInitLogger = true;
    }

    MOCK_METHOD(bool, ExecStep,
                (net::Step * pStep, ev_tstamp dTimeout, int iErrno, const std::string& strErrMsg,
                 const std::string& strErrShow),
                (override));

    MOCK_METHOD(bool, RegisterCallback, (net::Step* pStep, double dTimeout), (override));

    bool SendTo(const net::tagMsgShell&) override { return false; }
    bool SendTo(const net::tagMsgShell&, const MsgHead&, const MsgBody&) override { return false; }
    bool SendTo(const net::tagMsgShell&, uint32, uint32, const std::string&) override { return false; }
    bool AutoSend(const std::string&, const MsgHead&, const MsgBody&) override { return false; }
    bool SetConnectIdentify(const net::tagMsgShell&, const std::string&) override { return false; }
    void SetProcessName(const util::CJsonObject&) override {}
};

/** 安装 Stub 为当前 GetLabor()，并在析构前恢复 g_pLabor，避免悬空指针。 */
class ScopedLaborStub
{
public:
    explicit ScopedLaborStub(::testing::NiceMock<LaborInterfaceStub>& stub) : prev_(::GetLabor()), stub_(stub)
    {
    }

    ~ScopedLaborStub()
    {
        g_pLabor = prev_;
    }

    LaborInterfaceStub& stub() { return stub_; }

private:
    net::Labor* prev_;
    ::testing::NiceMock<LaborInterfaceStub>& stub_;
};

} // namespace

class NetInterfaceTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { log4cplus::initialize(); }

    static void TearDownTestSuite() { log4cplus::deinitialize(); }

    ::testing::NiceMock<LaborInterfaceStub> laborStub_;
};

TEST_F(NetInterfaceTest, GetConfigPath_AppendsConfSlash)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_EQ(net::GetConfigPath(), "/tmp/thunder_net_interface_ut/conf/");
}

TEST_F(NetInterfaceTest, GetConfig_ReadsValidJsonFile)
{
    ScopedLaborStub scope(laborStub_);
    const std::string realPath = "/tmp/thunder_ut_net_getconfig.json";
    std::ofstream out(realPath);
    ASSERT_TRUE(out.good());
    out << R"({"k":1})";
    out.close();

    util::CJsonObject conf;
    EXPECT_TRUE(net::GetConfig(conf, realPath));
    int v = 0;
    EXPECT_TRUE(conf.Get("k", v));
    EXPECT_EQ(v, 1);
    std::remove(realPath.c_str());
}

TEST_F(NetInterfaceTest, GetConfig_FalseWhenFileMissing)
{
    ScopedLaborStub scope(laborStub_);
    util::CJsonObject conf;
    EXPECT_FALSE(net::GetConfig(conf, "/nonexistent/thunder_ut_config.json"));
}

TEST_F(NetInterfaceTest, GetFileData_ReturnsContent)
{
    ScopedLaborStub scope(laborStub_);
    const std::string path = "/tmp/thunder_ut_net_getfile.txt";
    std::ofstream out(path);
    ASSERT_TRUE(out.good());
    out << "hello";
    out.close();
    std::string data;
    EXPECT_TRUE(net::GetFileData(data, path));
    EXPECT_EQ(data, "hello");
    std::remove(path.c_str());
}

TEST_F(NetInterfaceTest, GetFileData_FalseWhenMissing)
{
    ScopedLaborStub scope(laborStub_);
    std::string data;
    EXPECT_FALSE(net::GetFileData(data, "/nonexistent/thunder_ut_blob.bin"));
}

TEST_F(NetInterfaceTest, Launch_ReturnsFalseWhenStepNull)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_FALSE(net::Launch(std::unique_ptr<net::Step>{}));
}

TEST_F(NetInterfaceTest, Launch_ReturnsExecStepResultAndReleasesStep)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), ExecStep(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillOnce(::testing::Invoke([](net::Step* p, ev_tstamp, int, const std::string&, const std::string&) -> bool {
            if (p == nullptr)
            {
                return false;
            }
            const auto st = p->Emit();
            EXPECT_EQ(st, net::STATUS_CMD_COMPLETED);
            delete p;
            return true;
        }));

    net::tagMsgShell shell{};
    HttpMsg req;
    auto step = std::make_unique<net::StepCo20Func>(
        shell, req, [](net::StepCo20& st) -> net::AsyncTask { return CoOnlyReturn(st); });

    EXPECT_TRUE(net::Launch(std::move(step)));
}

TEST_F(NetInterfaceTest, Launch_ReturnsFalseWhenExecStepFails)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), ExecStep(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillOnce(::testing::Invoke([](net::Step* p, ev_tstamp, int, const std::string&, const std::string&) -> bool {
            delete p;
            return false;
        }));

    net::tagMsgShell shell{};
    HttpMsg req;
    auto step = std::make_unique<net::StepCo20Func>(
        shell, req, [](net::StepCo20& st) -> net::AsyncTask { return CoOnlyReturn(st); });

    EXPECT_FALSE(net::Launch(std::move(step)));
}

TEST_F(NetInterfaceTest, LaunchCo_UsesLaunchWithHttpMsg)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), ExecStep(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillOnce(::testing::Invoke([](net::Step* p, ev_tstamp, int, const std::string&, const std::string&) -> bool {
            delete p;
            return true;
        }));

    net::tagMsgShell shell{};
    HttpMsg req;
    EXPECT_TRUE(
        net::LaunchCo(shell, req, [](net::StepCo20& st) -> net::AsyncTask { return CoOnlyReturn(st); }));
}

TEST_F(NetInterfaceTest, LaunchCo_UsesLaunchWithMsgHead)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), ExecStep(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillOnce(::testing::Invoke([](net::Step* p, ev_tstamp, int, const std::string&, const std::string&) -> bool {
            delete p;
            return true;
        }));

    net::tagMsgShell shell{};
    MsgHead head;
    EXPECT_TRUE(
        net::LaunchCo(shell, head, [](net::StepCo20& mh) -> net::AsyncTask { return CoOnlyReturn(mh); }));
}

TEST_F(NetInterfaceTest, LaunchCo_UniquePtrStep)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), ExecStep(::testing::_, ::testing::_, ::testing::_, ::testing::_, ::testing::_))
        .WillOnce(::testing::Invoke([](net::Step* p, ev_tstamp, int, const std::string&, const std::string&) -> bool {
            delete p;
            return true;
        }));

    net::tagMsgShell shell{};
    HttpMsg req;
    EXPECT_TRUE(net::LaunchCo(std::make_unique<net::StepCo20Func>(
        shell, req, [](net::StepCo20& s) -> net::AsyncTask { return CoOnlyReturn(s); })));
}

TEST_F(NetInterfaceTest, Register_NullStep_ReturnsFalseWithoutCrash)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_FALSE(net::Register(nullptr));
}

TEST_F(NetInterfaceTest, Register_CallsRegisterCallbackAndInitOnSuccess)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), RegisterCallback(::testing::_, ::testing::_)).WillOnce(::testing::Return(true));

    auto* step = new net::MysqlStep("127.0.0.1", 3306, "db", "u", "p", "utf8");
    EXPECT_TRUE(net::Register(step));
    delete step;
}

TEST_F(NetInterfaceTest, Register_DeletesStepWhenRegisterCallbackFails)
{
    ScopedLaborStub scope(laborStub_);
    EXPECT_CALL(scope.stub(), RegisterCallback(::testing::_, ::testing::_)).WillOnce(::testing::Return(false));

    auto* step = new net::MysqlStep("127.0.0.1", 3306, "db", "u", "p", "utf8");
    EXPECT_FALSE(net::Register(step));
}

TEST_F(NetInterfaceTest, Json2Pb_EmptyString_ReturnsFalse)
{
    ScopedLaborStub scope(laborStub_);
    MsgBody body;
    EXPECT_FALSE(net::Json2Pb("", body));
}

TEST_F(NetInterfaceTest, Json2Pb_RoundTripField)
{
    ScopedLaborStub scope(laborStub_);
    const std::string json = R"({"sbody":"abc"})";
    MsgBody body;
    ASSERT_TRUE(net::Json2Pb(json, body));
    EXPECT_EQ(body.sbody(), "abc");
}

TEST_F(NetInterfaceTest, Pb2Json_String_Succeeds)
{
    ScopedLaborStub scope(laborStub_);
    MsgBody body;
    body.set_sbody("x");
    std::string json;
    ASSERT_TRUE(net::Pb2Json(body, json));
    EXPECT_NE(json.find("x"), std::string::npos);
}

TEST_F(NetInterfaceTest, Pb2Json_CJsonObject_Succeeds)
{
    ScopedLaborStub scope(laborStub_);
    MsgBody body;
    body.set_sbody("y");
    util::CJsonObject ojson;
    ASSERT_TRUE(net::Pb2Json(body, ojson));
    std::string s;
    EXPECT_TRUE(ojson.Get("sbody", s));
    EXPECT_EQ(s, "y");
}
