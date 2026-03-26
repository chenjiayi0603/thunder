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
#include "protocol/oss_sys.pb.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>

namespace net
{
namespace
{
void ApplySubscribedRouteSnapshot(Labor *labor, const NodeNotice &snap)
{
	if (snap.node_arry_reg_size() <= 0)
	{
		return;
	}
	std::unordered_map<std::string, std::unordered_set<std::string>> expected;
	char strIdentify[64] = {0};
	for (int i = 0; i < snap.node_arry_reg_size(); ++i)
	{
		const NodeReport &oNodeReg = snap.node_arry_reg(i);
		for (int j = 0; j < oNodeReg.worker_num(); ++j)
		{
			snprintf(strIdentify, sizeof(strIdentify), "%s:%u.%d", oNodeReg.node_ip().c_str(), static_cast<unsigned>(oNodeReg.node_port()), j);
			expected[oNodeReg.node_type()].insert(strIdentify);
		}
	}
	for (const auto &pr : expected)
	{
		std::vector<std::string> locals;
		labor->GetNodeIdentifys(pr.first, locals);
		for (const auto &id : locals)
		{
			if (pr.second.find(id) == pr.second.end())
			{
				labor->DelNodeIdentify(pr.first, id);
				LOG4_TRACE("%s() full snapshot prune DelNodeIdentify(%s,%s)", __FUNCTION__, pr.first.c_str(), id.c_str());
			}
		}
	}
	for (int i = 0; i < snap.node_arry_reg_size(); ++i)
	{
		const NodeReport &oNodeReg = snap.node_arry_reg(i);
		for (int j = 0; j < oNodeReg.worker_num(); ++j)
		{
			snprintf(strIdentify, sizeof(strIdentify), "%s:%u.%d", oNodeReg.node_ip().c_str(), static_cast<unsigned>(oNodeReg.node_port()), j);
			labor->AddNodeIdentify(oNodeReg.node_type(), std::string(strIdentify));
			LOG4_TRACE("%s() full snapshot AddNodeIdentify(%s,%s)", __FUNCTION__, oNodeReg.node_type().c_str(), strIdentify);
		}
	}
}
} // namespace

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
			if (oNodeReportRsp.has_subscribed_route_snapshot())
			{
				ApplySubscribedRouteSnapshot(GetLabor(), oNodeReportRsp.subscribed_route_snapshot());
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
