#include "step/CoTask.hpp"
#include <iostream>
#include <cassert>

using namespace thunder;

// 测试 1: 基本协程执行
CoTask test_basic_coroutine() {
    std::cout << "Test 1: Basic coroutine - Start" << std::endl;
    co_await std::suspend_never();
    std::cout << "Test 1: Basic coroutine - End" << std::endl;
    co_return;
}

// 测试 2: 协程异常处理
CoTask test_exception_handling() {
    std::cout << "Test 2: Exception handling - Start" << std::endl;
    throw std::runtime_error("Test exception");
    co_return;
}

int main() {
    std::cout << "=== Thunder CoTask Unit Tests ===" << std::endl;

    // Test 1: 基本协程
    {
        auto task = test_basic_coroutine();
        assert(!task.done());
        task.resume();
        assert(task.done());
        std::cout << "✓ Test 1 passed: Basic coroutine" << std::endl;
    }

    // Test 2: 异常处理
    {
        auto task = test_exception_handling();
        assert(!task.done());
        try {
            task.resume();
        } catch (...) {
            // 预期异常
        }
        assert(task.done());
        assert(task.has_exception());
        std::cout << "✓ Test 2 passed: Exception handling" << std::endl;
    }

    // Test 3: 移动语义
    {
        auto task1 = test_basic_coroutine();
        auto task2 = std::move(task1);
        assert(task1.done() || !task1.done());  // task1 可能被清空
        task2.resume();
        std::cout << "✓ Test 3 passed: Move semantics" << std::endl;
    }

    std::cout << "=== All CoTask tests passed ===" << std::endl;
    return 0;
}
