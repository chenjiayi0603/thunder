#ifndef SRC_HttpStateFuncCo_HPP_
#define SRC_HttpStateFuncCo_HPP_

#include <map>
#include "step/CoroutineState.hpp"
#include "step/Step.hpp"

namespace core
{

/**
 * @brief 原 TestHttpRequestStateFunc（lambda + StepState）的协程版
 * @note 对应 state0→state1(SetNextState 3 跳过 state2)→state3 与 OnSucc/OnFail
 */
struct HttpStateFuncParam : public net::StepParam
{
    HttpStateFuncParam() : val(3)
    {
        for (uint32 i = 1; i <= val; ++i)
            m.insert(std::make_pair(static_cast<int>(i), static_cast<int>(i)));
    }
    uint32 val = 3;
    std::map<int, int> m;
    uint32 Inc()
    {
        ++val;
        m.insert(std::make_pair(static_cast<int>(val), static_cast<int>(val)));
        return val;
    }
};

class HttpStateFuncCo : public net::CoroutineState
{
public:
    HttpStateFuncCo(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    ~HttpStateFuncCo() override;

    net::CoTask<void> Run() override;

private:
    void SendSucc();
    void SendFail();
};

} // namespace core

#endif
