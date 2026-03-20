#pragma once

#include "task/HttpResponse.hpp"
#include <coroutine>
#include <string>
#include <unordered_map>

namespace net {

class HttpTask;

class HttpGetAwaiter {
public:
    HttpGetAwaiter(HttpTask* task, std::string url) : task_(task), url_(std::move(url)) {}

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);
    HttpResponse await_resume() const;

private:
    HttpTask* task_;
    std::string url_;
};

class HttpPostAwaiter {
public:
    HttpPostAwaiter(HttpTask* task, std::string url, std::string body,
                      std::unordered_map<std::string, std::string> headers = {});

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);
    HttpResponse await_resume() const;

private:
    HttpTask* task_;
    std::string url_;
    std::string body_;
    std::unordered_map<std::string, std::string> headers_;
};

} // namespace net
