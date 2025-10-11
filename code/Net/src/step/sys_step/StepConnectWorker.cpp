/*******************************************************************************
 * Project:  Net
 * @file     StepConnectWorker.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年8月14日
 * @note
 * Modify history:
 ******************************************************************************/
#include "StepConnectWorker.hpp"

namespace net
{

StepConnectWorker::StepConnectWorker(const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
    : m_iTimeoutNum(0),
      m_stMsgShell(stMsgShell), m_oConnectMsgHead(oInMsgHead), m_oConnectMsgBody(oInMsgBody),
      pStepTellWorker(nullptr)
{
}

E_CMD_STATUS StepConnectWorker::Emit(int iErrno,const std::string& strErrMsg,const std::string& strErrShow)
{
    m_oConnectMsgHead.set_seq(GetSequence());
    GetLabor()->SendTo(m_stMsgShell, m_oConnectMsgHead, m_oConnectMsgBody);
    return(STATUS_CMD_RUNNING);
}

E_CMD_STATUS StepConnectWorker::Callback(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data)
{
    OrdinaryResponse oRes;
    if (oRes.ParseFromString(oInMsgBody.body()))
    {
        if (oRes.err_no() == ERR_OK)
        {
            for (int i = 0; i < 3; ++i)
            {
                if (GetLabor()->ExecStep(new StepTellWorker(m_stMsgShell)))
                {
                	return(STATUS_CMD_COMPLETED);
                }
            }
            return(STATUS_CMD_FAULT);
        }
        else
        {
            LOG4_ERROR("error %d: %s!", oRes.err_no(), oRes.err_msg().c_str());
            return(STATUS_CMD_FAULT);
        }
    }
    else
    {
        LOG4_ERROR("error %d: WorkerLoad ParseFromString error!", ERR_PARASE_PROTOBUF);
        return(STATUS_CMD_FAULT);
    }
}

E_CMD_STATUS StepConnectWorker::Timeout()
{
    ++m_iTimeoutNum;
    if (m_iTimeoutNum <= 3)
    {
        return(Emit(ERR_OK));
    }
    else
    {
        LOG4_ERROR("timeout %d times!", m_iTimeoutNum);
        return(STATUS_CMD_FAULT);
    }
}

} /* namespace net */
