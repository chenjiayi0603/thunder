#pragma once

#include <coroutine>
#include <cstdint>
#include <string>
#include <vector>

namespace net {

class BaseTask;

struct RedisResponse {
    enum class Type { NIL, STRING, INTEGER, ARRAY, ERROR };
    Type type = Type::NIL;
    std::string str;
    int64_t integer = 0;
    std::vector<RedisResponse> elements;
    int errNo = 0;
    std::string errMsg;

    bool Ok() const { return type != Type::ERROR && errNo == 0; }
    bool IsNil() const { return type == Type::NIL; }
};

class RedisCommandAwaiter {
public:
    RedisCommandAwaiter(BaseTask* task, std::string host, int port, std::string command)
        : task_(task), host_(std::move(host)), port_(port), command_(std::move(command)) {}

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);
    RedisResponse await_resume() const;

private:
    BaseTask* task_;
    std::string host_;
    int port_;
    std::string command_;
};

class RedisCoHelper {
public:
    explicit RedisCoHelper(BaseTask* task, std::string host = "127.0.0.1", int port = 6379)
        : task_(task), host_(std::move(host)), port_(port) {}

    RedisCommandAwaiter Get(const std::string& key);
    RedisCommandAwaiter Set(const std::string& key, const std::string& value);
    RedisCommandAwaiter Del(const std::string& key);
    RedisCommandAwaiter HGet(const std::string& key, const std::string& field);
    RedisCommandAwaiter HSet(const std::string& key, const std::string& field, const std::string& value);
    RedisCommandAwaiter Expire(const std::string& key, int seconds);
    RedisCommandAwaiter Execute(const std::string& command);

private:
    BaseTask* task_;
    std::string host_;
    int port_;
};

} // namespace net
