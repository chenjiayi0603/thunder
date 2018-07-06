/*******************************************************************************
 * Project:  CenterServer
 * @file     StepRestartWorkers.h
 * @brief 
 * @author   chenjiayi
 * @date:    2016年9月14日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEPRESTARTWORKERS_H_
#define SRC_STEPRESTARTWORKERS_H_
#include "StarshipError.h"
#include "StarshipErrorMapping.h"
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"

namespace starshiplib
{

class StepRestartWorkers: public oss::Step
{
public:
    StepRestartWorkers(const oss::tagMsgShell &stMsgShell,const MsgHead& oInMsgHead,
                    const std::string& sOnlineNodeIdentify,const server::online_node_ack &oOnlineNodeAck);
    virtual ~StepRestartWorkers();
    virtual oss::E_CMD_STATUS Callback(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);

    virtual oss::E_CMD_STATUS Timeout();
    virtual oss::E_CMD_STATUS Emit(int iErrno, const std::string &strErrMsg = "", const std::string &strErrShow = "");
    oss::E_CMD_STATUS Response(int iErrno);
private:
    oss::tagMsgShell m_stMsgShell;
    MsgHead m_oInMsgHead;
    std::string m_sOnlineNodeIdentify;
    server::online_node_ack m_oOnlineNodeAck;
    int m_iTimeOut;
};

} /* namespace starshiplib */

#endif /* SRC_STEPOFFLINENODE_H_ */
