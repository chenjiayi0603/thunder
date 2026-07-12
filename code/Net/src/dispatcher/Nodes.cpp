
/*******************************************************************************
 * Project:  Net
 * @file     Nodes.cpp
 * @brief 
 * @author   cjy
 * @date:    2020-1-9
 * @note
 * Modify history:
 ******************************************************************************/
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include "cryptopp/md5.h"
#include "Nodes.hpp"
#include "labor/Labor.hpp"
#include <cstdlib>

namespace net
{

const uint32 Nodes::mc_uiAlive;

Nodes::Nodes(int iHashAlgorithm, int iVirtualNodeNum)
    : m_iHashAlgorithm(iHashAlgorithm), m_iVirtualNodeNum(iVirtualNodeNum)
{
	if (HASH_fnv1_64 == m_iHashAlgorithm)
	{
		m_hashfunc = util::hash_fnv1_64;
	}
	else if (HASH_murmur3_32 == m_iHashAlgorithm)
	{
		m_hashfunc = util::murmur3_32;
	}
	else
	{
		m_hashfunc = util::hash_fnv1a_64;
	}
}

Nodes::~Nodes()
{
    m_mapNode.clear();
}

const std::string& Nodes::GetNodeIdentify(const std::string& strNodeType, const std::string& strHashKey)
{
    uint32 uiKeyHash = m_hashfunc(strHashKey.c_str(), strHashKey.size());
    return GetNodeIdentify(strNodeType,uiKeyHash);
}

const std::string& Nodes::GetNodeIdentify(const std::string& strNodeType, uint32 uiHash)
{
    // ════════════════════════════════════════════════════════════════
    // Canary 灰度加权随机路由
    //
    // 数据流:
    //   etcd PUT /thunder/canary/{TYPE}/weights '{"v1":80,"v2":20}'
    //     → Manager gRPC Watch → AssembleAndPushRouteUpdated
    //       → 按 version 展开为 ip:port 权重 (v1 的 80 均分给所有 v1 节点)
    //         → NodeNotice.canary_weights → 共享内存 → Worker
    //           → CmdNodeNotice → SetCanaryWeights → m_mapCanaryWeights
    //             → 本函数: 加权随机选节点
    //
    // m_mapCanaryWeights 结构:
    //   "LOGIC" → {"10.0.0.1:16068"→80, "10.0.0.2:16069"→20}
    //   key=nodeType, value=map<ip:port, weight>
    //
    // 权重键不存在或被删除 → m_mapCanaryWeights 为空
    //   → 跳过本段 → 走一致性哈希（原始行为，不变量）
    // ════════════════════════════════════════════════════════════════

    auto canary_iter = m_mapCanaryWeights.find(strNodeType);
    if (canary_iter != m_mapCanaryWeights.end() && !canary_iter->second.empty())
    {
        const auto& weightMap = canary_iter->second;

        // 计算总权重
        int32_t totalWeight = 0;
        for (const auto& kv : weightMap)
            totalWeight += kv.second;

        if (totalWeight > 0)
        {
            // 加权随机: 取 [0, totalWeight) 随机数，按累积权重命中
            // 例: {"A"→70, "B"→30}, randVal=45 → A 累积=70≥45 → 命中 A
            int32_t randVal = std::rand() % totalWeight;
            int32_t acc = 0;
            for (const auto& kv : weightMap)
            {
                acc += kv.second;
                if (randVal < acc)
                {
                    // 安全检查: 节点必须在当前注册表中（防止连到已下线 Pod）
                    auto node_type_iter = m_mapNode.find(strNodeType);
                    if (node_type_iter != m_mapNode.end() &&
                        node_type_iter->second->mapNode2Hash.find(kv.first) !=
                            node_type_iter->second->mapNode2Hash.end())
                    {
                        return kv.first; // 加权随机命中
                    }
                }
            }

            // 所有权重节点都已下线 → 返回权重表中第一个存活节点
            for (const auto& kv : weightMap)
            {
                auto node_type_iter = m_mapNode.find(strNodeType);
                if (node_type_iter != m_mapNode.end() &&
                    node_type_iter->second->mapNode2Hash.find(kv.first) !=
                        node_type_iter->second->mapNode2Hash.end())
                {
                    return kv.first;
                }
            }
        }
        // totalWeight==0 (全 0 权重) 或所有节点下线 → 回退到一致性哈希
    }

    // ════════════════════════════════════════════════════════════════
    // 默认: 一致性哈希路由
    // 以下代码与 canary 无关，是 Thunder 原有路由逻辑
    // m_mapCanaryWeights 为空时直接走到这里
    // ════════════════════════════════════════════════════════════════
    auto node_type_iter = m_mapNode.find(strNodeType);
    if (node_type_iter == m_mapNode.end())
    {
    	return(m_sEmptyNodeIdentify);
    }
    else
    {
    	if (m_bCheckInternalRouter)
    	{
    		auto c_iter = node_type_iter->second->mapHash2Node.lower_bound(uiHash);
			if (c_iter == node_type_iter->second->mapHash2Node.end())
			{
				if (IsAlive(node_type_iter->second->mapHash2Node.begin()->second))//目前一致性哈希需要等待该路由节点存活,或者中心剔除路由
				{
					return node_type_iter->second->mapHash2Node.begin()->second;
				}
			}
			else
			{
				if (IsAlive(c_iter->second))
				{
					return c_iter->second;
				}
			}
    	}
    	else
    	{
    		auto c_iter = node_type_iter->second->mapHash2Node.lower_bound(uiHash);
			if (c_iter == node_type_iter->second->mapHash2Node.end())
			{
				return node_type_iter->second->mapHash2Node.begin()->second;
			}
			else
			{
				return c_iter->second;
			}
    	}
    	return(m_sEmptyNodeIdentify);
    }
}

const std::string& Nodes::GetNodeIdentify(const std::string& strNodeType)
{
    auto node_type_iter = m_mapNode.find(strNodeType);
    if (node_type_iter == m_mapNode.end())
    {
    	return(m_sEmptyNodeIdentify);
    }
    else
    {
    	if (m_bCheckInternalRouter)
    	{
    		int i = node_type_iter->second->mapNode2Hash.size();
			do
			{
				node_type_iter->second->itPollingNode++;
				if (node_type_iter->second->itPollingNode == node_type_iter->second->mapNode2Hash.end())
				{
					node_type_iter->second->itPollingNode = node_type_iter->second->mapNode2Hash.begin();
				}
				if (IsAlive(node_type_iter->second->itPollingNode->first))//目前轮询支持获取其中一个存活的节点
				{
					return node_type_iter->second->itPollingNode->first;
				}
			}while(i-- > 0);
			return(m_sEmptyNodeIdentify);
    	}
    	else
    	{
    		node_type_iter->second->itPollingNode++;
			if (node_type_iter->second->itPollingNode == node_type_iter->second->mapNode2Hash.end())
			{
				node_type_iter->second->itPollingNode = node_type_iter->second->mapNode2Hash.begin();
			}
			return node_type_iter->second->itPollingNode->first;
    	}
    }
}

const std::unordered_set<std::string>& Nodes::GetNodeIdentifys(const std::string& strNodeType)
{
	m_setEmptyIdentifySet.clear();
	auto node_type_iter = m_mapNode.find(strNodeType);
	if (node_type_iter == m_mapNode.end())
	{
		return(m_setEmptyIdentifySet);
	}
	if (m_bCheckInternalRouter)
	{
		for (auto iter = node_type_iter->second->mapNode2Hash.begin(); iter != node_type_iter->second->mapNode2Hash.end(); ++iter)
		{
			if (IsAlive(iter->first))//获取存活的节点
			{
				m_setEmptyIdentifySet.insert(iter->first);
			}
		}
		return(m_setEmptyIdentifySet);
	}
	else
	{
		for (auto iter = node_type_iter->second->mapNode2Hash.begin(); iter != node_type_iter->second->mapNode2Hash.end(); ++iter)
		{
			m_setEmptyIdentifySet.insert(iter->first);
		}
		return(m_setEmptyIdentifySet);
	}

}

void Nodes::AddNodeIdentify(const std::string& strNodeType, const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{//内部节点心跳处理
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) == m_mapNodeIdentifyHeartbeat.end())
		{
			m_mapNodeIdentifyHeartbeat[strNodeIdentify] = std::make_pair(GetLabor()->GetNowTime(),mc_uiAlive);//util::GetSecond()
		}
	}

    auto node_type_iter = m_mapNode.find(strNodeType);
    if (node_type_iter == m_mapNode.end())
    {
        std::shared_ptr<tagNode> pNode = std::make_shared<tagNode>();
        m_mapNode.insert(std::make_pair(strNodeType, pNode));
        CryptoPP::Weak::MD5 oMd5;
        char szVirtualNodeIdentify[32] = {0};
        unsigned char szDigest[CryptoPP::Weak::MD5::DIGESTSIZE] = {0};
        int32 iPointPerHash = 4;
        std::vector<uint32> vecHash;
        pNode->mapNode2Hash.insert(std::make_pair(strNodeIdentify, vecHash));
        for (int i = 0; i < m_iVirtualNodeNum / iPointPerHash; ++i)     // distribution: ketama
        {
            snprintf(szVirtualNodeIdentify, 32, "%s##%d", strNodeIdentify.c_str(), i);
            oMd5.CalculateDigest(szDigest, (const unsigned char*)szVirtualNodeIdentify, strlen(szVirtualNodeIdentify));
            for (int j = 0; j < iPointPerHash; ++j)
            {
                uint32 k = (static_cast<uint32>(szDigest[3 + j * iPointPerHash] & 0xFF) << 24)
                       | (static_cast<uint32>(szDigest[2 + j * iPointPerHash] & 0xFF) << 16)
                       | (static_cast<uint32>(szDigest[1 + j * iPointPerHash] & 0xFF) << 8)
                       | (szDigest[j * iPointPerHash] & 0xFF);
                pNode->mapNode2Hash.begin()->second.push_back(k);
                pNode->mapHash2Node.insert(std::make_pair(k, strNodeIdentify));
            }
        }
        pNode->itPollingNode = pNode->mapNode2Hash.begin();
    }
    else
    {
        auto node_iter = node_type_iter->second->mapNode2Hash.find(strNodeIdentify);
        if (node_iter == node_type_iter->second->mapNode2Hash.end())
        {
            CryptoPP::Weak::MD5 oMd5;
            char szVirtualNodeIdentify[32] = {0};
            unsigned char szDigest[CryptoPP::Weak::MD5::DIGESTSIZE] = {0};
            int32 iPointPerHash = 4;
            std::vector<uint32> vecHash;
            node_type_iter->second->mapNode2Hash.insert(std::make_pair(strNodeIdentify, vecHash));
            for (int i = 0; i < m_iVirtualNodeNum / iPointPerHash; ++i)     // distribution: ketama
            {
                snprintf(szVirtualNodeIdentify, 32, "%s##%d", strNodeIdentify.c_str(), i);
                oMd5.CalculateDigest(szDigest, (const unsigned char*)szVirtualNodeIdentify, strlen(szVirtualNodeIdentify));
                for (int j = 0; j < iPointPerHash; ++j)
                {
                    uint32 k = (static_cast<uint32>(szDigest[3 + j * iPointPerHash] & 0xFF) << 24)
                           | (static_cast<uint32>(szDigest[2 + j * iPointPerHash] & 0xFF) << 16)
                           | (static_cast<uint32>(szDigest[1 + j * iPointPerHash] & 0xFF) << 8)
                           | (szDigest[j * iPointPerHash] & 0xFF);
                    node_type_iter->second->mapNode2Hash.begin()->second.push_back(k);
                    node_type_iter->second->mapHash2Node.insert(std::make_pair(k, strNodeIdentify));
                }
            }
            node_type_iter->second->itPollingNode = node_type_iter->second->mapNode2Hash.begin();
        }
    }
}

