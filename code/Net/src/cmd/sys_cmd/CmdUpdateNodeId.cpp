/*******************************************************************************
 * Project:  Net
 * @file     CmdUpdateNodeId.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年9月18日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdUpdateNodeId.hpp"

namespace net
{

bool CmdUpdateNodeId::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
	NodeReportRsp oNodeReportRsp;
	if (oNodeReportRsp.ParseFromString(oInMsgBody.body()))
	{
		if (oNodeReportRsp.errcode() == 0)
		{
			if (GetLabor()->GetNodeId() != oNodeReportRsp.node_id())
			{
				GetLabor()->SetNodeId(oNodeReportRsp.node_id());
				LOG4_INFO("SetNodeId node_id(%u)!",oNodeReportRsp.node_id());
			}
		}
		else
		{
			LOG4_WARN("oNodeReportRsp errcode %u!", oNodeReportRsp.errcode());
		}
	}
	else
	{
		LOG4_WARN("oNodeReportRsp parse error!");
	}
    return(false);
}

} /* namespace net */
