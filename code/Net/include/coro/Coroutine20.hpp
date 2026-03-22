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
 * @note 可 co_await；coro_ 始终为本 Task 对应协程帧句柄。外层 co_await 本 Task 时，该帧为子协程，
 *       父协程句柄由 task_awaiter::await_suspend 写入 promise.continuation_，final_suspend 时再对称转移回去。
 */
template<typename T>
struct [[nodiscard]] Task
{
    using value_type = T;
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
private:
    handle_type coro_; ///< 本 Task 对应协程帧；父/等待者句柄在 promise.continuation_
public:
    /// 调用时机：本条协程帧分配后，编译器调 `promise_type::get_return_object()` 时构造；`h` 为 `from_promise(*this)`，指向本帧。用户代码不显式调此构造函数。
    Task(handle_type h) : coro_(h) {}
    Task(const Task&) = delete; ///< 禁止拷贝：一帧只能由一个 `Task` 持有，所有权仅可移动转移。
    /// 转移帧所有权；源 Task 的 coro_ 置空以免 double-destroy。
    Task(Task&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
    /// 最后一处持有者析构时销毁协程帧（须已跑完或未再 resume，避免与运行中帧冲突）。
    ~Task() { if (coro_) coro_.destroy(); }

    Task& operator=(const Task&) = delete;
    /// 先 destroy 当前帧再接管 other；与移动构造语义一致。
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

    /**
     * @brief Task<T> 的 promise
     * @note promise_type 藏在本条协程堆帧里，管挂起、存结果、异常、收尾还给谁；不是 awaiter；父子各一份、不混用
     * @note 流程概要：get_return_object 绑定帧 → initial_suspend 先挂起 → 经 resume 进入协程体
      */
    struct promise_type
    {
        /// 帧与 promise 就绪后由编译器调用，构造返回给调用方的 Task。
        Task<T> get_return_object() { return Task<T>{handle_type::from_promise(*this)}; }
        /// 协程入口：始终先挂起，避免同步跑完协程体才回到调用方。
        std::suspend_always initial_suspend() noexcept { return {}; }

        /// co_return expr 时、进入 final_suspend 之前写入结果。
        template<typename U>
        void return_value(U&& value)
        {
            value_.emplace(std::forward<U>(value));
        }

        /// 协程体内未捕获异常逃出时由运行时调用。
        void unhandled_exception() { exception_ = std::current_exception(); }

        /// 协程体结束后的最后一挂；通过内联 final_awaiter 把恢复目标交给 continuation_。
        auto final_suspend() noexcept
        {
            //         final_awaiter（协程收尾）
            // 用在 promise 的 final_suspend 里，不是普通「等 IO」awaiter。
            // await_suspend 收到的 h 是「即将结束的这条子协程」；常返回 continuation_，把执行权对称转移回父协程。
            struct final_awaiter
            {
                // await_ready()
                // 当前协程已执行到 co_await 这一行，同步调用。
                // true  → 不挂起：跳过 await_suspend，接着在同一次求值里调 await_resume()，其返回值即 co_await 的结果。
                // false → 需要挂起：接着调 await_suspend。
                bool await_ready() noexcept { return false; }
                //             await_suspend( std::coroutine_handle<> h )
                // 仅在 await_ready 为 false 时调用。
                // h 是「马上要因本次 co_await 而挂起」的这条协程帧（常用来登记父/续体，或交给调度器去跑别的）。
                // 从这里起本协程挂起，直到别处对 h（或对称转移返回的句柄）resume。
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
                {
                    return h.promise().continuation_;
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{};
        }

        /// 供外层 co_await 的 await_resume 调用：有异常则重抛，否则移出返回值。
        T result()
        {
            if (exception_) std::rethrow_exception(exception_);
            return std::move(value_.value());
        }

        /// 仅在外层 co_await 本 Task 时由 task_awaiter::await_suspend 调用，登记父句柄供 final_suspend 跳回。
        void set_continuation(std::coroutine_handle<> h) { continuation_ = h; }

        std::optional<T> value_;
        std::exception_ptr exception_;
        std::coroutine_handle<> continuation_ = std::noop_coroutine(); ///< 父协程句柄；无 co_await 时多为 noop_coroutine()
    };

    /**
     * @brief 外层 co_await Task<T> 时的 awaitable
     * @note **使用位置在父协程**：源码里 `co_await child_task` 写在父协程体中；`task_awaiter` 在该挂起点求值，用于挂起**父**帧并 resume **子**帧。
     *       子协程体内不会出现 `task_awaiter` 类型名。await_ready / await_suspend / await_resume 的调用方上下文均为父帧上的 `co_await` 状态机。
     *       await_ready：子已结束则不必挂起父。await_suspend：登记父句柄 h 并 return 子句柄。await_resume：子 final_suspend 还权后父侧取 result()。
     */
    auto operator co_await() const noexcept
    {
        /// 嵌套在 `Task::operator co_await` 内，不是子协程帧里的类型；随父侧 `co_await` 求值构造，携带**子帧**句柄。
        struct task_awaiter
        {
            handle_type coro_; ///< 被等待的子协程帧；await_suspend(h) 中的 h 为当时挂起的父协程
            // await_ready()
            // 当前协程已执行到 co_await 这一行，同步调用。
            // true  → 不挂起：跳过 await_suspend，接着在同一次求值里调 await_resume()，其返回值即 co_await 的结果。
            // false → 需要挂起：接着调 await_suspend。
            bool await_ready() const noexcept { return !coro_ || coro_.done(); }
            //             await_suspend( std::coroutine_handle<> h )
            // 仅在 await_ready 为 false 时调用。
            // h 是「马上要因本次 co_await 而挂起」的这条协程帧（常用来登记父/续体，或交给调度器去跑别的）。
            // 从这里起本协程挂起，直到别处对 h（或对称转移返回的句柄）resume。
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                coro_.promise().set_continuation(h);
                return coro_;
            }
             //             await_resume()
            // 本协程被 resume 之后，在 co_await 的恢复点调用。
            // 返回值作为整个 co_await 表达式的结果；若有异常，按标准从 await_resume 传出。
            T await_resume() { return coro_.promise().result(); }
        };
        return task_awaiter{coro_};
    }

    /// 非 co_await 路径下驱动协程（如单元测试）；须保证不会在帧存活期间 double-destroy。
    handle_type native_handle() const noexcept { return coro_; }
};

// void 特化版本
template<>
struct [[nodiscard]] Task<void>
{
    using value_type = void;
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
private:
    handle_type coro_; ///< 本 Task 对应协程帧；父/等待者句柄在 promise.continuation_
public:
    /// 同 Task<T>：仅 `get_return_object()` 中 `from_promise` 构造；`h` 为 `from_promise(*this)`，指向本帧。
    Task(handle_type h) : coro_(h) {}
    Task(const Task&) = delete; ///< 禁止拷贝；所有权仅移动。
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
    /**
     * @brief Task<void> 的 promise
     * @note 流程概要：get_return_object 绑定帧 → initial_suspend 先挂起 → 经 resume 进入协程体
     *       → co_return 触发 return_void → final_suspend 通过对称转移回到 continuation_（co_await 时登记的父协程）
     *       → 父协程在 await_resume 中调用 result()。
     */
    struct promise_type
    {
        /// 触发时机：本条协程帧与 promise 就绪后、协程体执行前，由编译器调用；构造对外 `Task<void>`（`from_promise` 绑定本帧）。
        Task<void> get_return_object() { return Task<void>{handle_type::from_promise(*this)}; }
        /// 触发时机：紧接在 get_return_object 之后；`suspend_always` 表示先挂起，须有人 resume 本帧后才进入 `CoroutineMain` 等协程体。
        std::suspend_always initial_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { exception_ = std::current_exception(); }

        
        auto final_suspend() noexcept
        {
            struct final_awaiter
            {
                bool await_ready() noexcept { return false; }
                /// h 为正在结束的本 Task 协程（子帧）句柄；返回 continuation_ 即父协程（或 noop）。
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
        std::coroutine_handle<> continuation_ = std::noop_coroutine(); ///< 等待本 Task 的外层（父）协程句柄
    };

    /**
     * @brief 使 `Task<void>` 可作为 awaitable；返回 awaiter `task_awaiter`（约定见 `Task<T>` 同名注释）。
     * @note **使用在父协程**：`co_await child_task` 写在父体；`task_awaiter` 在父挂起点构造；
     *       `coro_` 为子帧，`await_suspend(h)` 的 `h` 为父帧句柄。
     */
    auto operator co_await() const noexcept
    {
        /// 嵌套在 `Task::operator co_await` 内，非子帧内类型；父侧 `co_await` 时构造，携带子帧句柄 `coro_`。
        struct task_awaiter
        {
            handle_type coro_; ///< 被等待的子帧（本 Task）；`await_suspend(h)` 的 `h` 为父协程句柄

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
};

/**
 * @brief 异步任务类型（无 co_await）
 * @note 与 Task 不同：initial_suspend=suspend_never 立即进入协程体；final_suspend=suspend_always 使帧活到本对象析构。
 *       coro_ 仍为「本协程」帧句柄，不提供 operator co_await，由持有者（如 StepCo20::m_oAsyncBootstrap）管生命周期。
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
        /// 协程体结束后仍挂起在 final，帧由 AsyncTask::coro_ 持有直至析构，避免栈上同步调用期间提前 destroy 帧（见仓库 bugfix 笔记）。
        std::suspend_always final_suspend() noexcept { return {}; }
    };

    handle_type native_handle() const noexcept { return coro_; }

private:
    handle_type coro_; ///< 本 AsyncTask 协程帧（非父协程）
};

} // namespace net

#endif // SRC_Coroutine20_HPP_