void Nodes::DelNodeIdentify(const std::string& strNodeType, const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{//内部节点心跳处理
		m_mapNodeIdentifyHeartbeat.erase(strNodeIdentify);
	}
    auto node_type_iter = m_mapNode.find(strNodeType);
    if (node_type_iter != m_mapNode.end())
    {
        auto node_iter = node_type_iter->second->mapNode2Hash.find(strNodeIdentify);//std::unordered_map<std::string, std::vector<uint32> >
        if (node_iter != node_type_iter->second->mapNode2Hash.end())
        {
            for (const auto& hash_iter:node_iter->second)//std::vector<uint32>
            {
//                node_type_iter->second->mapHash2Node.erase(node_type_iter->second->mapHash2Node.find(*hash_iter));//std::map<uint32, std::string>
            	node_type_iter->second->mapHash2Node.erase(hash_iter);
            }
            node_type_iter->second->mapNode2Hash.erase(node_iter);
            node_type_iter->second->itPollingNode = node_type_iter->second->mapNode2Hash.begin();
        }

        if (node_type_iter->second->mapNode2Hash.empty())
        {
            m_mapNode.erase(node_type_iter);
        }
    }
}

bool Nodes::IsNodeType(const std::string& strNodeIdentify, const std::string& strNodeType)
{
    auto node_type_iter = m_mapNode.find(strNodeType);
    if (node_type_iter != m_mapNode.end())
    {
        auto node_iter = node_type_iter->second->mapNode2Hash.find(strNodeIdentify);
        if (node_iter != node_type_iter->second->mapNode2Hash.end())
        {
            return(true);
        }
    }
    return(false);
}

