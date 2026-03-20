#pragma once

#include "NetDefine.hpp"
#include <chrono>
#include <coroutine>

struct ev_loop;
struct ev_timer;

namespace net {

class SleepForAwaiter {
public:
    SleepForAwaiter(ev_tstamp seconds, struct ev_loop* loop) : seconds_(seconds), loop_(loop) {}

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle);

private:
    ev_tstamp seconds_;
    struct ev_loop* loop_;
    static void TimerCallback(struct ev_loop* loop, ev_timer* w, int revents);
};

inline SleepForAwaiter SleepFor(std::chrono::milliseconds ms, struct ev_loop* loop) {
    return SleepForAwaiter(static_cast<ev_tstamp>(ms.count()) / 1000.0, loop);
}

} // namespace net
