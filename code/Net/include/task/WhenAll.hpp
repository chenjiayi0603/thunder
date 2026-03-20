#pragma once

#include "step/Coroutine20.hpp"
#include "task/HttpResponse.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <tuple>
#include <utility>

namespace net {

namespace detail {

/** 并发等待两个 Task<HttpResponse>（简化实现，满足常见 HTTP 组合场景） */
struct WhenAllHttpResponseAwaiter {
    Task<HttpResponse> t0_;
    Task<HttpResponse> t1_;
    std::tuple<HttpResponse, HttpResponse> results_{};
    std::atomic<int> remaining_{2};
    std::coroutine_handle<> continuation_{};
    std::exception_ptr exception_{};

    WhenAllHttpResponseAwaiter(Task<HttpResponse>&& a, Task<HttpResponse>&& b)
        : t0_(std::move(a)), t1_(std::move(b)) {}

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        continuation_ = h;
        [](WhenAllHttpResponseAwaiter* self) -> AsyncTask {
            try {
                std::get<0>(self->results_) = co_await std::move(self->t0_);
            } catch (...) {
                self->exception_ = std::current_exception();
            }
            if (self->remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                self->continuation_.resume();
            }
        }(this);
        [](WhenAllHttpResponseAwaiter* self) -> AsyncTask {
            try {
                std::get<1>(self->results_) = co_await std::move(self->t1_);
            } catch (...) {
                self->exception_ = std::current_exception();
            }
            if (self->remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                self->continuation_.resume();
            }
        }(this);
    }

    std::tuple<HttpResponse, HttpResponse> await_resume() {
        if (exception_) std::rethrow_exception(exception_);
        return std::move(results_);
    }
};

} // namespace detail

inline auto when_all(Task<HttpResponse>&& a, Task<HttpResponse>&& b) {
    return detail::WhenAllHttpResponseAwaiter(std::move(a), std::move(b));
}

} // namespace net