void Nodes::SetCheckInternalRouter(bool bCheckInternalRouter,uint32 uiHeartbeatTime)
{
	m_bCheckInternalRouter = bCheckInternalRouter;
	m_uiHeartbeatTime = uiHeartbeatTime;
}

bool Nodes::SetHeartBeat(const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) != m_mapNodeIdentifyHeartbeat.end())//内部节点心跳处理
		{
			m_mapNodeIdentifyHeartbeat[strNodeIdentify].second |= mc_uiBeat;
			m_mapNodeIdentifyHeartbeat[strNodeIdentify].first = GetLabor()->GetNowTime();
			return true;
		}
		return false;
	}
	return true;
}

bool Nodes::CheckHeartBeat(const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) != m_mapNodeIdentifyHeartbeat.end())//内部节点心跳处理
		{
			uint32 servertime = GetLabor()->GetNowTime();//util::GetSecond();
			uint32 lastchecktime = m_mapNodeIdentifyHeartbeat[strNodeIdentify].first;
			if (servertime >= lastchecktime + m_uiHeartbeatTime)//心跳最短时间限制（一般会是更长时间）
			{
				uint32 uiHeartBeat = m_mapNodeIdentifyHeartbeat[strNodeIdentify].second;
				uiHeartBeat = ((uiHeartBeat << 1) & mc_uiAlive) ;
				m_mapNodeIdentifyHeartbeat[strNodeIdentify] = std::make_pair(servertime,uiHeartBeat);
				return (uiHeartBeat > 0);//返回当前心跳存活情况.如果没有联通过的，一开始也允许存活
			}
			else
			{
				uint32 uiHeartBeat = m_mapNodeIdentifyHeartbeat[strNodeIdentify].second;
				return (uiHeartBeat > 0);//返回当前心跳存活情况.如果没有联通过的，一开始也允许存活
			}
		}
		return true;
	}
	return true;
}

