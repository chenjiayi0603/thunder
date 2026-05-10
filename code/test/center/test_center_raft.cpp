/**
 * Center Raft 共识层单元测试（纯逻辑，无 Net 依赖）
 *
 * 覆盖：
 *   - 多数派计算 (raft_majority)
 *   - Node ID 环形分配 (MergeNodeIdAllocRing)
 *   - 游标消毒 (SanitizeNodeIdCursor)
 *   - AllocNextNodeId 循环分配
 *   - Raft 状态机边缘逻辑
 */
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

// === 被测试的纯函数（与 SessionRaftCluster.cpp 中匿名命名空间实现一致）===

namespace
{

constexpr uint16_t NODE_ID_MAX = 256;

/** 多数派 = ⌊n/2⌋+1，n=0 退化为 1 */
static size_t raft_majority(size_t n)
{
    return n == 0 ? 1u : (n / 2 + 1);
}

uint16_t MergeNodeIdAllocRing(uint16_t local, uint32_t remoteU32)
{
    if (remoteU32 == 0u || remoteU32 >= static_cast<uint32_t>(NODE_ID_MAX))
    {
        return (local == 0u) ? static_cast<uint16_t>(1u) : local;
    }
    const uint16_t remote = static_cast<uint16_t>(remoteU32);
    if (local == 0u)
    {
        local = 1u;
    }
    const uint32_t ring = static_cast<uint32_t>(NODE_ID_MAX - 1u);
    const uint32_t a = static_cast<uint32_t>(local) - 1u;
    const uint32_t b = static_cast<uint32_t>(remote) - 1u;
    const uint32_t forwardLocalToRemote = (b + ring - a) % ring;
    if (forwardLocalToRemote == 0u)
    {
        return local;
    }
    if (forwardLocalToRemote < (ring + 1u) / 2u)
    {
        return remote;
    }
    return local;
}

uint16_t SanitizeNodeIdCursor(uint16_t cursor)
{
    if (cursor == 0u || cursor >= static_cast<uint16_t>(NODE_ID_MAX))
    {
        return 1u;
    }
    return cursor;
}

std::pair<uint16_t, uint16_t> AllocNextNodeIdAndAdvance(uint16_t cursor)
{
    cursor = SanitizeNodeIdCursor(cursor);
    const uint16_t id = cursor;
    ++cursor;
    if (cursor >= NODE_ID_MAX)
    {
        cursor = 1;
    }
    return {id, cursor};
}

} // namespace

// ========== 多数派测试 ==========

TEST(CenterRaft, MajoritySingleNode)
{
    EXPECT_EQ(1u, raft_majority(0));
    EXPECT_EQ(1u, raft_majority(1));
}

TEST(CenterRaft, MajorityTwoNodesRequiresBoth)
{
    EXPECT_EQ(2u, raft_majority(2));
}

TEST(CenterRaft, MajorityThreeNodes)
{
    EXPECT_EQ(2u, raft_majority(3));
}

TEST(CenterRaft, MajorityFourNodes)
{
    EXPECT_EQ(3u, raft_majority(4));
}

TEST(CenterRaft, MajorityFiveNodes)
{
    EXPECT_EQ(3u, raft_majority(5));
}

TEST(CenterRaft, MajorityTenNodes)
{
    EXPECT_EQ(6u, raft_majority(10));
}

TEST(CenterRaft, MajorityLargeScale)
{
    EXPECT_EQ(51u, raft_majority(100));
    EXPECT_EQ(6u, raft_majority(11));
}

// ========== Node ID 环形合并测试 ==========

TEST(CenterRaft, MergeRing_LocalZero_BecomesOne)
{
    EXPECT_EQ(5u, MergeNodeIdAllocRing(0, 5));
}

TEST(CenterRaft, MergeRing_RemoteZero_KeepsLocal)
{
    EXPECT_EQ(5u, MergeNodeIdAllocRing(5, 0));
}

TEST(CenterRaft, MergeRing_RemoteExceedsMax_KeepsLocal)
{
    EXPECT_EQ(5u, MergeNodeIdAllocRing(5, 256));
    EXPECT_EQ(5u, MergeNodeIdAllocRing(5, 999));
}

TEST(CenterRaft, MergeRing_SameValues)
{
    EXPECT_EQ(10u, MergeNodeIdAllocRing(10, 10));
}

TEST(CenterRaft, MergeRing_RemoteAheadWithinHalfRing)
{
    EXPECT_EQ(20u, MergeNodeIdAllocRing(10, 20));
}

TEST(CenterRaft, MergeRing_LocalAheadRemoteBehind)
{
    EXPECT_EQ(10u, MergeNodeIdAllocRing(200, 10));
}

