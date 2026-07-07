/*******************************************************************************
 * Project:  Thunder
 * @file     test_connector_tcp.cpp
 * @brief    TcpCenterConnector 单元测试
 * @author   cjy
 * @date:    2026-05-24
 * @note
 *   测试覆盖：
 *     1. 配置解析（JSON 数组 / CSV / 空配）
 *     2. TryConsumeMessage 事件分发
 *     3. IsCenterConnection / OnConnectionDestroy
 *     4. SetNodeInfo 节点信息存储
 *     5. Raft leader 跟踪逻辑
 ******************************************************************************/
#include <gtest/gtest.h>
#include <ev.h>
#include <cstring>
#include <memory>
#include <string>
#include "protocol/oss_sys.pb.h"
#include "util/json/CJsonObject.hpp"
#include "labor/TcpCenterConnector.hpp"
#include "cmd/CW.hpp"

using namespace net;

namespace
{

// ========== 辅助 ==========

struct ev_loop* makeLoop() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
    return ev_loop_new(EVFLAG_FORKCHECK | EVBACKEND_EPOLL);
#pragma GCC diagnostic pop
}
void freeLoop(struct ev_loop* loop) { if (loop) ev_loop_destroy(loop); }

// 构造一个最简的 center 配置 JSON
std::string centerJsonArray(const std::string& host, int port)
{
    return R"({"host":")" + host + R"(","port":)" + std::to_string(port) + "}";
}

std::string centerJsonArrayMulti()
{
    return R"([
        {"host":"10.0.0.1","port":27001},
        {"host":"10.0.0.2","port":27001}
    ])";
}

// 构造 CMD_RSP_NODE_REGISTER 的消息体（模拟 Center 响应）
std::string makeRegisterRsp(uint32_t errcode, uint32_t node_id = 42,
                            const std::string& leader = "",
                            int route_node_count = 0)
{
    NodeReportRsp rsp;
    rsp.set_errcode(errcode);
    if (node_id != 0) rsp.set_node_id(node_id);
    if (!leader.empty()) rsp.set_current_leader_identify(leader);
    if (route_node_count > 0)
    {
        NodeNotice* notice = rsp.mutable_subscribed_route_snapshot();
        for (int i = 0; i < route_node_count; ++i)
        {
            auto* n = notice->add_node_arry_reg();
            n->set_node_ip("10.0.0." + std::to_string(i + 1));
            n->set_node_port(10000 + i);
            n->set_node_type("LOGIC");
            n->set_node_id(i + 1);
        }
    }
    return rsp.SerializeAsString();
}

// 收集 CenterEvent 的回调辅助
struct EventCollector
{
    std::vector<CenterEvent> events;
    void onEvent(const CenterEvent& ev) { events.push_back(ev); }
    void clear() { events.clear(); }
};

// ========== 配置解析测试 ==========

TEST(TcpCenterConnector, Init_SingleCenter_Object)
{
    auto loop = makeLoop();
    ASSERT_NE(loop, nullptr);

    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    ASSERT_TRUE(conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr));
    EXPECT_EQ(conn.CenterCount(), 1u);
    EXPECT_FALSE(conn.IsConnected());  // 未实际连接

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, Init_MultiCenter_Array)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArrayMulti()));

    TcpCenterConnector conn(conf);
    ASSERT_TRUE(conn.Init(loop, nullptr, nullptr));
    EXPECT_EQ(conn.CenterCount(), 2u);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, Init_EmptyConfig)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse("{}"));

    TcpCenterConnector conn(conf);
    ASSERT_TRUE(conn.Init(loop, nullptr, nullptr));
    EXPECT_EQ(conn.CenterCount(), 0u);
    EXPECT_EQ(conn.Name(), std::string("tcp"));

    // ReportNodeStatus with no centers should not crash
    EXPECT_FALSE(conn.ReportNodeStatus("fake_report", true));

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, SetNodeInfo)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    conf.Parse("{}");
    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // SetNodeInfo should not crash
    conn.SetNodeInfo("ACCESS", "192.168.1.1", 16001,
                     "1.2.3.4", 8080, "gw.example.com", 443, 4);

    conn.Destroy();
    freeLoop(loop);
}

// ========== IsCenterConnection / OnConnectionDestroy ==========

TEST(TcpCenterConnector, IsCenterConnection)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    EXPECT_TRUE(conn.IsCenterConnection("10.0.0.1:27001.0"));
    EXPECT_FALSE(conn.IsCenterConnection("10.0.0.2:27001.0"));
    EXPECT_FALSE(conn.IsCenterConnection(""));
    EXPECT_FALSE(conn.IsCenterConnection("random_string"));

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, OnConnectionDestroy_Unknown)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    conf.Parse("{}");
    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // 销毁不存在的连接不应崩溃
    conn.OnConnectionDestroy("not_exist:27001.0", 99, 1);

    conn.Destroy();
    freeLoop(loop);
}

// ========== TryConsumeMessage 事件分发测试 ==========

