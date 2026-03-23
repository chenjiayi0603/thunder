#ifndef SRC_StepCo20Func_HPP_
#define SRC_StepCo20Func_HPP_

#include "StepCo20.hpp"
#include <functional>

namespace net
{

/**
 * @brief 通用协程步骤：构造时传入返回 AsyncTask 的协程 lambda；Emit emplace 的即是该条 AsyncTask（无壳内 co_await）。
 *        业务体内自行 co_await HttpGetAsync / SendToInternalAsync 等；`m_fn(StepCo20&)` 协程用 `promise_type(StepCo20&)`，return_void 自动 Notify。
 *
 * 用法示例（协程宜为具名函数，lambda 仅转发）：
 *   net::LaunchCo(shell, httpMsg, [](net::StepCo20& s) -> net::AsyncTask { return MyCo(s); });
 */
class StepCo20Func : public StepCo20
{
public:
    using CoroFn = std::function<AsyncTask(StepCo20&)>;

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

    AsyncTask StepAsync() override { return m_fn(*this); }

    void OnCoroutineComplete(bool /*bSuccess*/) override {}

private:
    CoroFn m_fn;
};

/**
 * @brief `Launch(new StepCo20Func(stMsgShell, oHttpMsg, std::move(fn)))` 的便捷封装（沿用 `Launch` 默认超时参数），实现在 `Interface.cpp`。
 */
bool LaunchCo(const tagMsgShell& stMsgShell,
              const HttpMsg& oHttpMsg,
              StepCo20Func::CoroFn fn);

bool LaunchCo(const tagMsgShell& stMsgShell,
              const MsgHead& oMsgHead,
              StepCo20Func::CoroFn fn);

/** 自定义 StepCo20 子类走 `Launch(step)`，与 CoroFn 版相同默认超时。 */
bool LaunchCo(StepCo20* stepCo20);

} // namespace net

#endif // SRC_StepCo20Func_HPP_
