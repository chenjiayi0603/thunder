#include "step/HttpAwaitable.hpp"
#include "step/StepCoroutine.hpp"
#include "HttpMsg.hpp"

namespace thunder {

void HttpAwaitable::await_suspend(std::coroutine_handle<> h) {
    m_pCo->SetCoHandle(h);

    // 发起 HTTP 请求（同 HttpStep 的逻辑）
    if (m_method == "GET") {
        m_pCo->HttpGet(m_url);
    } else if (m_method == "POST") {
        m_pCo->HttpPost(m_url, m_body);
    }

    // 当 HTTP 响应到达时，框架会调用 Callback → Emit → resume()
}

const HttpMsg& HttpAwaitable::await_resume() const {
    return m_pCo->GetLastResponse();
}

}  // namespace thunder
