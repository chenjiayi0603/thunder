/**
 * @file  test_etcd_parse.cpp
 * @brief EtcdParse 纯解析/决策逻辑单元测试（无 etcd 依赖）
 *
 * 用真实抓取的 etcd HTTP gateway 响应作为夹具，锁住两个线上 bug：
 *   - issus #19：注册键绑在旧租约时必须重绑（DecideRegAction → Rebind）
 *   - issus #20：空 keyspace 的 range 响应必须能取出 revision；watch 取消必须
 *                能解析 compact_revision，否则 start_revision 退化为 1 → 每秒重连风暴
 */
#include <gtest/gtest.h>

#include "register/EtcdParse.hpp"

using namespace net::etcd_parse;

// ── issus #20：空 keyspace 的 /v3/kv/range 响应（真实抓取）必须取出 revision ──
// 这是风暴根因的关键夹具：revision 是 gateway int64 字符串 "84"。
TEST(EtcdParseRevision, ExtractsRevisionFromEmptyRangeResponse)
{
    const std::string resp =
        R"({"header":{"cluster_id":"14841639068965178418","member_id":"10276657743932975437","revision":"84","raft_term":"18"}})";
    int64_t rev = 0;
    EXPECT_TRUE(ParseRangeRevision(resp, rev));
    EXPECT_EQ(rev, 84);
}

TEST(EtcdParseRevision, ExtractsRevisionWithKvs)
{
    const std::string resp =
        R"({"header":{"revision":"100"},"kvs":[{"key":"YQ==","value":"Yg=="}],"count":"1"})";
    int64_t rev = 0;
    EXPECT_TRUE(ParseRangeRevision(resp, rev));
    EXPECT_EQ(rev, 100);
}

TEST(EtcdParseRevision, FailsOnEmptyOrGarbage)
{
    int64_t rev = -1;
    EXPECT_FALSE(ParseRangeRevision("", rev));
    EXPECT_FALSE(ParseRangeRevision("not json", rev));
    EXPECT_FALSE(ParseRangeRevision(R"({"no_header":1})", rev));
}

// ── issus #20：watch 取消响应（真实抓取）必须解析出 canceled + compact_revision ──
TEST(EtcdParseWatch, ParsesCancelWithCompactRevision)
{
    const std::string line =
        R"({"result":{"header":{"cluster_id":"14841639068965178418","member_id":"10276657743932975437","raft_term":"18"},"canceled":true,"compact_revision":"84"}})";
    WatchControl wc;
    ASSERT_TRUE(ParseWatchControl(line, wc));
    EXPECT_TRUE(wc.canceled);
    EXPECT_EQ(wc.compactRevision, 84);
}

TEST(EtcdParseWatch, ParsesCreated)
{
    const std::string line =
        R"({"result":{"header":{"revision":"84"},"created":true}})";
    WatchControl wc;
    ASSERT_TRUE(ParseWatchControl(line, wc));
    EXPECT_TRUE(wc.created);
    EXPECT_FALSE(wc.canceled);
    EXPECT_EQ(wc.compactRevision, 0);
}

TEST(EtcdParseWatch, RejectsNonObjectLine)
{
    WatchControl wc;
    EXPECT_FALSE(ParseWatchControl("", wc));
    EXPECT_FALSE(ParseWatchControl("garbage", wc));
}

// ── issus #19：注册动作决策（核心 bug） ──
TEST(EtcdRegAction, ClaimWhenKeyAbsent)
{
    EXPECT_EQ(DecideRegAction(/*found=*/false, /*existing=*/0, /*current=*/123), RegAction::Claim);
}

TEST(EtcdRegAction, FreshWhenKeyBoundToCurrentLease)
{
    EXPECT_EQ(DecideRegAction(true, /*existing=*/123, /*current=*/123), RegAction::Fresh);
}

// 这是线上 bug 的精确场景：重启换了新租约，但 etcd 里还残留旧租约绑的注册键。
TEST(EtcdRegAction, RebindWhenKeyBoundToStaleLease)
{
    EXPECT_EQ(DecideRegAction(true, /*existing=*/100 /*旧租约*/, /*current=*/200 /*新租约*/),
              RegAction::Rebind);
}

TEST(EtcdRegAction, RebindWhenExistingKeyHasNoLease)
{
    EXPECT_EQ(DecideRegAction(true, /*existing=*/0, /*current=*/200), RegAction::Rebind);
}

TEST(EtcdRegAction, RebindWhenCurrentLeaseUnset)
{
    // 当前租约未建立（异常）→ 不能当作 Fresh，按 Rebind 走（会先建租约）。
    EXPECT_EQ(DecideRegAction(true, /*existing=*/100, /*current=*/0), RegAction::Rebind);
}

// ── 注册键 range 响应：取出 lease（gateway int64 字符串） ──
TEST(EtcdParseRegistryKv, ExtractsLeaseAndExistence)
{
    // value 用明文 JSON（helper 支持），lease 为 gateway 字符串形式
    const std::string resp =
        R"({"header":{"revision":"10"},"count":"1","kvs":[{"key":"a","value":"{\"node_id\":7}","lease":"7587895296023927814"}]})";
    uint32_t nodeId = 0;
    int64_t  lease  = 0;
    EXPECT_TRUE(ParseRegistryKv(resp, nodeId, lease));
    EXPECT_EQ(lease, 7587895296023927814LL);
    EXPECT_EQ(nodeId, 7u);
}

TEST(EtcdParseRegistryKv, FalseWhenCountZero)
{
    const std::string resp = R"({"header":{"revision":"84"},"count":"0"})";
    uint32_t nodeId = 99;
    int64_t  lease  = 99;
    EXPECT_FALSE(ParseRegistryKv(resp, nodeId, lease));
}
