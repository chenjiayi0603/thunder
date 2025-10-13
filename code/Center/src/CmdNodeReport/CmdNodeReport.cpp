/*******************************************************************************
 * Project:  Center
 * @file     CmdNodeReport.cpp
 * @brief 
 * @author   Tommy
 * @date:    Feb 14, 2017
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdNodeReport.hpp"

MUDULE_CREATE(coor::CmdNodeReport);

namespace coor
{

bool CmdNodeReport::Init()
{
	if (nullptr == m_pSessionOnlineNodes)
	{
		m_pSessionOnlineNodes = GetSessionOnlineNodes();
		if (nullptr == m_pSessionOnlineNodes)
		{
			LOG4_ERROR("no session node found!");
			return false;
		}
	}
    return(true);
}

bool CmdNodeReport::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oMsgHead,const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() NodeReport oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
	NodeReport oNodeReport;
	NodeReportRsp oNodeReportRsp;
    if (oNodeReport.ParseFromString(oMsgBody.body()))
    {
        std::string strNodeIdentify = oNodeReport.node_ip() + std::string(":") + std::to_string(oNodeReport.node_port());
        LOG4_TRACE("%s() NodeReport oMsgHead.cmd:%u nodeIdentify:%s", __FUNCTION__, oMsgHead.cmd(),strNodeIdentify.c_str());
        uint16 unNodeId = m_pSessionOnlineNodes->AddNode(oNodeReport);
        if (0 == unNodeId)
        {
            LOG4_ERROR("there is no valid node_id in the system!");
            oNodeReportRsp.set_errcode(1);
        }
        else
        {
        	oNodeReportRsp.set_node_id(unNodeId);
        	oNodeReportRsp.set_errcode(0);
            LOG4_INFO("AddNode node_id(%u)!",unNodeId);
        }
    }
    else
    {
        LOG4_ERROR("failed to parse node info json from MsgBody.data()!");
    }
    return(GetLabor()->SendToClient(stMsgShell, oMsgHead, oNodeReportRsp.SerializeAsString()));
}

} /* namespace coor */
