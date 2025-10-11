/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdReloadModule.hpp"
#include "labor/Worker.hpp"

namespace net
{

bool CmdReloadModule::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	util::CJsonObject oModuleConfJson;
	if(!oModuleConfJson.Parse(oInMsgBody.body()))
	{
		LOG4_WARN("failed to parse oModuleConfJson:(%s)",oInMsgBody.body().c_str());
	}
	else
	{
		LOG4_INFO("CMD_REQ_RELOAD_MODULE:update module conf to oModuleConfJson(%s) %s",
				oModuleConfJson.ToString().c_str(),"force operation");
		((Worker*)GetLabor())->LoadModule(oModuleConfJson,true);
	}
    return(true);
}

} /* namespace net */
