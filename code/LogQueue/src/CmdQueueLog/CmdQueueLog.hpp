/*******************************************************************************
 * Project:  LogQueue
 * @file     CmdQueueLog.cpp
 * @brief
 * @author   cjy
 * @date:    2017年5月26日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef PLUGINS_CMDQUEUELOG_HPP_
#define PLUGINS_CMDQUEUELOG_HPP_
#include "AnalysisError.h"
#include "AnalysisErrorMapping.h"
#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "../LogQueueSession.h"

namespace analysis
{

class CmdQueueLog: public net::Cmd
{
public:
    CmdQueueLog();
    virtual ~CmdQueueLog();
    virtual bool AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
private:
    bool Init();
    bool Response(int iErrno,const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
    bool m_boInit;
    LogQueueSession* m_pLogQueueSession;
};

} /* namespace analysis */

#endif /* PLUGINS_CMDBEHAVIOURLOG_HPP_ */