//(内部连接)检查存活（最近三次心跳任意一次成功则认为在线）
bool Nodes::IsAlive(const std::string& strNodeIdentify,bool bAutoConnent)
{
	if (m_bCheckInternalRouter)
	{
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) != m_mapNodeIdentifyHeartbeat.end())//内部节点心跳处理
		{
			if ((m_mapNodeIdentifyHeartbeat[strNodeIdentify].second & mc_uiAlive) > 0)
			{
				return true;
			}
			else
			{
				if (bAutoConnent)
				{
					if (m_mapNodeIdentifyHeartbeat[strNodeIdentify].first + 5 <= GetLabor()->GetNowTime())//超过时间的不存活连接依旧尝试处理
					{
						m_mapNodeIdentifyHeartbeat[strNodeIdentify].first = GetLabor()->GetNowTime();
	//					m_mapNodeIdentifyHeartbeat[strNodeIdentify].second |= mc_uiBeat;
						LOG4_WARN("%s() strIdentify %s maybe is not alive,but try it anyway",__FUNCTION__,strNodeIdentify.c_str());
						return true;
					}
					else
					{
						LOG4_ERROR("%s() strIdentify %s is not alive",__FUNCTION__,strNodeIdentify.c_str());
						return false;
					}
				}
				else
				{
					LOG4_ERROR("%s() strIdentify %s is not alive",__FUNCTION__,strNodeIdentify.c_str());
					return false;
				}
			}
		}
		return true;//如果没有联通过的，一开始也允许存活
	}
	return true;
}

