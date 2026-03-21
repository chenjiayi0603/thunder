/*******************************************************************************
 * Interface 节点独立副本（见 StepBinaryCo20.cpp）
 ******************************************************************************/
#ifndef INTERFACE_MODULEHELLO_StepBinaryCo20Binary_HPP_
#define INTERFACE_MODULEHELLO_StepBinaryCo20Binary_HPP_

#include "step/StepCo20.hpp"

namespace core
{

/**
 * @brief TestStepCo20Binary：co_await SendToInternalByNodeTypeAsync("LOGIC",...)；HTTP 入口须用 HttpMsg 构造以正确回 HTTP。
 *        发往 LOGIC 的 MsgBody 为客户端消息体原样透传（不再包 option/via/forward），便于 Cmd 侧按 JSON 字段解析。
 */
class StepBinaryCo20Binary : public net::StepCo20
{
public:
    /// 须用 HttpMsg 构造，保证 ResponseToClient 走 HTTP 分支（m_oReqMsgHead.cmd()==0）
    StepBinaryCo20Binary(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);

    net::Task<> CoroutineMain() override;

protected:
    void OnCoroutineComplete(bool bSuccess) override;
};

} // namespace core

#endif // INTERFACE_MODULEHELLO_StepBinaryCo20Binary_HPP_
