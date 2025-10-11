/*******************************************************************************
 * Project:  Net
 * @file     CmdBeat.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdLogicConfig.hpp"
#include "labor/Worker.hpp"

namespace net
{

bool CmdLogicConfig::AnyMessage(
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
		util::CJsonObject oCmds;
		if(oConfJson.Get("cmd",oCmds))
		{
			LOG4_INFO("reload so conf to oCmds(%s)", oCmds.ToString().c_str());
			((Worker*)GetLabor())->ReloadSo(oCmds);
		}
		util::CJsonObject oUrlPaths;
		if(oConfJson.Get("url_path",oUrlPaths))
		{
			LOG4_INFO("reload module conf to oUrlPaths(%s)", oUrlPaths.ToString().c_str());
			((Worker*)GetLabor())->ReloadModule(oUrlPaths);
		}
	}
    return(true);
}

} /* namespace net */
