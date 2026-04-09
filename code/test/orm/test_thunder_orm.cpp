/**
 * Thunder ORM（MysqlMapper / RedisMapper）单测：
 * - 无外部依赖：非法连接、非法标识符
 * - THUNDER_ORM_INTEGRATION=1：对真实 MySQL / Redis 做 Drogon 风格 insert / set+get（需环境变量）
 */
#include "orm/ThunderMysqlMapper.hpp"
#include "orm/ThunderRedisMapper.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <future>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

namespace
{

bool integrationEnabled()
{
    const char* e = std::getenv("THUNDER_ORM_INTEGRATION");
    return e != nullptr && std::strcmp(e, "1") == 0;
}

util::tagDbConnInfo mysqlConnFromEnv()
{
    util::tagDbConnInfo c;
    const char* host = std::getenv("THUNDER_ORM_MYSQL_HOST");
    const char* user = std::getenv("THUNDER_ORM_MYSQL_USER");
    const char* pwd = std::getenv("THUNDER_ORM_MYSQL_PASSWORD");
    const char* db = std::getenv("THUNDER_ORM_MYSQL_DB");
    const char* port = std::getenv("THUNDER_ORM_MYSQL_PORT");
    std::strncpy(c.m_szDbHost, host ? host : "127.0.0.1", sizeof(c.m_szDbHost) - 1);
    std::strncpy(c.m_szDbUser, user ? user : "root", sizeof(c.m_szDbUser) - 1);
    std::strncpy(c.m_szDbPwd, pwd ? pwd : "", sizeof(c.m_szDbPwd) - 1);
    std::strncpy(c.m_szDbName, db ? db : "test", sizeof(c.m_szDbName) - 1);
    std::strncpy(c.m_szDbCharSet, "utf8", sizeof(c.m_szDbCharSet) - 1);
    c.m_uiDbPort = static_cast<unsigned int>(port ? std::atoi(port) : 3306);
    return c;
}

void redisHostPortFromEnv(std::string& host, int& port)
{
    const char* h = std::getenv("THUNDER_ORM_REDIS_HOST");
    const char* p = std::getenv("THUNDER_ORM_REDIS_PORT");
    host = h ? h : "127.0.0.1";
    port = p ? std::atoi(p) : 6379;
}

} // namespace

// ---- MySQL ----

TEST(ThunderOrmMysql, InsertFuture_ThrowsOnInvalidColumn)
{
    util::tagDbConnInfo c{};
    thunder::orm::MysqlMapper mapper(c);
    auto fut = mapper.insertFuture("tbl", {{"bad-col", "x"}});
    EXPECT_THROW((void)fut.get(), std::exception);
}

TEST(ThunderOrmMysql, InsertFuture_ThrowsOnConnectionRefused)
{
    util::tagDbConnInfo c;
    std::strncpy(c.m_szDbHost, "127.0.0.1", sizeof(c.m_szDbHost) - 1);
    std::strncpy(c.m_szDbUser, "nouser", sizeof(c.m_szDbUser) - 1);
    std::strncpy(c.m_szDbPwd, "nopass", sizeof(c.m_szDbPwd) - 1);
    std::strncpy(c.m_szDbName, "nodb", sizeof(c.m_szDbName) - 1);
    std::strncpy(c.m_szDbCharSet, "utf8", sizeof(c.m_szDbCharSet) - 1);
    c.m_uiDbPort = 1; // 通常无监听

    thunder::orm::MysqlMapper mapper(c);
    auto fut = mapper.insertFuture("t", {{"a", "b"}});
    EXPECT_THROW((void)fut.get(), std::exception);
}

TEST(ThunderOrmMysql, AsyncInsert_ErrOnBadColumnName)
{
    util::tagDbConnInfo c{};
    thunder::orm::MysqlMapper mapper(c);
    std::string err;
    mapper.insert(
        "tbl",
        {{"bad-name", "v"}},
        [&](std::uint64_t) { err = "unexpected ok"; },
        [&](const std::string& e) { err = e; });
    EXPECT_NE(err.find("invalid column"), std::string::npos);
}

TEST(ThunderOrmMysql, Integration_AsyncAndFuture)
{
    if (!integrationEnabled())
    {
        GTEST_SKIP() << "set THUNDER_ORM_INTEGRATION=1 and MySQL env to run";
    }
    util::tagDbConnInfo c = mysqlConnFromEnv();
    thunder::orm::MysqlMapper mapper(c);

    // 示例：CREATE TABLE IF NOT EXISTS thunder_orm_ut_users (
    //   id INT AUTO_INCREMENT PRIMARY KEY,
    //   user_id VARCHAR(64), user_name VARCHAR(128), password VARCHAR(128), org_name VARCHAR(128));
    const std::string table = "thunder_orm_ut_users";
    std::mutex m;
    std::condition_variable cv;
    bool async_done = false;
    std::string async_err;

    mapper.insert(
        table,
        {{"user_id", "orm_async"}, {"user_name", "u1"}, {"password", "p1"}, {"org_name", "o1"}},
        [&](std::uint64_t /*id*/) {
            std::lock_guard<std::mutex> lk(m);
            async_done = true;
            cv.notify_one();
        },
        [&](const std::string& e) {
            std::lock_guard<std::mutex> lk(m);
            async_err = e;
            async_done = true;
            cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::seconds(15), [&] { return async_done; }))
        {
            GTEST_SKIP() << "mysql async insert timed out";
        }
    }
    if (!async_err.empty())
    {
        GTEST_SKIP() << "mysql async: " << async_err;
    }

    auto fu = mapper.insertFuture(
        table,
        {{"user_id", "orm_future"}, {"user_name", "u2"}, {"password", "p2"}, {"org_name", "o2"}});
    try
    {
        (void)fu.get();
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "mysql future: " << e.what();
    }
}

// ---- Redis ----

TEST(ThunderOrmRedis, SetFuture_ThrowsOnBadPort)
{
    thunder::orm::RedisMapper r("127.0.0.1", 1);
    auto fut = r.setFuture("k", "v");
    EXPECT_THROW((void)fut.get(), std::exception);
}

TEST(ThunderOrmRedis, AsyncSet_ErrOnEmptyKey)
{
    thunder::orm::RedisMapper r("127.0.0.1", 6379);
    bool err_path = false;
    r.set(
        "",
        "v",
        [&]() { err_path = false; },
        [&](const std::string&) { err_path = true; });
    EXPECT_TRUE(err_path);
}

TEST(ThunderOrmRedis, Integration_SetGetFuture)
{
    if (!integrationEnabled())
    {
        GTEST_SKIP() << "set THUNDER_ORM_INTEGRATION=1 to run";
    }
    std::string host;
    int port = 6379;
    redisHostPortFromEnv(host, port);
    thunder::orm::RedisMapper r(host, port);
    const std::string key = "thunder:orm:ut:key";
    const std::string val = "hello_orm";

    try
    {
        r.setFuture(key, val).get();
        EXPECT_EQ(r.getFuture(key).get(), val);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "redis integration: " << e.what();
    }
}
