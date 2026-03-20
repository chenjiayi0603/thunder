#include "task/SleepFor.hpp"
#include <ev.h>
#include <new>

namespace net {

struct SleepTimerData {
    std::coroutine_handle<> h;
    ev_timer* timer;
};

void SleepForAwaiter::TimerCallback(struct ev_loop* loop, ev_timer* w, int) {
    auto* d = static_cast<SleepTimerData*>(w->data);
    ev_timer_stop(loop, w);
    delete w;
    auto h = d->h;
    delete d;
    if (h && !h.done()) h.resume();
}

void SleepForAwaiter::await_suspend(std::coroutine_handle<> handle) {
    auto* d = new (std::nothrow) SleepTimerData{handle, nullptr};
    if (!d || !loop_) {
        if (handle && !handle.done()) handle.resume();
        return;
    }
    d->timer = new (std::nothrow) ev_timer();
    if (!d->timer) {
        delete d;
        if (handle && !handle.done()) handle.resume();
        return;
    }
    ev_timer_init(d->timer, TimerCallback, seconds_, 0.0);
    d->timer->data = d;
    ev_timer_start(loop_, d->timer);
}

} // namespace net
