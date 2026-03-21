/*******************************************************************************
 * Interface 节点独立副本（见 StepBinaryCo20Binary.cpp）
 ******************************************************************************/
#ifndef INTERFACE_MODULEHELLO_StepBinaryCo20Binary_HPP_
#define INTERFACE_MODULEHELLO_StepBinaryCo20Binary_HPP_

#include "step/StepCo20.hpp"

namespace core
{

/**
 * @brief 协程发往 LOGIC（cmd 10001）：body JSON 的 option 为 GenKey / VerifyKey / TestStepCo20Binary。
 *        GenKey、VerifyKey 原 SendToCallback 逻辑改为组包后 co_await SendToInternalByNodeTypeAsync；
 *        TestStepCo20Binary 为消息体原样透传。HTTP 入口须用 HttpMsg 构造以便 ResponseToClient。
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
