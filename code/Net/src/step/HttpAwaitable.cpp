#include "step/HttpAwaitable.hpp"
#include "step/StepCoroutine.hpp"

namespace net
{

void HttpAwaitable::await_suspend(std::coroutine_handle<> h)
{
    m_pStep->SaveCoroutineHandle(h);
    m_pStep->DoHttpRequest(*this);
}

const HttpMsg& HttpAwaitable::await_resume()
{
    return m_pStep->GetLastHttpResponse();
}

} // namespace net
