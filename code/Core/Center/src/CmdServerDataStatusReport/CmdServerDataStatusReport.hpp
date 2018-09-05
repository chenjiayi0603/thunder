/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdNodeReg.hpp
 * @brief    节点上报
 * @author   cjy
 * @date:    2017年1月13日
 * @note     其它模块向CENTER报告相关信息
 * Modify history:
 ******************************************************************************/
#ifndef CMD_NODE_REPORT_HPP_
#define CMD_NODE_REPORT_HPP_
#include <iostream>
#include "util/json/CJsonObject.hpp"
#include "protocol/oss_sys.pb.h"
#include "cmd/Cmd.hpp"
#include "dbi/MysqlDbi.hpp"
#include "Comm.hpp"
#include "NodeSession.h"

namespace core
{
/**
 * @brief   服务器上报
 * @author  chenjiayi
 * @date    2015年10月9日
 * @note    各个模块启动时需要向CENTER进行上报服务器数据
 */
class CmdServerReport : public net::Cmd
{
public:
    CmdServerReport();
    virtual ~CmdServerReport();
    virtual bool Init();
    virtual bool AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
private:
    bool Response(const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,int iRet);
    NodeSession* pSess;
    bool boInit;
};

} /* namespace core */

#endif /* CMD_NODE_REPORT_HPP_ */