bool Nodes::CheckIsAlive(const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) != m_mapNodeIdentifyHeartbeat.end())//内部节点心跳处理
		{
			if ((m_mapNodeIdentifyHeartbeat[strNodeIdentify].second & mc_uiAlive) > 0)
			{
				LOG4_TRACE("%s() strIdentify %s is alive",__FUNCTION__,strNodeIdentify.c_str());
				return true;
			}
			else
			{
				LOG4_TRACE("%s() strIdentify %s is not alive",__FUNCTION__,strNodeIdentify.c_str());
				return true;
			}
		}
		return true;//如果没有联通过的，一开始也允许存活
	}
	return true;
}

void Nodes::ClearAlive(const std::string& strNodeIdentify)
{
	if (m_bCheckInternalRouter)
	{
		if (m_mapNodeIdentifyHeartbeat.find(strNodeIdentify) != m_mapNodeIdentifyHeartbeat.end())//内部节点心跳处理
		{
			m_mapNodeIdentifyHeartbeat[strNodeIdentify].second = 0;
			LOG4_TRACE("%s() clear strIdentify %s alive",__FUNCTION__,strNodeIdentify.c_str());
		}
	}
}

// ============================================================
// 灰度权重管理（Manager 通过 CmdNodeNotice 下发，Worker 调用）
// ============================================================

void Nodes::SetCanaryWeights(const std::string& strNodeType,
                              const std::map<std::string, int32_t>& mapWeights)
{
    if (mapWeights.empty())
    {
        m_mapCanaryWeights.erase(strNodeType);
    }
    else
    {
        m_mapCanaryWeights[strNodeType] = mapWeights;
    }
    LOG4_TRACE("SetCanaryWeights nodeType=%s entries=%zu",
               strNodeType.c_str(), mapWeights.size());
}

void Nodes::ClearCanaryWeights(const std::string& strNodeType)
{
    if (strNodeType.empty())
    {
        m_mapCanaryWeights.clear();
    }
    else
    {
        m_mapCanaryWeights.erase(strNodeType);
    }
    LOG4_TRACE("ClearCanaryWeights nodeType=%s", strNodeType.c_str());
}


} /* namespace net */
