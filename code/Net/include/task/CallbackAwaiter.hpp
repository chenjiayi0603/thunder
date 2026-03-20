#pragma once

#include <coroutine>
#include <exception>
#include <optional>

namespace net {

template <typename T>
struct CallbackAwaiter {
    bool await_ready() noexcept { return false; }

    const T& await_resume() const {
        if (exception_) std::rethrow_exception(exception_);
        return result_.value();
    }

protected:
    void setValue(T&& v) { result_.emplace(std::move(v)); }
    void setValue(const T& v) { result_.emplace(v); }
    void setException(const std::exception_ptr& e) { exception_ = e; }

private:
    std::optional<T> result_;
    std::exception_ptr exception_{nullptr};
};

template <>
struct CallbackAwaiter<void> {
    bool await_ready() noexcept { return false; }

    void await_resume() const {
        if (exception_) std::rethrow_exception(exception_);
    }

protected:
    void setException(const std::exception_ptr& e) { exception_ = e; }

private:
    std::exception_ptr exception_{nullptr};
};

} // namespace net
