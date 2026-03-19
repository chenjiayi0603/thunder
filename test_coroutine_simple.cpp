#include <iostream>
#include <coroutine>
#include <thread>
#include <chrono>

// 简单的协程示例
struct SimpleTask {
    struct promise_type {
        SimpleTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

SimpleTask simple_coroutine() {
    std::cout << "协程开始执行" << std::endl;
    co_await std::suspend_always{};
    std::cout << "协程恢复执行" << std::endl;
}

// 测试 C++20 协程功能
int main() {
    std::cout << "测试 C++20 协程功能" << std::endl;
    std::cout << "编译器支持 C++20 协程" << std::endl;
    
    // 测试协程类型
    std::cout << "std::coroutine_handle 大小: " << sizeof(std::coroutine_handle<>) << std::endl;
    std::cout << "std::suspend_always 大小: " << sizeof(std::suspend_always) << std::endl;
    std::cout << "std::suspend_never 大小: " << sizeof(std::suspend_never) << std::endl;
    
    return 0;
}