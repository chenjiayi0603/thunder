#include "Coroutine20.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace
{

/// 计划中的 SyncDriver 思路：用 suspend_never 的「外层」协程可在一次栈上深入子 Task。
/// 但 net::Task 使用 initial_suspend=suspend_always，子协程需额外 resume；故单测用 native_handle()
/// 做显式泵送，并在嵌套 co_await 时用测试内静态句柄辅助 pump。

template<typename T>
T run_task_to_value(net::Task<T>& task)
{
    auto h = task.native_handle();
    // 避免在模板内使用 gtest 断言宏（部分环境下对表达式展开不兼容）
    while (h.address() != nullptr && !h.done())
    {
        h.resume();
    }
    return h.promise().result();
}

void run_task_void(net::Task<void>& task)
{
    auto h = task.native_handle();
    while (h.address() != nullptr && !h.done())
    {
        h.resume();
    }
    h.promise().result();
}

} // namespace

// ---- Task<bool> ----

TEST(Coroutine20, Task_bool_returns_value)
{
    auto make = []() -> net::Task<bool> { co_return true; };
    net::Task<bool> t = make();
    EXPECT_TRUE(run_task_to_value(t));
}

TEST(Coroutine20, Task_bool_returns_false)
{
    auto make = []() -> net::Task<bool> { co_return false; };
    net::Task<bool> t = make();
    EXPECT_FALSE(run_task_to_value(t));
}

TEST(Coroutine20, Task_void_runs_to_completion)
{
    static int side = 0;
    side = 0;
    auto make = []() -> net::Task<void> {
        side = 42;
        co_return;
    };
    net::Task<void> t = make();
    run_task_void(t);
    EXPECT_EQ(side, 42);
}

TEST(Coroutine20, Task_bool_exception_propagates)
{
    EXPECT_THROW(
        {
            auto make = []() -> net::Task<bool> { throw std::runtime_error("boom"); };
            net::Task<bool> t = make();
            auto h = t.native_handle();
            while (h.address() != nullptr && !h.done())
            {
                h.resume();
            }
            (void)h.promise().result();
        },
        std::runtime_error);
}

// 嵌套 Task：在挂起点注册子句柄，便于在同一线程 pump
static thread_local std::coroutine_handle<> g_pump_leaf;
static thread_local std::coroutine_handle<> g_pump_sub;

TEST(Coroutine20, Task_chained_two_levels)
{
    auto leaf = []() -> net::Task<int> { co_return 40; };
    auto sub = [&]() -> net::Task<int> {
        net::Task<int> L = leaf();
        g_pump_leaf = L.native_handle();
        int x = co_await std::move(L);
        g_pump_leaf = {};
        co_return x + 1;
    };
    auto top = [&]() -> net::Task<int> {
        net::Task<int> S = sub();
        g_pump_sub = S.native_handle();
        int y = co_await std::move(S);
        g_pump_sub = {};
        co_return y + 1;
    };

    net::Task<int> t = top();
    auto h = t.native_handle();
    for (int i = 0; i < 64 && h.address() != nullptr && !h.done(); ++i)
    {
        h.resume();
        if (g_pump_leaf && !g_pump_leaf.done())
        {
            g_pump_leaf.resume();
        }
        if (g_pump_sub && !g_pump_sub.done())
        {
            g_pump_sub.resume();
        }
    }
    ASSERT_TRUE(h.done());
    EXPECT_EQ(h.promise().result(), 42);
}

// AsyncTask 仅 `AsyncTask(StepCo20&)`，见 thunder_test_step_co20。

TEST(Coroutine20, Task_move_semantics)
{
    auto make = []() -> net::Task<bool> { co_return true; };
    net::Task<bool> a = make();
    auto ha = a.native_handle();
    ASSERT_NE(ha.address(), nullptr);
    net::Task<bool> b = std::move(a);
    EXPECT_EQ(a.native_handle().address(), nullptr);
    EXPECT_EQ(b.native_handle().address(), ha.address());
    EXPECT_TRUE(run_task_to_value(b));
}
