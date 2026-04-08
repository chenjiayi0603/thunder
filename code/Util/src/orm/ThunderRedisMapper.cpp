#include "ThunderRedisMapper.hpp"

#include "hiredis-vip/hiredis.h"

#include <future>
#include <stdexcept>
#include <thread>

namespace thunder::orm
{

namespace
{

bool redisKeyOk(const std::string& key, std::string& err_out)
{
    err_out.clear();
    if (key.empty())
    {
        err_out = "empty key";
        return false;
    }
    for (unsigned char c : key)
    {
        if (c <= ' ' || c == '\r' || c == '\n')
        {
            err_out = "key contains whitespace/control";
            return false;
        }
    }
    return true;
}

redisContext* connectRedis(const std::string& host, int port, std::string& err_out)
{
    err_out.clear();
    redisContext* c = redisConnect(host.c_str(), port);
    if (c == nullptr)
    {
        err_out = "redisConnect returned null";
        return nullptr;
    }
    if (c->err)
    {
        err_out = std::string(c->errstr);
        redisFree(c);
        return nullptr;
    }
    return c;
}

void runSet(std::string host, int port, std::string key, std::string value, RedisOk on_ok, RedisErr on_err)
{
    std::string e;
    redisContext* c = connectRedis(host, port, e);
    if (!c)
    {
        on_err(e);
        return;
    }
    redisReply* r = static_cast<redisReply*>(redisCommand(
        c,
        "SET %b %b",
        key.data(),
        key.size(),
        value.data(),
        value.size()));
    if (r == nullptr)
    {
        std::string msg = c->err ? std::string(c->errstr) : std::string("redisCommand failed");
        redisFree(c);
        on_err(msg);
        return;
    }
    if (r->type == REDIS_REPLY_ERROR)
    {
        std::string msg = r->str ? r->str : "REDIS_REPLY_ERROR";
        freeReplyObject(r);
        redisFree(c);
        on_err(msg);
        return;
    }
    freeReplyObject(r);
    redisFree(c);
    on_ok();
}

} // namespace

RedisMapper::RedisMapper(std::string host, int port) : host_(std::move(host)), port_(port) {}

void RedisMapper::set(const std::string& key, const std::string& value, RedisOk on_ok, RedisErr on_err)
{
    if (!on_ok || !on_err)
    {
        return;
    }
    std::string e;
    if (!redisKeyOk(key, e))
    {
        on_err(e);
        return;
    }
    std::thread(runSet, host_, port_, key, value, std::move(on_ok), std::move(on_err)).detach();
}

std::future<void> RedisMapper::setFuture(const std::string& key, const std::string& value)
{
    return std::async(std::launch::async, [h = host_, port = port_, key, value]() {
        std::string e;
        if (!redisKeyOk(key, e))
        {
            throw std::runtime_error(e);
        }
        redisContext* c = connectRedis(h, port, e);
        if (!c)
        {
            throw std::runtime_error(e);
        }
        redisReply* r = static_cast<redisReply*>(
            redisCommand(c, "SET %b %b", key.data(), key.size(), value.data(), value.size()));
        if (r == nullptr)
        {
            std::string msg = c->err ? std::string(c->errstr) : std::string("redisCommand failed");
            redisFree(c);
            throw std::runtime_error(msg);
        }
        if (r->type == REDIS_REPLY_ERROR)
        {
            std::string msg = r->str ? r->str : "REDIS_REPLY_ERROR";
            freeReplyObject(r);
            redisFree(c);
            throw std::runtime_error(msg);
        }
        freeReplyObject(r);
        redisFree(c);
    });
}

std::future<std::string> RedisMapper::getFuture(const std::string& key)
{
    return std::async(std::launch::async, [h = host_, port = port_, key]() -> std::string {
        std::string e;
        if (!redisKeyOk(key, e))
        {
            throw std::runtime_error(e);
        }
        redisContext* c = connectRedis(h, port, e);
        if (!c)
        {
            throw std::runtime_error(e);
        }
        redisReply* r =
            static_cast<redisReply*>(redisCommand(c, "GET %b", key.data(), key.size()));
        if (r == nullptr)
        {
            std::string msg = c->err ? std::string(c->errstr) : std::string("redisCommand failed");
            redisFree(c);
            throw std::runtime_error(msg);
        }
        if (r->type == REDIS_REPLY_ERROR)
        {
            std::string msg = r->str ? r->str : "REDIS_REPLY_ERROR";
            freeReplyObject(r);
            redisFree(c);
            throw std::runtime_error(msg);
        }
        if (r->type == REDIS_REPLY_NIL)
        {
            freeReplyObject(r);
            redisFree(c);
            return {};
        }
        if (r->type != REDIS_REPLY_STRING)
        {
            freeReplyObject(r);
            redisFree(c);
            throw std::runtime_error("unexpected redis reply type");
        }
        std::string out(r->str, r->len);
        freeReplyObject(r);
        redisFree(c);
        return out;
    });
}

} // namespace thunder::orm
