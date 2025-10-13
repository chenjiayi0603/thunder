/*******************************************************************************
 * Project:  Net
 * @file     CmdNodeNotice.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdSetLogLevel.hpp"

namespace net
{

bool CmdSetLogLevel::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	LogLevel oLogLevel;
	if(!oLogLevel.ParseFromString(oInMsgBody.body()))
	{
		LOG4_WARN("failed to parse oLogLevel,body(%s)",oInMsgBody.body().c_str());
	}
	else
	{
		LOG4_INFO("CMD_REQ_SET_LOG_LEVEL:log level set to %d", oLogLevel.log_level());
		GetLabor()->ResetLogLevel(oLogLevel.log_level());
	}
    return(true);
}

} /* namespace net */