TEST(CenterRaft, MergeRing_RemoteFarBehind_KeepsLocal)
{
    EXPECT_EQ(10u, MergeNodeIdAllocRing(10, 200));
}

TEST(CenterRaft, MergeRing_LocalAtOne_RemoteAhead)
{
    EXPECT_EQ(30u, MergeNodeIdAllocRing(1, 30));
}

TEST(CenterRaft, MergeRing_LocalAt255_RemoteAt1)
{
    EXPECT_EQ(1u, MergeNodeIdAllocRing(255, 1));
}

TEST(CenterRaft, MergeRing_ExactlyHalfRing_TakesRemote)
{
    EXPECT_EQ(128u, MergeNodeIdAllocRing(1, 128));
}

TEST(CenterRaft, MergeRing_HalfRingMinusOne_TakesRemote)
{
    EXPECT_EQ(127u, MergeNodeIdAllocRing(1, 127));
}

// ========== 游标消毒测试 ==========

TEST(CenterRaft, SanitizeCursor_ValidRange)
{
    EXPECT_EQ(1u, SanitizeNodeIdCursor(1));
    EXPECT_EQ(128u, SanitizeNodeIdCursor(128));
    EXPECT_EQ(255u, SanitizeNodeIdCursor(255));
}

TEST(CenterRaft, SanitizeCursor_ZeroBecomesOne)
{
    EXPECT_EQ(1u, SanitizeNodeIdCursor(0));
}

TEST(CenterRaft, SanitizeCursor_ExceedsMaxBecomesOne)
{
    EXPECT_EQ(1u, SanitizeNodeIdCursor(256));
    EXPECT_EQ(1u, SanitizeNodeIdCursor(300));
}

// ========== AllocNextNodeId 循环分配测试 ==========

TEST(CenterRaft, AllocNextNodeId_BasicSequence)
{
    auto [id1, cursor1] = AllocNextNodeIdAndAdvance(1);
    EXPECT_EQ(1u, id1);
    EXPECT_EQ(2u, cursor1);

    auto [id2, cursor2] = AllocNextNodeIdAndAdvance(cursor1);
    EXPECT_EQ(2u, id2);
    EXPECT_EQ(3u, cursor2);
}

TEST(CenterRaft, AllocNextNodeId_WrapAt255)
{
    auto [id, cursor] = AllocNextNodeIdAndAdvance(255);
    EXPECT_EQ(255u, id);
    EXPECT_EQ(1u, cursor);
}

TEST(CenterRaft, AllocNextNodeId_ZeroCursorNormalized)
{
    auto [id, cursor] = AllocNextNodeIdAndAdvance(0);
    EXPECT_EQ(1u, id);
    EXPECT_EQ(2u, cursor);
}

TEST(CenterRaft, AllocNextNodeId_FullCycle)
{
    uint16_t cursor = 1;
    std::unordered_set<uint16_t> allocated;
    for (int i = 0; i < 255; ++i)
    {
        auto [id, next] = AllocNextNodeIdAndAdvance(cursor);
        EXPECT_EQ(0u, allocated.count(id)) << "Duplicate id " << id;
        allocated.insert(id);
        cursor = next;
    }
    EXPECT_EQ(255u, allocated.size());
    auto [wrapId, wrapCursor] = AllocNextNodeIdAndAdvance(cursor);
    EXPECT_EQ(1u, wrapId);
    EXPECT_EQ(2u, wrapCursor);
}

TEST(CenterRaft, AllocNextNodeId_NoZeroAllocated)
{
    uint16_t cursor = 1;
    for (int i = 0; i < 510; ++i)
    {
        auto [id, next] = AllocNextNodeIdAndAdvance(cursor);
        EXPECT_NE(0u, id) << "Allocated id=0 at iteration " << i;
        cursor = next;
    }
}

// ========== Raft 端到端纯逻辑测试 ==========

TEST(CenterRaft, RaftMajorityEdge)
{
    EXPECT_EQ(4u, raft_majority(7));
    EXPECT_EQ(4u, raft_majority(6));
}

TEST(CenterRaft, MergeRing_MonotonicAdvance)
{
    uint16_t local = 1;
    for (uint32_t remote = 2; remote <= 128; ++remote)
    {
        local = MergeNodeIdAllocRing(local, remote);
        EXPECT_EQ(static_cast<uint16_t>(remote), local);
    }
}

TEST(CenterRaft, MergeRing_RejectsStale)
{
    uint16_t local = 100;
    local = MergeNodeIdAllocRing(local, 50);
    EXPECT_EQ(100u, local) << "Should not regress to stale cursor";
}
