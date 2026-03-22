/*******************************************************************************
 * Project:  Net
 * @file     coro/Awaitable.hpp
 * @brief    通用 Awaitable 基础设施
 * @author   cjy
 * @date:    2024
 * @note     提供 co_await 支持的基础模板类
 *           用于将各种异步操作（HTTP、Redis、MySQL、Timer等）协程化
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_AWAITABLE_HPP_
#define SRC_STEP_AWAITABLE_HPP_

#include <coroutine>
#include <functional>
#include <optional>
#include <variant>
#include <exception>
#include <chrono>
#include "NetDefine.hpp"

namespace net
{

// 前置声明
class Step;
class StepCo20;

/**
 * @brief 通用 Awaitable 模板类
 * @tparam T 异步操作的返回值类型
 * @note 提供标准的 co_await 接口，支持任意异步操作
 */
template<typename T>
class Awaitable
{
public:
    using ResultType = T;
    using CallbackType = std::function<void(std::coroutine_handle<>)>;
    
    /**
     * @brief 构造函数
     * @param callback 异步操作启动时的回调
     */
    explicit Awaitable(CallbackType callback)
        : m_callback(std::move(callback))
        , m_hasResult(false)
    {
    }
    
    /**
     * @brief 检查是否已准备好
     * @note 返回 false 表示需要挂起协程
     */
    bool await_ready() const noexcept
    {
        return m_hasResult;
    }
    
    /**
     * @brief 挂起协程
     * @param handle 协程句柄，用于后续恢复
     */
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        m_handle = handle;
        if (m_callback)
        {
            m_callback(handle);
        }
    }
    
    /**
     * @brief 恢复协程并获取结果
     * @return 异步操作的结果
     */
    T await_resume()
    {
        if (m_exception)
        {
            std::rethrow_exception(m_exception);
        }
        return std::move(m_result.value());
    }
    
    /**
     * @brief 设置结果
     * @param result 异步操作的结果
     */
    void SetResult(T result)
    {
        m_result = std::move(result);
        m_hasResult = true;
    }
    
    /**
     * @brief 设置异常
     * @param ex 异常指针
     */
    void SetException(std::exception_ptr ex)
    {
        m_exception = ex;
        m_hasResult = true;
    }
    
    /**
     * @brief 恢复协程
     */
    void Resume()
    {
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
    }
    
private:
    CallbackType m_callback;
    std::coroutine_handle<> m_handle;
    std::optional<T> m_result;
    std::exception_ptr m_exception;
    bool m_hasResult;
};

/**
 * @brief void 特化版本
 */
template<>
class Awaitable<void>
{
public:
    using CallbackType = std::function<void(std::coroutine_handle<>)>;
    
    explicit Awaitable(CallbackType callback = nullptr)
        : m_callback(std::move(callback))
        , m_ready(false)
    {
    }
    
    bool await_ready() const noexcept
    {
        return m_ready;
    }
    
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        m_handle = handle;
        if (m_callback)
        {
            m_callback(handle);
        }
    }
    
    void await_resume()
    {
        if (m_exception)
        {
            std::rethrow_exception(m_exception);
        }
    }
    
    void SetReady()
    {
        m_ready = true;
    }
    
    void SetException(std::exception_ptr ex)
    {
        m_exception = ex;
        m_ready = true;
    }
    
    void Resume()
    {
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
    }
    
private:
    CallbackType m_callback;
    std::coroutine_handle<> m_handle;
    std::exception_ptr m_exception;
    bool m_ready;
};

/**
 * @brief 延迟等待器
 * @note 用于在协程中实现延时操作
 */
class DelayAwaitable
{
public:
    explicit DelayAwaitable(std::chrono::milliseconds delay)
        : m_delay(delay)
    {
    }
    
    bool await_ready() const noexcept
    {
        return m_delay.count() <= 0;
    }
    
    void await_suspend(std::coroutine_handle<> handle) noexcept
    {
        m_handle = handle;
        // 这里需要与 libev 的定时器集成
        // 在实际实现中，需要注册一个 ev_timer
    }
    
    void await_resume() noexcept
    {
    }
    
    void Resume()
    {
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
    }
    
private:
    std::chrono::milliseconds m_delay;
    std::coroutine_handle<> m_handle;
};

/**
 * @brief 立即完成的 Awaitable
 * @note 用于返回立即可用的值
 */
template<typename T>
class ImmediateAwaitable
{
public:
    explicit ImmediateAwaitable(T value)
        : m_value(std::move(value))
    {
    }
    
    bool await_ready() const noexcept
    {
        return true;
    }
    
    void await_suspend(std::coroutine_handle<>) noexcept
    {
        // 不会被调用，因为 await_ready 返回 true
    }
    
    T await_resume()
    {
        return std::move(m_value);
    }
    
private:
    T m_value;
};

/**
 * @brief 异步结果类型
 * @note 封装异步操作的成功/失败结果
 */
template<typename T>
class AsyncResult
{
public:
    AsyncResult() : m_success(false) {}
    
    explicit AsyncResult(T value)
        : m_value(std::move(value))
        , m_success(true)
    {
    }
    
    static AsyncResult Error(int errNo, const std::string& errMsg)
    {
        AsyncResult result;
        result.m_errNo = errNo;
        result.m_errMsg = errMsg;
        result.m_success = false;
        return result;
    }
    
    bool IsSuccess() const { return m_success; }
    int GetErrNo() const { return m_errNo; }
    const std::string& GetErrMsg() const { return m_errMsg; }
    
    T& Value() { return m_value; }
    const T& Value() const { return m_value; }
    
    T ValueOr(T defaultValue) const
    {
        return m_success ? m_value : defaultValue;
    }
    
private:
    T m_value;
    int m_errNo = 0;
    std::string m_errMsg;
    bool m_success;
};

/**
 * @brief void 特化版本
 */
template<>
class AsyncResult<void>
{
public:
    AsyncResult() : m_success(true) {}
    
    static AsyncResult Error(int errNo, const std::string& errMsg)
    {
        AsyncResult result;
        result.m_errNo = errNo;
        result.m_errMsg = errMsg;
        result.m_success = false;
        return result;
    }
    
    bool IsSuccess() const { return m_success; }
    int GetErrNo() const { return m_errNo; }
    const std::string& GetErrMsg() const { return m_errMsg; }
    
private:
    int m_errNo = 0;
    std::string m_errMsg;
    bool m_success;
};

/**
 * @brief 协程上下文
 * @note 保存协程执行的上下文信息
 */
struct CoroutineContext
{
    std::coroutine_handle<> handle;
    StepCo20* state = nullptr;
    ev_timer* timer = nullptr;
    void* userData = nullptr;
};

} /* namespace net */

#endif /* SRC_STEP_AWAITABLE_HPP_ */
