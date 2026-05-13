/**
 * ConHash 一致性哈希单元测试
 *
 * lookupNode() 返回通过 addNode 传入的用户数据指针 (void*)。
 * 测试通过传入标识字符串与返回指针的关系验证一致性哈希行为。
 */
#include "dispatcher/ConHash.hpp"
#include <gtest/gtest.h>
#include <string>
#include <set>

class ConHashTest : public ::testing::Test
{
};

TEST_F(ConHashTest, ConstructionDefault)
{
    ConHash ch;
    SUCCEED();
}

TEST_F(ConHashTest, LookupEmptyReturnsNull)
{
    ConHash ch;
    EXPECT_EQ(nullptr, ch.lookupNode("any_key"));
}

TEST_F(ConHashTest, AddNodeAndLookupReturnsNonNull)
{
    ConHash ch;
    int data = 42;
    EXPECT_EQ(0, ch.addNode("srv1", 160, &data));
    auto* result = ch.lookupNode("user_1");
    EXPECT_EQ(&data, result);
}

TEST_F(ConHashTest, AddNodeZeroVirtualFails)
{
    ConHash ch;
    EXPECT_EQ(-1, ch.addNode("bad", 0, nullptr));
}

TEST_F(ConHashTest, AddNodeEmptyIdentityFails)
{
    ConHash ch;
    EXPECT_EQ(-1, ch.addNode("", 160, nullptr));
}

TEST_F(ConHashTest, LookupConsistency)
{
    ConHash ch;
    int data_a = 1, data_b = 2, data_c = 3;
    ch.addNode("A", 160, &data_a);
    ch.addNode("B", 160, &data_b);
    ch.addNode("C", 160, &data_c);

    // 相同 key 始终返回同一节点
    for (int i = 0; i < 10; ++i)
    {
        auto* n1 = ch.lookupNode("constant_key");
        auto* n2 = ch.lookupNode("constant_key");
        EXPECT_EQ(n1, n2);
    }
}

TEST_F(ConHashTest, MultipleNodesAreUsed)
{
    ConHash ch;
    int data_a = 1, data_b = 2, data_c = 3;
    ch.addNode("A", 160, &data_a);
    ch.addNode("B", 160, &data_b);
    ch.addNode("C", 160, &data_c);

    std::set<void*> seen;
    for (int i = 0; i < 500; ++i)
    {
        seen.insert(ch.lookupNode(std::to_string(i)));
    }
    // 至少应该能落到两个不同节点
    EXPECT_GE(seen.size(), 2u);
}

TEST_F(ConHashTest, RemoveNode)
{
    ConHash ch;
    int data_x = 10, data_y = 20;
    ch.addNode("X", 160, &data_x);
    ch.addNode("Y", 160, &data_y);

    int removed = ch.removeNode("X");
    EXPECT_EQ(1, removed);

    // X 不存在则返回 0
    EXPECT_EQ(0, ch.removeNode("X"));

    // Y 仍然可用
    auto* y = ch.lookupNode("some_key");
    EXPECT_EQ(&data_y, y);
}

TEST_F(ConHashTest, RemoveNonexistentNode)
{
    ConHash ch;
    EXPECT_EQ(0, ch.removeNode("ghost"));
}

TEST_F(ConHashTest, ClearAllNodes)
{
    ConHash ch;
    ch.addNode("n1", 160, nullptr);
    ch.addNode("n2", 160, nullptr);
    ch.clear();
    EXPECT_EQ(nullptr, ch.lookupNode("key"));
}

TEST_F(ConHashTest, SetHashFunction)
{
    ConHash ch;
    // 自定义哈希函数：始终返回 42
    ch.setHashFunc([](const std::string&) -> uint32_t { return 42; });
    int data = 99;
    ch.addNode("fixed", 160, &data);
    auto* result = ch.lookupNode("anything");
    EXPECT_EQ(&data, result);
}
