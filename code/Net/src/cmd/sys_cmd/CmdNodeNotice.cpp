/*******************************************************************************
 * Project:  Net
 * @file     CmdNodeNotice.cpp
 * @brief 
 * @author   Tommy
 * @date:    2019年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <iostream>
#include <cmd/sys_cmd/CmdNodeNotice.hpp>
#include "util/json/CJsonObject.hpp"

namespace net
{

bool CmdNodeNotice::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	if (m_oNodeNotice.ParseFromString(oInMsgBody.body()))
	{
		LOG4_TRACE("CmdNodeNotice seq[%u] oNodeNotice[%s]",oInMsgHead.seq(),m_oNodeNotice.DebugString().c_str());
		char   strIdentify[50] = {0};
		for (int i = 0;i< m_oNodeNotice.node_arry_reg_size();i++)
		{
			const auto& oNodeReg = m_oNodeNotice.node_arry_reg(i);
			{
				for(int j = 0;j<oNodeReg.worker_num();j++)
				{
					sprintf(strIdentify,"%s:%d.%d",oNodeReg.node_ip().c_str(),oNodeReg.node_port(),j);
					GetLabor()->AddNodeIdentify(oNodeReg.node_type(),std::string(strIdentify));
					LOG4_TRACE("AddNodeIdentify(%s,%s)",oNodeReg.node_type().c_str(),strIdentify);
				}
			}
		}

		for (int i = 0;i< m_oNodeNotice.node_arry_exit_size();i++)
		{
			const auto& oNodeExit = m_oNodeNotice.node_arry_exit(i);
			{
				for(int j = 0;j<oNodeExit.worker_num();j++)
				{
					sprintf(strIdentify,"%s:%d.%d",oNodeExit.node_ip().c_str(),oNodeExit.node_port(),j);
					GetLabor()->DelNodeIdentify(oNodeExit.node_type(),std::string(strIdentify));
					LOG4_TRACE("DelNodeIdentify(%s,%s)",oNodeExit.node_type().c_str(),strIdentify);
				}
			}
		}
		GetLabor()->SendToClient(stMsgShell,oInMsgHead,"ok");
	}
	else
	{
		LOG4_WARN("CmdNodeNotice seq[%u] parse failed",oInMsgHead.seq());
		GetLabor()->SendToClient(stMsgShell,oInMsgHead,"failed");
	}
    return(true);
}

} /* namespace net */
