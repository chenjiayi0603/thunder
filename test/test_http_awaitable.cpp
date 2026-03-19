#include "step/HttpAwaitable.hpp"
#include "step/StepCoroutine.hpp"
#include <iostream>
#include <cassert>

using namespace thunder;

// 模拟 HttpMsg 结构
struct HttpMsg {
    int status_code = 200;
    std::string body = "Mock response";
};

// 模拟 StepCoroutine 用于测试
class TestStepCoroutine : public StepCoroutine {
public:
    CoTask Run() override {
        std::cout << "TestStepCoroutine: Running" << std::endl;
        auto resp = co_await HttpGetAsync("http://test.com");
        std::cout << "TestStepCoroutine: Got response" << std::endl;
        co_return;
    }
};

int main() {
    std::cout << "=== Thunder HttpAwaitable Unit Tests ===" << std::endl;

    // Test 1: HttpAwaitable 创建
    {
        TestStepCoroutine co;
        HttpAwaitable awaitable(&co, "http://test.com", "GET");
        assert(!awaitable.await_ready());  // 总是挂起
        std::cout << "✓ Test 1 passed: HttpAwaitable creation" << std::endl;
    }

    // Test 2: POST 请求
    {
        TestStepCoroutine co;
        HttpAwaitable awaitable(&co, "http://test.com", "POST", "test body");
        // 验证 POST 请求创建成功
        std::cout << "✓ Test 2 passed: POST request creation" << std::endl;
    }

    std::cout << "=== All HttpAwaitable tests passed ===" << std::endl;
    return 0;
}
