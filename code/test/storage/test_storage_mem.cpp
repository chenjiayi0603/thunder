/**
 * MemOperator 存储协议单元测试
 */
#include "NetDefine.hpp"
#include "storage/MemOperator.hpp"
#include <gtest/gtest.h>

TEST(MemOperator, Construction)
{
    net::MemOperator op(
        1,                           // sectionFactor
        "test_table",                 // tableName
        DataMem::MemOperate::DbOperate::SELECT,  // queryType
        "redis_key",                  // redisKey
        "SET redis_key %s",           // writeCmd
        "GET redis_key"               // readCmd
    );
    (void)op;
    SUCCEED();
}

TEST(MemOperator, ConstructionWithModFactor)
{
    net::MemOperator op(
        2,
        "tbl_mod",
        DataMem::MemOperate::DbOperate::INSERT,
        "mod_key",
        "SET mod_key %s",
        "GET mod_key",
        10  // modFactor
    );
    (void)op;
    SUCCEED();
}

TEST(MemOperator, AddField)
{
    net::MemOperator op(1, "users", DataMem::MemOperate::DbOperate::SELECT,
                        "user_key", "HSET user_key %s", "HGET user_key %s");
    EXPECT_TRUE(op.AddField("name", "thunder"));
    EXPECT_TRUE(op.AddField("age", 25));
    EXPECT_TRUE(op.AddField("score", 100u));
    EXPECT_TRUE(op.AddField("balance", 99999LL));
    EXPECT_TRUE(op.AddField("id", 12345ULL));
    EXPECT_TRUE(op.AddField("rate", 0.5f));
    EXPECT_TRUE(op.AddField("percent", 99.9));
}

TEST(MemOperator, ClearFields)
{
    net::MemOperator op(1, "t", DataMem::MemOperate::DbOperate::SELECT,
                        "k", "SET k %s", "GET k");
    op.AddField("f1", "v1");
    op.AddField("f2", "v2");
    op.ClearFields();
}

TEST(MemOperator, AddRedisField)
{
    net::MemOperator op(1, "tbl", DataMem::MemOperate::DbOperate::SELECT,
                        "rk", "SET rk %s", "GET rk");
    EXPECT_TRUE(op.AddRedisField("rf1", "rv1"));
}

TEST(MemOperator, MakeMemOperate)
{
    net::MemOperator op(1, "t", DataMem::MemOperate::DbOperate::INSERT,
                        "k", "SET k %s", "");
    DataMem::MemOperate* mem = op.MakeMemOperate();
    EXPECT_NE(nullptr, mem);
}

TEST(MemOperator, FieldOperatorEnum)
{
    EXPECT_EQ(0x0001, net::FIELD_OPERATOR_DB);
    EXPECT_EQ(0x0002, net::FIELD_OPERATOR_REDIS);
}
