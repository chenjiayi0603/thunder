/**
 * RedisOperator 存储协议单元测试
 */
#include "NetDefine.hpp"
#include "storage/RedisOperator.hpp"
#include <gtest/gtest.h>

TEST(RedisOperator, Construction)
{
    net::RedisOperator op(1, "mykey", "SET mykey %s", "GET mykey");
    (void)op;
    SUCCEED();
}

TEST(RedisOperator, AddRedisFieldBasic)
{
    net::RedisOperator op(1, "test_key", "HSET test_key %s", "HGET test_key %s");
    EXPECT_TRUE(op.AddRedisField("field1", "value1"));
}

TEST(RedisOperator, AddRedisFieldMultipleTypes)
{
    net::RedisOperator op(1, "k", "SET k %s", "GET k");
    EXPECT_TRUE(op.AddRedisField("f1", 42));
    EXPECT_TRUE(op.AddRedisField("f2", 100u));
    EXPECT_TRUE(op.AddRedisField("f3", 999999LL));
    EXPECT_TRUE(op.AddRedisField("f4", 888888ULL));
    EXPECT_TRUE(op.AddRedisField("f5", 3.14f));
    EXPECT_TRUE(op.AddRedisField("f6", 2.718));
}

TEST(RedisOperator, SetRedisStructure)
{
    net::RedisOperator op(1, "hash_key", "HSET hash_key %s", "HGET hash_key %s");
    op.SetRedisStructure(1); // e.g. hash structure
}

TEST(RedisOperator, MakeMemOperate)
{
    net::RedisOperator op(1, "rk", "SET rk %s", "");
    DataMem::MemOperate* memOp = op.MakeMemOperate();
    EXPECT_NE(nullptr, memOp);
}

TEST(RedisOperator, AddPinelineCmd)
{
    net::RedisOperator op(1, "pk", "SET pk %s", "GET pk");
    EXPECT_TRUE(op.AddPinelineCmd("INCR counter"));
    EXPECT_EQ(1u, op.PinelineCmdSize());
}

TEST(RedisOperator, AddMultiplePinelineCmds)
{
    net::RedisOperator op(1, "mk", "SET mk %s", "GET mk");
    op.AddPinelineCmd("SET a 1");
    op.AddPinelineCmd("SET b 2");
    op.AddPinelineCmd("SET c 3");
    EXPECT_EQ(3u, op.PinelineCmdSize());
}

TEST(RedisOperator, AddPinelineCmdWithFormat)
{
    net::RedisOperator op(1, "fmt_k", "SET fmt_k %s", "GET fmt_k");
    EXPECT_TRUE(op.AddPinelineCmd("HSET %s %s %s", "hashkey", "field", "val"));
    EXPECT_EQ(1u, op.PinelineCmdSize());
    EXPECT_NE(std::string::npos, op.m_vecPinelineCmds[0].find("HSET"));
}

TEST(RedisOperator, EmptyPinelineCmdIgnored)
{
    net::RedisOperator op(1, "ek", "SET ek %s", "GET ek");
    EXPECT_TRUE(op.AddPinelineCmd(""));
    EXPECT_EQ(0u, op.PinelineCmdSize());
}

TEST(RedisOperator, SectionFactor)
{
    net::RedisOperator op(8, "sk", "SET sk %s", "GET sk");
    // section factor 8
}
