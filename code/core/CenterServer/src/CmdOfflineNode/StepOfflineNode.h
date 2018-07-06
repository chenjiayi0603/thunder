/*******************************************************************************
 * Project:  CenterServer
 * @file     StepOfflineNode.h
 * @brief 
 * @author   chenjiayi
 * @date:    2016年9月14日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEPOFFLINENODE_H_
#define SRC_STEPOFFLINENODE_H_
#include "StarshipError.h"
#include "StarshipErrorMapping.h"
#include "protocol/oss_sys.pb.h"
#include "server.pb.h"
#include "user_basic.pb.h"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"

namespace starshiplib
{

class StepOfflineNode: public oss::Step
{
public:
    StepOfflineNode(const oss::tagMsgShell &stMsgShell,const MsgHead& oInMsgHead,
                    const std::string& sOfflineNodeIdentify,const server::offline_node_ack &oOfflineNodeAck);
    virtual ~StepOfflineNode();
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
    std::string m_sOfflineNodeIdentify;
    server::offline_node_ack m_oOfflineNodeAck;
    int m_iTimeOut;
};

} /* namespace starshiplib */

#endif /* SRC_STEPOFFLINENODE_H_ */