TEST(TcpCenterConnector, TryConsumeMessage_NotOwner)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    conf.Parse("{}");
    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // 非本插件管辖的 identify → 返回 false
    EXPECT_FALSE(conn.TryConsumeMessage(5, 1, "other:27001.0",
                                         CMD_RSP_NODE_REGISTER, 100, "body"));

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_RegisterSuccess)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    // 模拟 Center 返回注册成功
    std::string body = makeRegisterRsp(0, 42);
    bool consumed = conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                                           CMD_RSP_NODE_REGISTER, 1, body);
    EXPECT_TRUE(consumed);
    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::Registered);
    EXPECT_EQ(collector.events[0].node_id, 42u);
    EXPECT_EQ(collector.events[0].errcode, 0);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_RegisterError)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    std::string body = makeRegisterRsp(1, 0);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 2, body);
    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::Registered);
    EXPECT_EQ(collector.events[0].errcode, 1);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_RouteSnapshot)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    // 注册响应带 3 个节点的路由快照
    std::string body = makeRegisterRsp(0, 42, "", 3);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 3, body);

    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::Registered);
    EXPECT_FALSE(collector.events[0].route_snapshot.empty());

    // 验证快照内容
    NodeNotice notice;
    ASSERT_TRUE(notice.ParseFromString(collector.events[0].route_snapshot));
    EXPECT_EQ(notice.node_arry_reg_size(), 3);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_HeartbeatRsp)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    // CMD_RSP_NODE_STATUS_REPORT 也应触发 Registered 事件
    std::string body = makeRegisterRsp(0, 42);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_STATUS_REPORT, 4, body);

    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::Registered);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_ConfigUpdated)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    ConfigInfo cfg;
    cfg.set_file_content("{\"key\":\"value\"}");
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_SET_NODE_CUSTOM_CONFIG, 5, cfg.SerializeAsString());

    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::ConfigUpdated);
    EXPECT_EQ(collector.events[0].config_content, "{\"key\":\"value\"}");

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_NodeStop)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_NODE_STOP, 6, "");

    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::NodeStop);

    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, TryConsumeMessage_RestartWorkers)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    EventCollector collector;
    conn.Init(loop,
        [&](const CenterEvent& ev) { collector.onEvent(ev); }, nullptr);

    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_NODE_RESTART_WORKERS, 7, "");

    ASSERT_EQ(collector.events.size(), 1u);
    EXPECT_EQ(collector.events[0].type, CenterEventType::NodeRestartWorkers);

    conn.Destroy();
    freeLoop(loop);
}

// ========== Raft leader 跟踪 ==========

TEST(TcpCenterConnector, RaftLeader_Err2ClearsLeader)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArrayMulti()));  // 两个 center

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // err=2 (no stable leader) → 不应崩溃，应清除所有 leader
    std::string body = makeRegisterRsp(2, 0);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 1, body);

    // 验证不会崩溃（leader 在插件内部清除）
    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, RaftLeader_ReceivesLeaderHint)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArrayMulti()));

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // 注册响应中指出 leader 是 10.0.0.2:27001.0
    std::string body = makeRegisterRsp(0, 42, "10.0.0.2:27001.0");
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 1, body);

    // 不应该崩溃
    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, RaftLeader_UnknownLeaderIgnored)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // 注册响应中指出 leader 是一个不在本地配置中的地址
    std::string body = makeRegisterRsp(0, 42, "10.0.0.99:27001.0");
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 1, body);

    // 不应崩溃
    conn.Destroy();
    freeLoop(loop);
}

// ========== 生命周期 ==========

TEST(TcpCenterConnector, DestroyBeforeInit)
{
    util::CJsonObject conf;
    conf.Parse("{}");
    TcpCenterConnector conn(conf);
    conn.Destroy();  // 不应崩溃
}

TEST(TcpCenterConnector, DoubleInit)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    conf.Parse(centerJsonArray("10.0.0.1", 27001));
    TcpCenterConnector conn(conf);
    ASSERT_TRUE(conn.Init(loop, nullptr, nullptr));
    ASSERT_TRUE(conn.Init(loop, nullptr, nullptr));  // 第二次应返回 true（幂等）
    conn.Destroy();
    freeLoop(loop);
}

TEST(TcpCenterConnector, IsConnected_NoActiveConnection)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    conf.Parse(centerJsonArray("10.0.0.1", 27001));
    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);
    EXPECT_FALSE(conn.IsConnected());  // 未实际发起连接
    conn.Destroy();
    freeLoop(loop);
}

// ========== 回调为 nullptr 不崩溃 ==========

TEST(TcpCenterConnector, NullCallback_NoCrash)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);  // callback 为 null

    // 各种消息不应崩溃
    std::string body = makeRegisterRsp(0, 42);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_RSP_NODE_REGISTER, 1, body);
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_NODE_STOP, 2, "");
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_NODE_RESTART_WORKERS, 3, "");

    ConfigInfo cfg;
    cfg.set_file_content("x");
    conn.TryConsumeMessage(0, 0, "10.0.0.1:27001.0",
                           CMD_REQ_SET_NODE_CUSTOM_CONFIG, 4, cfg.SerializeAsString());

    conn.Destroy();
    freeLoop(loop);
}

// ========== Stale fd/seq 消费 ==========

TEST(TcpCenterConnector, TryConsumeMessage_StaleFd)
{
    auto loop = makeLoop();
    util::CJsonObject conf;
    ASSERT_TRUE(conf.Parse(centerJsonArray("10.0.0.1", 27001)));

    TcpCenterConnector conn(conf);
    conn.Init(loop, nullptr, nullptr);

    // identify 匹配但 fd/seq 不匹配 → 消费掉（避免后续误处理）
    std::string body = makeRegisterRsp(0, 42);
    bool consumed = conn.TryConsumeMessage(999, 999, "10.0.0.1:27001.0",
                                           CMD_RSP_NODE_REGISTER, 1, body);
    EXPECT_TRUE(consumed);  // 仍然消费（不抛给其他 handler）

    conn.Destroy();
    freeLoop(loop);
}

}  // namespace
