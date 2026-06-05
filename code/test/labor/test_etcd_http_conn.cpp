/**
 * @file  test_etcd_http_conn.cpp
 * @brief EtcdHttpConn 集成测试（issus #24 Phase A）—— 对真实 etcd 验证
 *
 * 证明:在 libev 主循环上用框架 HttpCodec 自管连接,能正确编码请求 + 解码 etcd 响应,
 * 且串行链式请求(lease → range)在同一 keep-alive 连接上工作。
 * 需本机 127.0.0.1:2379 有 etcd;不可达则 SKIP。
 */
#include <ev.h>
#include <gtest/gtest.h>

#include <log4cplus/logger.h>

#include <string>

#include "labor/EtcdHttpConn.hpp"

using namespace net;

namespace
{
void OnTimeout(struct ev_loop* loop, ev_timer*, int) { ev_break(loop, EVBREAK_ALL); }
}  // namespace

// 单请求:lease grant 应拿到 200 + 含 "ID" 的响应体
TEST(EtcdHttpConn, RealLeaseGrant)
{
    struct ev_loop* loop   = ev_loop_new(EVFLAG_AUTO);
    auto            logger = log4cplus::Logger::getInstance("test");
    EtcdHttpConn    conn(loop, "127.0.0.1", 2379, logger);

    bool        done = false, ok = false;
    int         status = 0;
    std::string body;
    conn.Post("/v3/lease/grant", "{\"TTL\":10}",
              [&](bool o, int s, const std::string& b) {
                  done = true; ok = o; status = s; body = b;
                  ev_break(loop, EVBREAK_ALL);
              });

    ev_timer to;
    ev_timer_init(&to, OnTimeout, 3.0, 0.0);
    ev_timer_start(loop, &to);
    ev_run(loop, 0);
    conn.Close();              // 停 watcher（loop 销毁前）
    ev_loop_destroy(loop);

    if (!done || (!ok && body.empty()))
        GTEST_SKIP() << "etcd 127.0.0.1:2379 不可达，跳过集成测试";

    EXPECT_TRUE(ok);
    EXPECT_EQ(status, 200);
    EXPECT_NE(body.find("\"ID\""), std::string::npos) << "resp=" << body;
}

// 链式:lease grant 完成后在回调里再发 range —— 验证串行队列 + 同连接复用(Phase B 注册流程基础)
TEST(EtcdHttpConn, RealChainedLeaseThenRange)
{
    struct ev_loop* loop   = ev_loop_new(EVFLAG_AUTO);
    auto            logger = log4cplus::Logger::getInstance("test");
    EtcdHttpConn    conn(loop, "127.0.0.1", 2379, logger);

    bool        got1 = false, got2 = false;
    std::string body1, body2;
    conn.Post("/v3/lease/grant", "{\"TTL\":10}",
              [&](bool ok, int, const std::string& b) {
                  got1 = ok; body1 = b;
                  if (ok)
                  {
                      conn.Post("/v3/kv/range", R"({"key":"AA==","range_end":"AA=="})",
                                [&](bool ok2, int, const std::string& b2) {
                                    got2 = ok2; body2 = b2;
                                    ev_break(loop, EVBREAK_ALL);
                                });
                  }
                  else
                  {
                      ev_break(loop, EVBREAK_ALL);
                  }
              });

    ev_timer to;
    ev_timer_init(&to, OnTimeout, 4.0, 0.0);
    ev_timer_start(loop, &to);
    ev_run(loop, 0);
    conn.Close();              // 停 watcher（loop 销毁前）
    ev_loop_destroy(loop);

    if (!got1 && body1.empty())
        GTEST_SKIP() << "etcd 127.0.0.1:2379 不可达，跳过集成测试";

    EXPECT_TRUE(got1) << "lease grant 失败";
    EXPECT_NE(body1.find("\"ID\""), std::string::npos);
    EXPECT_TRUE(got2) << "链式 range 失败";
    EXPECT_NE(body2.find("\"header\""), std::string::npos) << "resp2=" << body2;
}
