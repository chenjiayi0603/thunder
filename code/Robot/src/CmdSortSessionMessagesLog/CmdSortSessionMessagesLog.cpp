/*******************************************************************************
 * Project:  LogicServer
 * @file     CmdSortSessionMessagesLog.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdSortSessionMessagesLog.hpp"
#include "util/CommonUtils.hpp"

#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create()
{
    oss::Cmd* pCmd = new robot::CmdSortSessionMessagesLog();
    return(pCmd);
}
#ifdef __cplusplus
}
#endif

namespace robot
{

CmdSortSessionMessagesLog::CmdSortSessionMessagesLog()
{
    m_pSessionAiEngine = NULL;
}

CmdSortSessionMessagesLog::~CmdSortSessionMessagesLog()
{
}

bool CmdSortSessionMessagesLog::Init()
{
	return true;
}

bool CmdSortSessionMessagesLog::AnyMessage(
                const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    LOG4_TRACE("%s()", __FUNCTION__);
    behaviour_common::sort_tb_session_messages_log_list_req oSortTbsessionMessagesLogListReq;
    behaviour_common::sort_tb_session_messages_log_list_ack oSortTbsessionMessagesLogListAck;
    if(!parseMsg(oInMsgBody,oSortTbsessionMessagesLogListReq))
    {
        LOG4_ERROR("parseHttpMsg failed:%d:%s", robot_err_code(ERR_MSG_BODY_DECODE), robot_err_msg(ERR_MSG_BODY_DECODE));
        return Response(ERR_SERVER_ERROR,oSortTbsessionMessagesLogListAck);
    }
    {//引擎排序消息日志
        //CustomClock CmdSortSessionMessagesLog use time(42.105999) ms,1000个消息
        CustomClock customClock("CmdSortSessionMessagesLog",GetLogger());
        m_pSessionAiEngine = GetSessionAiEngine(GetLabor());
        if (m_pSessionAiEngine)
        {
            std::vector<session_messages_log> messageLogs;
            int list_size = oSortTbsessionMessagesLogListReq.list_size();
            for(int i = 0;i < list_size;++i)
            {
                messageLogs.push_back(session_messages_log(oSortTbsessionMessagesLogListReq.list(i)));
            }
            if (messageLogs.size() > 0)
            {
                if (!m_pSessionAiEngine->SortSessionMessagesLog(messageLogs,oSortTbsessionMessagesLogListReq.total()))
                {
                    LOG4_ERROR("%s() failed to SortSessionMessagesLog!",__FUNCTION__);
                    return Response(ERR_SERVER_ERROR,oSortTbsessionMessagesLogListAck);
                }
                if (messageLogs.size() > 0)
                {
                    int s = messageLogs.size();
                    for(int i = 0;i < s;++i)
                    {
                        ::behaviour_common::tb_session_messages_log* pLog = oSortTbsessionMessagesLogListAck.add_list();
                        pLog->CopyFrom(messageLogs[i].message);
                    }
                }
            }
            //SortSessionMessagesLog messageLogs.size(942)
            LOG4_DEBUG("%s() SortSessionMessagesLog messageLogs.size(%u)!",__FUNCTION__,messageLogs.size());
        }
        else
        {
            LOG4_ERROR("%s() failed to GetSessionAiEngine!",__FUNCTION__);
            return Response(ERR_SERVER_ERROR,oSortTbsessionMessagesLogListAck);
        }
    }
    return Response(ERR_OK,oSortTbsessionMessagesLogListAck);
}

bool CmdSortSessionMessagesLog::parseMsg(const MsgBody& oInMsgBody,
                behaviour_common::sort_tb_session_messages_log_list_req &oSortTbsessionMessagesLogListReq)
{
    if (!ParseFromMsg(oInMsgBody,oSortTbsessionMessagesLogListReq))
    {
        LOG4_ERROR("%s() ParseFromMsg(oInMsgBody,oSortTbsessionMessagesLogListReq) failed!",__FUNCTION__);
        return(false);
    }
    LOG4_DEBUG("%s() m_oSortTbsessionMessagesLogListReq(%s)",oSortTbsessionMessagesLogListReq.DebugString().c_str());
    return(true);
}

bool CmdSortSessionMessagesLog::Response(int iErrno,
                behaviour_common::sort_tb_session_messages_log_list_ack &oSortTbsessionMessagesLogListAck)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    common::errorinfo* pError = new common::errorinfo();
    pError->set_error_code(robot_err_code(iErrno));
    pError->set_error_info(robot_err_msg(iErrno));
    pError->set_error_client_show(robot_err_msg(iErrno));
    oSortTbsessionMessagesLogListAck.set_allocated_error(pError);
    oOutMsgBody.set_body(oSortTbsessionMessagesLogListAck.SerializeAsString());
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(m_stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
        return false;
    }
    return true;
}



} /* namespace robot */
