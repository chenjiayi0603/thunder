#ifndef SRC_StepCo20Func_HPP_
#define SRC_StepCo20Func_HPP_

#include "StepCo20.hpp"
#include <functional>

namespace net
{

/**
 * @brief 通用协程步骤：构造时传入函数对象，CoroutineMain 直接调用它
 *
 * 用法示例：
 *   net::Launch(new net::StepCo20Func(shell, httpMsg,
 *       [](net::StepCo20& step) -> net::Task<> {
 *           auto ok = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
 *           step.ResponseToClient(ok ? 200 : 500, ok ? "{}" : R"({"code":1})");
 *       }));
 */
class StepCo20Func : public StepCo20
{
public:
    using CoroFn = std::function<Task<>(StepCo20&)>;

    StepCo20Func(const tagMsgShell& s, const HttpMsg& m, CoroFn fn)
        : StepCo20(s, m), m_fn(std::move(fn))
    {
        m_strStepDesc = "StepCo20Func";
    }

    StepCo20Func(const tagMsgShell& s, const MsgHead& h, CoroFn fn)
        : StepCo20(s, h), m_fn(std::move(fn))
    {
        m_strStepDesc = "StepCo20Func";
    }

    StepCo20Func(const tagMsgShell& s, const MsgHead& h, const MsgBody& b, CoroFn fn)
        : StepCo20(s, h, b), m_fn(std::move(fn))
    {
        m_strStepDesc = "StepCo20Func";
    }

    Task<> CoroutineMain() override { co_await m_fn(*this); }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

private:
    CoroFn m_fn;
};

} // namespace net

#endif // SRC_StepCo20Func_HPP_
