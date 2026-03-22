#ifndef SRC_Coroutine20_HPP_
#define SRC_Coroutine20_HPP_

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
#include <cstdio>

namespace net
{

// Forward declarations
template<typename T = void>
struct Task;

namespace detail
{
    template<typename T>
    struct TaskPromiseBase;
}

/**
 * @brief C++20 协程 Task 类型
 * @note 基于 C++20 协程的异步任务类型，支持 co_await 语法
 */
template<typename T>
struct [[nodiscard]] Task
{
    using value_type = T;
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    Task(handle_type h) : coro_(h) {}
    Task(const Task&) = delete;
    Task(Task&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
    ~Task() { if (coro_) coro_.destroy(); }

    Task& operator=(const Task&) = delete;
    Task& operator=(Task&& other) noexcept
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
        Task<T> get_return_object() { return Task<T>{handle_type::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        
        template<typename U>
        void return_value(U&& value)
        {
            value_.emplace(std::forward<U>(value));
        }
        
        void unhandled_exception() { exception_ = std::current_exception(); }
        
        auto final_suspend() noexcept
        {
            struct final_awaiter
            {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    return h.promise().continuation_;
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{};
        }
        
        T result()
        {
            if (exception_) std::rethrow_exception(exception_);
            return std::move(value_.value());
        }
        
        void set_continuation(std::coroutine_handle<> h) { continuation_ = h; }
        
        std::optional<T> value_;
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_ = std::noop_coroutine();
    };

    auto operator co_await() const noexcept
    {
        struct task_awaiter
        {
            handle_type coro_;
            
            bool await_ready() const noexcept { return !coro_ || coro_.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                coro_.promise().set_continuation(h);
                return coro_;
            }
            T await_resume() { return coro_.promise().result(); }
        };
        return task_awaiter{coro_};
    }

    /// 非 co_await 路径下驱动协程（如单元测试）；须保证不会在帧存活期间 double-destroy。
    handle_type native_handle() const noexcept { return coro_; }

private:
    handle_type coro_;
};

// void 特化版本
template<>
struct [[nodiscard]] Task<void>
{
    using value_type = void;
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    Task(handle_type h) : coro_(h) {}
    Task(const Task&) = delete;
    Task(Task&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
    ~Task() { if (coro_) coro_.destroy(); }

    Task& operator=(const Task&) = delete;
    Task& operator=(Task&& other) noexcept
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
        Task<void> get_return_object() { return Task<void>{handle_type::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception_ = std::current_exception(); }
        
        auto final_suspend() noexcept
        {
            struct final_awaiter
            {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    return h.promise().continuation_;
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{};
        }
        
        void result()
        {
            if (exception_) std::rethrow_exception(exception_);
        }
        
        void set_continuation(std::coroutine_handle<> h) { continuation_ = h; }
        
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_ = std::noop_coroutine();
    };

    auto operator co_await() const noexcept
    {
        struct task_awaiter
        {
            handle_type coro_;
            
            bool await_ready() const noexcept { return !coro_ || coro_.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                coro_.promise().set_continuation(h);
                return coro_;
            }
            void await_resume() { coro_.promise().result(); }
        };
        return task_awaiter{coro_};
    }

    handle_type native_handle() const noexcept { return coro_; }

private:
    handle_type coro_;
};

/**
 * @brief 异步任务类型，不等待结果
 * @note 用于启动协程但不等待其完成
 */
struct AsyncTask
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    AsyncTask(handle_type h) : coro_(h) {}
    AsyncTask(const AsyncTask&) = delete;
    AsyncTask(AsyncTask&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
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
        AsyncTask get_return_object() { return AsyncTask{handle_type::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() noexcept
        {
            std::fprintf(stderr, "Exception escaping net::AsyncTask\n");
            std::terminate();
        }
        // suspend_always so the frame stays alive until the owning AsyncTask is destroyed,
        // preventing the double-destroy that occurs when the owner resets the AsyncTask handle
        // while the coroutine is still executing a synchronous call on the real stack.
        std::suspend_always final_suspend() noexcept { return {}; }
    };

    handle_type native_handle() const noexcept { return coro_; }

private:
    handle_type coro_;
};

} // namespace net

#endif // SRC_Coroutine20_HPP_