#include <gtest/gtest.h>
#include <cstddef>

/** 与 SessionOnlineNodes::InitElection 中多数派定义一致：⌊n/2⌋+1，n=0 时退化为 1 */
static size_t raft_majority(size_t n)
{
    return n == 0 ? 1u : (n / 2 + 1);
}

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
