/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdReloadSo.hpp"
#include "labor/Worker.hpp"

namespace net
{

bool CmdReloadSo::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	util::CJsonObject oSoConfJson;
	if(!oSoConfJson.Parse(oInMsgBody.body()))
	{
		LOG4_WARN("failed to parse oSoConfJson:(%s)",oInMsgBody.body().c_str());
	}
	else
	{
		LOG4_INFO("CMD_REQ_RELOAD_SO:update so conf to oSoConfJson(%s) %s",
				oSoConfJson.ToString().c_str(),"force operation");
		((Worker*)GetLabor())->LoadSo(oSoConfJson,true);
	}
    return(true);
}

} /* namespace net */
