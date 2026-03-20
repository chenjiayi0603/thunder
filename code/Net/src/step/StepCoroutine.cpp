#include "step/StepCoroutine.hpp"
#include "step/HttpAwaitable.hpp"
#include "protocol/http.pb.h"
#include <exception>

namespace thunder {

StepCoroutine::StepCoroutine() : m_coTask(), m_coHandle(nullptr), m_bFirstEmit(true) {}

StepCoroutine::~StepCoroutine() {
    if (!m_coTask.done()) {
        m_coTask.destroy();
    }
}

E_CMD_STATUS StepCoroutine::Emit() {
    if (m_bFirstEmit) {
        m_bFirstEmit = false;
        // 首次调用：启动协程
        m_coTask = Run();
        m_coTask.resume();
    } else {
        // 后续调用：恢复协程
        if (!m_coTask.done()) {
            m_coTask.resume();
        }
    }

    // 检查协程是否完成
    if (m_coTask.done()) {
        // 检查异常
        if (m_coTask.has_exception()) {
            try {
                std::rethrow_exception(m_coTask.exception());
            } catch (const std::exception& e) {
                // 异常处理：调用 OnFail
                return OnFail(STEP_ERR_HTTP, e.what());
            }
        }
        // 协程正常完成
        return OnSucc(0);
    }

    return STATUS_CMD_RUNNING;
}

E_CMD_STATUS StepCoroutine::Callback(int iStep, const HttpMsg& oHttpMsg) {
    // 存储响应消息
    m_oLastHttpMsg = oHttpMsg;

    // 恢复协程
    return Emit();
}

E_CMD_STATUS StepCoroutine::Timeout() {
    // 超时处理
    if (!m_coTask.done()) {
        m_coTask.destroy();
    }
    return OnFail(STEP_ERR_TIMEOUT, "Step Timeout");
}

HttpAwaitable StepCoroutine::HttpGetAsync(const std::string& url) {
    return HttpAwaitable(this, url, "GET");
}

HttpAwaitable StepCoroutine::HttpPostAsync(const std::string& url, const std::string& body) {
    return HttpAwaitable(this, url, "POST", body);
}

}  // namespace thunder
