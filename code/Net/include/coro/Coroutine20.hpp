#ifndef SRC_Coroutine20_HPP_
#define SRC_Coroutine20_HPP_

#include <coroutine>
#include <exception>
#include <cstdio>

namespace net
{

class StepCo20;
/** StepCo20.cpp / 测试桩：return_void 里调用 NotifyEmitCoroutineSuccess */
void async_task_promise_notify_if(StepCo20* step) noexcept;

/**
 * @brief C++20 协程任务类型。首参固定为 `StepCo20&`，后可接业务参数。用 `co_await` 挂起，`co_return` 结束。
 *
 * 用法:
 *   net::AsyncTask MyCo(net::StepCo20& step, int arg) {
 *       bool ok = co_await step.HttpGetAsync("http://...");
 *       step.ResponseToClient(ok ? 200 : 500, "...");
 *       co_return;
 *   }
 */
struct AsyncTask
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit AsyncTask(handle_type h) : coro_(h) {}
    AsyncTask(const AsyncTask&) = delete;
    explicit AsyncTask(AsyncTask&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
    ~AsyncTask() { if (coro_) coro_.destroy(); }

    AsyncTask& operator=(const AsyncTask&) = delete;
    AsyncTask& operator=(AsyncTask&& other) noexcept
    {
        if (this != &other)
        {
            if (coro_) coro_.destroy();
            coro_ = other.coro_;
            other.coro_ = nullptr;
        }
        return *this;
    }

    struct promise_type
    {
        StepCo20* stepAutoNotify_{nullptr};

        template<typename... ExtraArgs>
        promise_type(StepCo20& step, ExtraArgs&&...) noexcept : stepAutoNotify_(&step) {}

        AsyncTask get_return_object() { return AsyncTask{handle_type::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        void return_void() noexcept { async_task_promise_notify_if(stepAutoNotify_); }
        void unhandled_exception() noexcept
        {
            std::fprintf(stderr, "Exception escaping net::AsyncTask\n");
            std::terminate();
        }
        std::suspend_always final_suspend() noexcept { return {}; }
    };

    handle_type native_handle() const noexcept { return coro_; }

private:
    handle_type coro_; ///< 本 AsyncTask 协程帧（非父协程）
};

} // namespace net

#endif // SRC_Coroutine20_HPP_