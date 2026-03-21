/*******************************************************************************
 * Project:  Center
 * @file     CmdRegister.cpp
 * @brief 
 * @author   cjy
 * @date:    Sep 19, 2016
 * @note
 * Modify history:
 ******************************************************************************/
#include "protocol/oss_sys.pb.h"
#include "CmdNodeRegister.hpp"

MUDULE_CREATE(coor::CmdNodeRegister);

namespace coor
{

bool CmdNodeRegister::Init()
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

/**
 * @brief 上报节点状态信息
 * @return 上报是否成功
 * @note 节点状态信息结构如：
 * {
 *     "node_type":"ACCESS",
 *     "node_ip":"192.168.11.12",
 *     "node_port":9988,
 *     "access_ip":"120.234.2.106",
 *     "access_port":10001,
 *     "worker_num":10,
 *     "active_time":16879561651.06,
 *     "node":{
 *         "load":1885792, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":495870
 *     },
 *     "worker":
 *     [
 *          {"load":655666, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}},
 *          {"load":655235, "connect":485872, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}},
 *          {"load":585696, "connect":415379, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}}
 *     ]
 * }
 */
bool CmdNodeRegister::AnyMessage(
                const net::tagMsgShell& stMsgShell,const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s() NodeRegister oMsgHead.cmd:%u)", __FUNCTION__, oMsgHead.cmd());
	NodeReport oNodeReport;
	NodeReportRsp oNodeReportRsp;
    if (oNodeReport.ParseFromString(oMsgBody.body()))
    {
        std::string strNodeIdentify = oNodeReport.node_ip() + std::string(":") + std::to_string(oNodeReport.node_port());
        LOG4_TRACE("%s() NodeRegister oMsgHead.cmd:%u nodeIdentify:%s", __FUNCTION__, oMsgHead.cmd(),strNodeIdentify.c_str());
        uint16 unNodeId = m_pSessionOnlineNodes->AddNode(oNodeReport,true);
        if (0 == unNodeId)
        {
            LOG4_ERROR("failed to AddNode ! nodeIdentify:%s (unexpected: AddNode should assign id when report node_id was 0)",
                        strNodeIdentify.c_str());
            oNodeReportRsp.set_errcode(1);
        }
        else
        {
        	LOG4_INFO("NodeRegister AddNode node_id(%u)!",unNodeId);
            oNodeReportRsp.set_node_id(unNodeId);
            oNodeReportRsp.set_errcode(0);
        }
    }
    else
    {
        LOG4_ERROR("failed to parse node info json from MsgBody.data()!");
        oNodeReportRsp.set_errcode(1);
    }
    LOG4_INFO("oNodeReportRsp(%s)!",oNodeReportRsp.DebugString().c_str());
    return GetLabor()->SendToClient(stMsgShell,oMsgHead,oNodeReportRsp.SerializeAsString());
}

} /* namespace coor */
