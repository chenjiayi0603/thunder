/*******************************************************************************
 * Project:  Net
 * @file     CoTask.hpp
 * @brief    C++20 协程任务类型
 * @author   cjy
 * @date:    2024
 * @note     用于替换 StepState 状态机模式，使异步代码写起来像同步代码
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_COTASK_HPP_
#define SRC_STEP_COTASK_HPP_

#include <coroutine>
#include <exception>
#include <utility>

namespace net
{

/**
 * @brief 协程任务类型
 * @note 作为协程函数的返回类型，管理协程的生命周期
 */
class CoTask
{
public:
    /**
     * @brief Promise 类型，定义协程的行为
     */
    struct promise_type
    {
        std::exception_ptr m_exception;  // 异常指针
        bool m_completed = false;        // 是否完成

        /**
         * @brief 获取返回对象
         * @return CoTask 实例
         */
        CoTask get_return_object()
        {
            return CoTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * @brief 初始挂起点
         * @note 返回 suspend_always，等 Emit() 调用才启动协程
         */
        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        /**
         * @brief 最终挂起点
         * @note 返回 suspend_always，防止协程帧提前销毁
         */
        std::suspend_always final_suspend() noexcept
        {
            m_completed = true;
            return {};
        }

        /**
         * @brief 无返回值
         */
        void return_void()
        {
            m_completed = true;
        }

        /**
         * @brief 处理未捕获的异常
         */
        void unhandled_exception()
        {
            m_exception = std::current_exception();
        }

        /**
         * @brief 检查是否有异常
         */
        bool hasException() const
        {
            return m_exception != nullptr;
        }

        /**
         * @brief 重新抛出异常
         */
        void rethrowIfException()
        {
            if (m_exception)
            {
                std::rethrow_exception(m_exception);
            }
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    /**
     * @brief 构造函数
     * @param h 协程句柄
     */
    explicit CoTask(handle_type h) : m_handle(h) {}

    /**
     * @brief 默认构造函数
     */
    CoTask() : m_handle(nullptr) {}

    /**
     * @brief 移动构造函数
     */
    CoTask(CoTask&& other) noexcept : m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    /**
     * @brief 移动赋值运算符
     */
    CoTask& operator=(CoTask&& other) noexcept
    {
        if (this != &other)
        {
            if (m_handle)
            {
                m_handle.destroy();
            }
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    /**
     * @brief 禁止拷贝
     */
    CoTask(const CoTask&) = delete;
    CoTask& operator=(const CoTask&) = delete;

    /**
     * @brief 析构函数
     */
    ~CoTask()
    {
        if (m_handle)
        {
            m_handle.destroy();
        }
    }

    /**
     * @brief 恢复协程执行
     */
    void resume()
    {
        if (m_handle && !m_handle.done())
        {
            m_handle.resume();
        }
    }

    /**
     * @brief 检查协程是否完成
     */
    bool done() const
    {
        return !m_handle || m_handle.done();
    }

    /**
     * @brief 销毁协程
     */
    void destroy()
    {
        if (m_handle)
        {
            m_handle.destroy();
            m_handle = nullptr;
        }
    }

    /**
     * @brief 检查协程是否有效
     */
    bool valid() const
    {
        return m_handle != nullptr;
    }

    /**
     * @brief 获取 Promise
     */
    promise_type& promise()
    {
        return m_handle.promise();
    }

    /**
     * @brief 检查是否有异常
     */
    bool hasException() const
    {
        return m_handle && m_handle.promise().hasException();
    }

    /**
     * @brief 重新抛出异常
     */
    void rethrowIfException()
    {
        if (m_handle)
        {
            m_handle.promise().rethrowIfException();
        }
    }

    /**
     * @brief 获取协程句柄
     */
    handle_type handle() const
    {
        return m_handle;
    }

private:
    handle_type m_handle;
};

} /* namespace net */

#endif /* SRC_STEP_COTASK_HPP_ */
