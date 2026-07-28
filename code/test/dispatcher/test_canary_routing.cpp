#include "dispatcher/Nodes.hpp"
#include <gtest/gtest.h>
#include <map>
#include <string>

using namespace net;

class CanaryRoutingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        nodes = std::make_unique<Nodes>(HASH_fnv1a_64, 200);
        // 关闭内部路由心跳检查，避免依赖 Labor 单例
        nodes->SetCheckInternalRouter(false, 0);

        // 注册 v1/v2 测试节点
        nodes->AddNodeIdentify("LOGIC", "10.0.0.1:16001");
        nodes->AddNodeIdentify("LOGIC", "10.0.0.2:16002");
        nodes->AddNodeIdentify("LOGIC", "10.0.0.3:16003");

        // 初始化随机种子
        std::srand(42);
    }

    std::unique_ptr<Nodes> nodes;
};

// ── 无灰度配置 → 一致性哈希正常工作 ───────────────────────
TEST_F(CanaryRoutingTest, NoCanaryWeights_FallbackToHash)
{
    // 未设置 canary weights，应该走默认 hash 路由（不崩溃）
    for (int i = 0; i < 100; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        EXPECT_FALSE(node.empty());
    }
}

// ── 权重路由：单节点 100% → 全部命中 ──────────────────────
TEST_F(CanaryRoutingTest, SingleWeight_AlwaysHit)
{
    std::map<std::string, int32_t> weights = {{"10.0.0.1:16001", 100}};
    nodes->SetCanaryWeights("LOGIC", weights);

    for (int i = 0; i < 100; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        EXPECT_EQ(node, "10.0.0.1:16001");
    }
}

// ── 权重 0 → 节点被排除 ──────────────────────────────────
TEST_F(CanaryRoutingTest, ZeroWeight_NodeExcluded)
{
    std::map<std::string, int32_t> weights = {
        {"10.0.0.1:16001", 100},
        {"10.0.0.2:16002", 0}     // 权重 0 → 不应被选中
    };
    nodes->SetCanaryWeights("LOGIC", weights);

    for (int i = 0; i < 100; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        EXPECT_EQ(node, "10.0.0.1:16001");
    }
}

// ── 70/30 分布验证 ───────────────────────────────────────
TEST_F(CanaryRoutingTest, WeightedDistribution_70_30)
{
    std::map<std::string, int32_t> weights = {
        {"10.0.0.1:16001", 70},
        {"10.0.0.2:16002", 30}
    };
    nodes->SetCanaryWeights("LOGIC", weights);

    int count1 = 0, count2 = 0;
    const int N = 10000;
    for (int i = 0; i < N; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        if (node == "10.0.0.1:16001") count1++;
        else if (node == "10.0.0.2:16002") count2++;
    }

    // 允许 ±2.5% 统计波动
    EXPECT_NEAR(count1, 7000, 250);
    EXPECT_NEAR(count2, 3000, 250);
    EXPECT_EQ(count1 + count2, N);
}

// ── 清除权重 → 恢复一致性哈希 ────────────────────────────
TEST_F(CanaryRoutingTest, ClearWeights_RestoreHash)
{
    std::map<std::string, int32_t> weights = {{"10.0.0.1:16001", 100}};
    nodes->SetCanaryWeights("LOGIC", weights);
    nodes->ClearCanaryWeights("LOGIC");

    // 恢复后应走哈希路由（不崩溃）
    for (int i = 0; i < 50; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        EXPECT_FALSE(node.empty());
    }
}

// ── 不同 nodeType 的权重互不影响 ──────────────────────────
TEST_F(CanaryRoutingTest, DifferentNodeTypes_Independent)
{
    nodes->AddNodeIdentify("INTERFACE", "10.0.0.10:16080");

    // 只给 LOGIC 设权重
    std::map<std::string, int32_t> weights = {{"10.0.0.1:16001", 100}};
    nodes->SetCanaryWeights("LOGIC", weights);

    // LOGIC → 走权重路由
    auto& logicNode = nodes->GetNodeIdentify("LOGIC", "hash1");
    EXPECT_EQ(logicNode, "10.0.0.1:16001");

    // INTERFACE → 无权重，走哈希（不崩溃）
    auto& ifNode = nodes->GetNodeIdentify("INTERFACE", "hash2");
    EXPECT_FALSE(ifNode.empty());
}

// ── 空权重 map 不生效 ───────────────────────────────────
TEST_F(CanaryRoutingTest, EmptyWeightsMap_NoEffect)
{
    std::map<std::string, int32_t> empty;
    nodes->SetCanaryWeights("LOGIC", empty);

    for (int i = 0; i < 50; i++)
    {
        auto& node = nodes->GetNodeIdentify("LOGIC", std::to_string(i));
        EXPECT_FALSE(node.empty());
    }
}
