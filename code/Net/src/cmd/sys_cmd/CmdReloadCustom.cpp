/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdReloadCustom.hpp"
#include "labor/Worker.hpp"

namespace net
{

bool CmdReloadCustom::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	util::CJsonObject oConfJson;
	if(!oConfJson.Parse(oInMsgBody.body()))
	{
		LOG4_WARN("failed to parse oConfJson:(%s)",oInMsgBody.body().c_str());
	}
	else
	{
		LOG4_INFO("CMD_REQ_SET_NODE_CUSTOM_CONFIG:update module conf to oModuleConfJson(%s)",oConfJson.ToString().c_str());
		GetLabor()->SetCustomConf(oConfJson);
		LOG4_INFO("CMD_REQ_SET_NODE_CUSTOM_CONFIG:update module conf to GetCustomConf(%s)",GetLabor()->GetCustomConf().ToString().c_str());

	}
    return(true);
}

} /* namespace net */
