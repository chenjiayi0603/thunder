/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdNodeReg.hpp
 * @brief    节点上报
 * @author   cjy
 * @date:    2017年1月13日
 * @note     其它模块向CENTER报告相关信息
 * Modify history:
 ******************************************************************************/
#ifndef CMD_NODE_GET_LOAD_MIN_HPP_
#define CMD_NODE_GET_LOAD_MIN_HPP_
#include "protocol/oss_sys.pb.h"
#include "cmd/Cmd.hpp"
#include "dbi/MysqlDbi.hpp"
#include "../Comm.hpp"
#include "../NodeSession.h"

namespace starshiplib
{
/**
 * @brief   获取access
 * @author  chenjiayi
 * @date    2015年12月2日
 * @note    获取低负载的access server
 */
class CmdGetLoadMinServer : public oss::Cmd
{
public:
    CmdGetLoadMinServer();
    virtual ~CmdGetLoadMinServer();
    virtual bool Init();
    virtual bool AnyMessage(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
private:
    bool Response(const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,const NodeLoadStatus& nodeLoadStatus,int iRet);
    NodeSession* pSess;
    bool boInit;
};

} /* namespace starshiplib */

#endif /* CMD_NODE_GET_LOAD_MIN_HPP_ */
