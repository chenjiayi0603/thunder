/*******************************************************************************
 * Project:  Net
 * @file     CmdNodeNotice.cpp
 * @brief 
 * @author   cjy
 * @date:    2019年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include <cmd/sys_cmd/CmdNodeNotice.hpp>

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

		// ── 灰度权重表处理（新增） ──
		// canary_weights: map<string,int32> → ip:port → weight
		// 按 nodeType 分组，每组收集 ip:port → weight 后批量设置
		if (m_oNodeNotice.canary_weights().size() > 0)
		{
			std::map<std::string, std::map<std::string, int32_t>> typeWeights;
			// 先收集每个 nodeType 的 ip:port → weight
			for (const auto& entry : m_oNodeNotice.canary_weights())
			{
				const std::string& ipPort = entry.first;
				int32_t weight = entry.second;

				// 从 node_arry_reg 查找对应 ip:port 的 nodeType
				for (int j = 0; j < m_oNodeNotice.node_arry_reg_size(); j++)
				{
					const auto& nr = m_oNodeNotice.node_arry_reg(j);
					std::string nodeIpPort = nr.node_ip() + ":" + std::to_string(nr.node_port());
					if (nodeIpPort == ipPort)
					{
						typeWeights[nr.node_type()][ipPort] = weight;
						break;
					}
				}
			}

			// 批量设置到 Nodes 路由表
			for (auto& tw : typeWeights)
			{
				GetLabor()->SetCanaryWeights(tw.first, tw.second);
			}
		}
		else
		{
			// 无 canary 权重 → 清除所有灰度路由，恢复一致性哈希
			GetLabor()->ClearCanaryWeights();
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
