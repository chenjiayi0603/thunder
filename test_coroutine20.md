# C++20 协程迁移测试文档

## 概述

本迁移任务旨在将 Thunder 框架中的 StepState 状态机模式迁移到 C++20 协程模式，使用 Drogon 框架的协程基础设施。

## 已完成的工作

### 1. 创建 C++20 协程基础设施
- **Coroutine20.hpp**: 基于 Drogon 框架的协程基础设施，提供了 `Task<>` 和 `AsyncTask` 类型
- 支持 `co_await` 语法，使异步代码看起来像同步代码

### 2. 创建 StepCo20 基类
- **StepCo20.hpp/cpp**: 基于 HttpStep 的 C++20 协程步骤基类
- 提供了协程生命周期管理
- 支持异步 HTTP 操作（HttpGetAsync, HttpPostAsync, SendToAsync）
- 内置超时和重试机制

### 3. 创建示例实现
- **StepHttpRequestCo20.hpp/cpp**: 展示如何使用 StepCo20 的示例
- 使用 `co_await` 语法编写异步 HTTP 请求链
- 代码更加简洁，逻辑更清晰

### 4. 集成到 Hello 模块
- 更新了 ModuleHello.cpp，添加了 `TestHttpRequestCo20` 测试选项
- 可以通过 HTTP 请求触发协程测试

## 使用方法

### 1. 测试 C++20 协程 HTTP 请求

发送 HTTP POST 请求到 Hello 模块：

```bash
curl -X POST http://localhost:27008/hello \
  -H "Content-Type: application/json" \
  -d '{"option": "TestHttpRequestCo20"}'
```

### 2. 与传统 StepState 对比

#### 传统 StepState 模式（状态机）：
```cpp
bool StepHttpRequestState::Emit(int iErrno, const std::string& strErrMsg, const std::string& strErrShow)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (0 != iErrno)
    {
        LOG4_ERROR("%s()", strErrMsg.c_str());
        return false;
    }
    switch (m_uiStep)
    {
        case 0:
            return HttpGet("http://www.baidu.com/");
        case 1:
            return HttpGet("http://www.sogou.com/");
        case 2:
            return HttpGet("http://www.alipay.com/");
        case 3:
            return SendToClient();
        default:
            return false;
    }
}
```

#### C++20 协程模式：
```cpp
net::Task<> StepHttpRequestCo20::CoroutineMain()
{
    LOG4_TRACE("%s() start", __FUNCTION__);
    
    try
    {
        // 状态0: 请求百度
        bool bSuccess = co_await HttpGetAsync("http://www.baidu.com/");
        if (!bSuccess) { Response(1); co_return; }
        
        // 状态1: 请求搜狗
        bSuccess = co_await HttpGetAsync("http://www.sogou.com/");
        if (!bSuccess) { Response(1); co_return; }
        
        // 状态2: 请求支付宝
        bSuccess = co_await HttpGetAsync("http://www.alipay.com/");
        if (!bSuccess) { Response(1); co_return; }
        
        // 状态3: 完成
        Response(0);
    }
    catch (const std::exception& e)
    {
        LOG4_ERROR("%s() exception: %s", __FUNCTION__, e.what());
        Response(1);
    }
    
    co_return;
}
```

### 3. 优势对比

| 特性 | StepState 状态机 | C++20 协程 |
|------|-----------------|------------|
| 代码可读性 | 需要手动管理状态变量 | 类似同步代码，逻辑清晰 |
| 错误处理 | 需要在每个状态中处理 | 可以使用 try-catch 统一处理 |
| 异步操作 | 需要回调函数 | 使用 co_await 等待结果 |
| 调试难度 | 状态跳转难以跟踪 | 线性执行，易于调试 |
| 代码维护 | 状态逻辑分散 | 逻辑集中，易于维护 |

## 编译要求

需要支持 C++20 标准的编译器：
- GCC 10+ 或 Clang 10+ 支持协程
- MSVC 2019+ 支持协程

需要在 CMakeLists.txt 中添加 C++20 支持：
```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

## 下一步工作

### 1. 创建更多 awaitable 包装器
- Redis 操作包装器
- 数据库操作包装器
- 消息发送包装器

### 2. 更新 CMakeLists.txt
- 确保项目启用 C++20 标准
- 添加必要的编译选项

### 3. 性能测试
- 对比协程模式和状态机模式的性能
- 测试内存使用情况
- 测试并发处理能力

### 4. 逐步迁移现有代码
- 识别适合迁移的 StepState 实现
- 分批次迁移，确保兼容性
- 提供迁移指南和示例

## 注意事项

1. **协程生命周期**: 协程对象需要在堆上分配，确保生命周期正确
2. **异常安全**: 使用 try-catch 包装协程体，避免未捕获异常
3. **资源管理**: 确保协程中的资源正确释放
4. **线程安全**: 协程可能在任意线程恢复，需要注意线程安全

## 参考文档

1. [C++20 协程规范](https://en.cppreference.com/w/cpp/language/coroutines)
2. [Drogon 协程实现](https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/utils/coroutine.h)
3. [Thunder 框架文档](thunder/README.md)