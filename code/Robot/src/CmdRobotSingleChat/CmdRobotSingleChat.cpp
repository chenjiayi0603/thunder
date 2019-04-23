/*******************************************************************************
 * Project:  RobotServer
 * @file     CmdRobotSingleChat.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年12月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "util/CommonUtils.hpp"
#include "util/UnixTime.hpp"
#include "CmdRobotSingleChat.hpp"


#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create()
{
    oss::Cmd* pCmd = new robot::CmdRobotSingleChat();
    return(pCmd);
}
#ifdef __cplusplus
}
#endif

namespace robot
{

CmdRobotSingleChat::CmdRobotSingleChat()
    : m_boInit(false),m_pRobotSession(NULL),pStepQueryRobotAnswer(NULL)
{
}

CmdRobotSingleChat::~CmdRobotSingleChat()
{
}

bool CmdRobotSingleChat::Init()
{
    if (m_boInit)
    {
        return true;
    }
    m_pRobotSession = GetRobotSession(GetLabor(),GetConfigPath());
    if(!m_pRobotSession)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    m_pRobotSession->ResetAiEngineQuestions();
//    m_pRobotSession->TestSphinx();
    m_boInit = true;
    return true;
}

bool CmdRobotSingleChat::AnyMessage(
                const oss::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    m_stMsgShell = stMsgShell;
    m_oInMsgHead = oInMsgHead;
    LOG4_TRACE("%s()", __FUNCTION__);
    if(oInMsgBody.additional().size() == 0)
    {
        LOG4_ERROR("%s() additional not exist",__FUNCTION__);
        Response(ERR_INVALID_PROTOCOL);
        return(false);
    }
    user_basic basicInfo;
    if(!basicInfo.ParseFromString(oInMsgBody.additional()))
    {
        LOG4_ERROR("%s() Parse additional failed",__FUNCTION__);
        Response(ERR_INVALID_PROTOCOL);
        return(false);
    }
    if(!parseMsg(oInMsgBody))
    {
        LOG4_ERROR("parseHttpMsg failed:%d:%s", robot_err_code(ERR_MSG_BODY_DECODE), robot_err_msg(ERR_MSG_BODY_DECODE));
        Response(ERR_REQ_MISS_PARAM);
        return(false);
    }
    m_pRobotSession = GetRobotSession(GetLabor(),GetConfigPath());
    if(!m_pRobotSession)
    {
        LOG4_ERROR("%d:%s failed to GetRobotSession", robot_err_code(ERR_SERVER_ERROR), robot_err_msg(ERR_SERVER_ERROR));
        Response(ERR_SERVER_ERROR);
        return(false);
    }
    std::string strFilteredQuestion;
    strFilteredQuestion.assign(m_oRobotSingleMsgReq.msg().c_str(),m_oRobotSingleMsgReq.msg().size());
    m_pRobotSession->SkipNonsenseLetters(strFilteredQuestion);
    if(strFilteredQuestion.size() > 0)
    {
        pStepQueryRobotAnswer = new StepQueryRobotAnswer(stMsgShell, oInMsgHead, m_oRobotSingleMsgReq,basicInfo,m_pRobotSession,strFilteredQuestion);
        if (pStepQueryRobotAnswer == NULL)
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(), "error %d: new StepFromClient() error!", oss::ERR_NEW);
            Response(ERR_SERVER_ERROR);
            return oss::STATUS_CMD_FAULT;
        }
        if (!RegisterCallback(pStepQueryRobotAnswer))
        {
            delete pStepQueryRobotAnswer;
            pStepQueryRobotAnswer = NULL;
            LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to RegisterCallback(pStepQueryRobotAnswer)");
            Response(ERR_SERVER_ERROR);
            return oss::STATUS_CMD_FAULT;
        }
        oss::E_CMD_STATUS ret = pStepQueryRobotAnswer->Emit(ERR_OK);
        if (oss::STATUS_CMD_FAULT == ret)
        {
            LOG4CPLUS_ERROR_FMT(GetLogger(), "failed to pStepQueryRobotAnswer Emit");
            DeleteCallback(pStepQueryRobotAnswer);
            return false;
        }
        else if (oss::STATUS_CMD_COMPLETED == ret)
        {
            DeleteCallback(pStepQueryRobotAnswer);
            return true;
        }
        return(true);
    }
    LOG4_WARN("no answer for no words");
    robot_session::robot_single_msg_ack oAck;
//    oAck.set_msg_id(loss::GetUniqueId(GetNodeId(),GetWorkerIndex()));//在Logic生成msg_id
    oAck.set_send_time(loss::GetCurrentTime());
    oAck.set_msg_type(eRobotMsgType_text);
    oAck.set_msg(m_pRobotSession->GetDefaultAnswer());
    Response(ERR_OK,oAck);
    return(true);
}

bool CmdRobotSingleChat::parseMsg(const MsgBody& oInMsgBody)
{
    /*
    message robot_single_msg_req
    {
        required uint32 send_id = 1;//发送用户id
        required uint32 send_type = 2;//发送用户类型
        required uint32 msg_type = 3;//消息类型
        required common.msg_content msg  = 4; // 消息内容
    }
     * */
    m_oRobotSingleMsgReq.Clear();//oRobotSingleMsg内存变化不大，使用Clear()
    if (!m_oRobotSingleMsgReq.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("%s() %d: user::user_register_req failed to ParseFromString(oInMsgBody.body())!",
                        __FUNCTION__,ERR_INVALID_PROTOCOL);
        return(false);
    }
    return(true);
}

void CmdRobotSingleChat::Response(int iErrno)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    robot_session::robot_single_msg_ack oRsp;
    common::errorinfo* pError = new common::errorinfo();
    pError->set_error_code(robot_err_code(iErrno));
    pError->set_error_info(robot_err_msg(iErrno));
    pError->set_error_client_show(robot_err_msg(iErrno));
    oRsp.set_allocated_error(pError);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(m_stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
}

void CmdRobotSingleChat::Response(int iErrno,robot_session::robot_single_msg_ack &oRsp)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    common::errorinfo* pError = new common::errorinfo();
    pError->set_error_code(robot_err_code(iErrno));
    pError->set_error_info(robot_err_msg(iErrno));
    pError->set_error_client_show(robot_err_msg(iErrno));
    oRsp.set_allocated_error(pError);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(m_oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(m_oInMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(m_stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("%s()failed to send error info to fd %d fd_seq %u",__FUNCTION__,
                        m_stMsgShell.iFd, m_stMsgShell.ulSeq);
    }
}


} /* namespace robot */
