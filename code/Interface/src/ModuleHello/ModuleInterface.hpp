/*******************************************************************************
 * Project:  Interface
 * @file     ModuleInterface.hpp
 * @brief    Interface 节点 ModuleHello：gentoken + 与 Hello 对齐的协程 HTTP 测试入口
 ******************************************************************************/
#ifndef SRC_ModuleInterface_ModuleInterface_HPP_
#define SRC_ModuleInterface_ModuleInterface_HPP_

#include <map>

#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "RobotError.h"
#include "../InterfaceSession.h"
#include "util/CommonUtils.hpp"

namespace robot
{

#define GET_TOKEN_GEN (10001)

class ModuleHello : public net::Module
{
public:
    ModuleHello();
    virtual ~ModuleHello();
    virtual bool Init();
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    bool AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody) override;
    void GenKey(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void VerifyKey(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);

private:
    /// 与 Hello ModuleHello::TestMsg 一致：body JSON 中 option 触发 Echo / 协程演示
    bool DispatchJsonTestsFromBody(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    void Response(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg, int iCode);
};

} /* namespace robot */

#endif /* SRC_ModuleInterface_ModuleInterface_HPP_ */
