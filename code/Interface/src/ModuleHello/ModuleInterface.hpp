/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.hpp
 * @brief    Interface 节点 ModuleHello：gentoken + 与 Hello 对齐的协程 HTTP 测试入口
 ******************************************************************************/
#ifndef SRC_ModuleInterface_ModuleInterface_HPP_
#define SRC_ModuleInterface_ModuleInterface_HPP_

#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "RobotError.h"
#include "../InterfaceSession.h"
#include "util/CommonUtils.hpp"

namespace robot
{

class ModuleHello : public net::Module
{
public:
    ModuleHello();
    virtual ~ModuleHello();
    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);

private:
    /// body JSON 中 option：Echo、TestStepHttpRequestCo、GenKey/VerifyKey（均为 net::StepCo20Func + lambda）等
    bool DispatchJsonTestsFromBody(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void Response(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, int iCode);
};

} /* namespace robot */

#endif /* SRC_ModuleInterface_ModuleInterface_HPP_ */
