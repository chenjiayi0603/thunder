/*******************************************************************************
 * Project:  Center
 * @file     SessionOnlineNodes.cpp
 * @brief
 * @author   Tommy
 * @date:    Sep 20, 2016
 * @note
 * Modify history:
 ******************************************************************************/
#include "SessionOnlineNodes.hpp"

namespace coor
{

    const uint32 SessionOnlineNodes::mc_uiBeat = 0x00000001;
    const uint32 SessionOnlineNodes::mc_uiLeader = 0x80000000;
    const uint32 SessionOnlineNodes::mc_uiAlive = 0x00000007; ///< 最近三次心跳任意一次成功则认为在线

    bool SessionOnlineNodes::Init(const util::CJsonObject &conf) //"CenterCmd.json"
    {
        if (!m_bInit)
        {
            m_bInit = true;
            return (InitFromLocal(conf));
        }
        return true;
    }

    /*
    "local_config":{
    "ipwhite":["192.168.11.66"],
    "//centers":"注册节点配置，用于center节点间的热备",
    "centers":["192.168.11.66:27000","192.168.11.66:27022","192.168.11.66:27032"],
    "center_beat":2.5,
    "node_types":[
    {"node_type":"ACCESS", "subscribe":["LOGIC"]},
    {"node_type":"LOGIC", "subscribe":["LOGIC", "REDISAGENT"]},
    {"node_type":"REDISAGENT", "subscribe":["DBAGENT"]},
    {"node_type":"DBAGENT", "subscribe":["LOGGER"]},
    {"node_type":"INTERFACE", "subscribe":["LOGIC", "REDISAGENT"]},
    {"node_type":"HELLO", "subscribe":["LOGIC", "REDISAGENT","DBAGENT"]},
    {"node_type":"ENTRANCE", "subscribe":["LOGIC", "REDISAGENT"]},
    {"node_type":"LOGGER", "subscribe":[]}
    ]
    }
    * */
    bool SessionOnlineNodes::InitFromLocal(util::CJsonObject oCustomConf)
    {
        oCustomConf.Get("need_leadership", m_boNeedLeadership);
        oCustomConf.Get("center_beat", m_uiCenterBeat);
        oCustomConf.Get("node_overdue", m_uiNodeOverdue);
        if (!m_boNeedLeadership)
        {
            m_bIsLeader = true;
        }
        LOG4_TRACE("%s() boNeedLeadership(%d) bIsLeader(%d) uiCenterBeat(%u)", __FUNCTION__, m_boNeedLeadership, m_bIsLeader, m_uiCenterBeat);
        InitElection(oCustomConf["centers"]);
        for (int i = 0; i < oCustomConf["ipwhite"].GetArraySize(); ++i)
        {
            AddIpwhite(oCustomConf["ipwhite"](i));
        }
        for (int i = 0; i < oCustomConf["node_types"].GetArraySize(); ++i)
        {
            for (int j = 0; j < oCustomConf["node_types"][i]["subscribe"].GetArraySize(); ++j)
            {
                AddSubscribe(oCustomConf["node_types"][i]("node_type"), oCustomConf["node_types"][i]["subscribe"](j));
            }
        }
        return true;
    }

    net::E_CMD_STATUS SessionOnlineNodes::Timeout()
    {
        LOG4_INFO("%s() WorkerIdentify(%s) IsLeader(%d)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), m_bIsLeader);
        m_uiServerTime = GetLabor()->GetNowTime();
        if (m_uiLastSendCenterBeat > 0 && m_uiServerTime >= m_uiLastSendCenterBeat + 1)
        {
            CheckLeader();
        }
        if (m_uiServerTime >= m_uiLastSendCenterBeat + m_uiCenterBeat)
        {
            SendCenterBeat(m_uiServerTime);
        }
        CheckNodesBeat();

        CheckSendingNodeNotice();
        return (net::STATUS_CMD_RUNNING);
    }

    void SessionOnlineNodes::AddIpwhite(const std::string &strIpwhite)
    {
        m_setIpwhite.insert(strIpwhite);
    }

    void SessionOnlineNodes::AddSubscribe(const std::string &strNodeType, const std::string &strBeSubscribeNodeType)
    {
        auto pub_iter = m_mapPublisher.find(strBeSubscribeNodeType);
        if (pub_iter == m_mapPublisher.end())
        {
            std::unordered_set<std::string> setSubscriber;
            setSubscriber.insert(strNodeType);
            m_mapPublisher.insert(std::make_pair(strBeSubscribeNodeType, setSubscriber));
        }
        else
        {
            pub_iter->second.insert(strNodeType);
        }
    }

    void SessionOnlineNodes::AddNodeId(uint32 uiNodeId)
    {
        if (m_setNodeId.find(uiNodeId) == m_setNodeId.end())
        {
            m_uiNodeVersion++;
            m_uiNodeVersion_LastModifyTime = GetLabor()->GetNowTime();
            m_setNodeId.insert(uiNodeId);
            m_setAddedNodeId.insert(uiNodeId);
        }
    }

    void SessionOnlineNodes::RemoveNodeId(uint32 uiNodeId)
    {
        if (m_setNodeId.find(uiNodeId) != m_setNodeId.end())
        {
            m_uiNodeVersion++;
            m_uiNodeVersion_LastModifyTime = GetLabor()->GetNowTime();
            m_setNodeId.erase(uiNodeId);
            m_setRemovedNodeId.insert(uiNodeId);
        }
    }

    bool SessionOnlineNodes::GenerateNodeId(NodeReport &oNodeInfoWithNodeId, const std::string &strNodeIdentify)
    {
        uint32 uiNodeId = oNodeInfoWithNodeId.node_id();
        // find node_id or create node_id
        auto identify_node_iter = m_mapIdentifyNodeId.find(strNodeIdentify);
        if (identify_node_iter == m_mapIdentifyNodeId.end())
        {
            while (0 == uiNodeId)
            {
                uiNodeId = ++m_unLastNodeId;
                auto node_id_iter = m_setNodeId.find(uiNodeId);
                if (node_id_iter != m_setNodeId.end())
                {
                    uiNodeId = 0;
                }
                if (m_unLastNodeId >= NODE_ID_MAX)
                {
                    m_unLastNodeId = 0;
                }
                if (m_setNodeId.size() >= NODE_ID_MAX)
                {
                    LOG4_ERROR("there is no valid node_id in the system!");
                    return (false);
                }
            }
            m_mapIdentifyNodeId.insert(std::make_pair(strNodeIdentify, std::make_pair(uiNodeId, oNodeInfoWithNodeId.node_type())));
        }
        else
        {
            if (uiNodeId > 0)
            {
                if (identify_node_iter->second.first != uiNodeId)
                {
                    RemoveNodeId(identify_node_iter->second.first);
                    identify_node_iter->second.first = uiNodeId; // 使用新数据
                }
            }
            else
            {
                uiNodeId = identify_node_iter->second.first;
            }
        }
        oNodeInfoWithNodeId.set_node_id(uiNodeId);
        return (true);
    }

    /*
    节点数据和负载状态都会保存
    oNodeInfo
    {
        "node_type": "LOGIC",
        "node_id": 2,
        "node_ip": "192.168.11.66",
        "node_port": 16068,
        "access_ip": "",
        "access_port": 0,
        "worker_num": 1,
        "active_time": 1577514574.585821,
        "node": {
            "load": 3,
            "connect": 3,
            "recv_num": 0,
            "recv_byte": 0,
            "send_num": 0,
            "send_byte": 0,
            "client": 0
        },
        "worker": [{
            "load": 3,
            "connect": 3,
            "recv_num": 0,
            "recv_byte": 0,
            "send_num": 0,
            "send_byte": 0,
            "client": 0
        }]
    }
     * */
    uint16 SessionOnlineNodes::AddNode(const NodeReport &oNodeReport, bool boRegister)
    {
        LOG4_TRACE("%s() WorkerIdentify(%s) oNodeInfo(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), oNodeReport.DebugString().c_str());
        std::string strNodeIdentify = oNodeReport.node_ip() + std::string(":") + std::to_string(oNodeReport.node_port());
        NodeReport oNodeInfoWithNodeId = oNodeReport;
        //    oNodeInfoWithNodeId.clear_node();
        //    oNodeInfoWithNodeId.clear_workers();

        if (!GenerateNodeId(oNodeInfoWithNodeId, strNodeIdentify))
        {
            LOG4_ERROR("there is no valid node_id in the system!");
            return 0;
        }

        auto node_type_iter = m_mapNode.find(oNodeInfoWithNodeId.node_type());
        if (node_type_iter == m_mapNode.end())
        {
            std::unordered_map<std::string, NodeReport> mapNodeInfo;
            mapNodeInfo.insert(std::make_pair(strNodeIdentify, oNodeInfoWithNodeId));
            m_mapNode.insert(std::make_pair(oNodeInfoWithNodeId.node_type(), mapNodeInfo));
            //        m_mapIdentifyNodeType.insert(std::make_pair(strNodeIdentify, oNodeInfoWithNodeId.node_type()));
            AddNodeId(oNodeInfoWithNodeId.node_id());
            AddNodeBroadcast(oNodeInfoWithNodeId);
            SendCenterBeat();
            return (oNodeInfoWithNodeId.node_id());
        }
        else
        {
            auto node_iter = node_type_iter->second.find(strNodeIdentify);
            if (node_iter == node_type_iter->second.end())
            {
                node_type_iter->second.insert(std::make_pair(strNodeIdentify, oNodeInfoWithNodeId));
                AddNodeId(oNodeInfoWithNodeId.node_id());
                //            m_mapIdentifyNodeType.insert(std::make_pair(strNodeIdentify, oNodeInfoWithNodeId.node_type()));
                AddNodeBroadcast(oNodeInfoWithNodeId);
                SendCenterBeat();
                return (oNodeInfoWithNodeId.node_id());
            }
            else
            {
                node_iter->second = oNodeInfoWithNodeId;
                AddNodeId(oNodeInfoWithNodeId.node_id());
                if (boRegister)
                {
                    AddNodeBroadcast(oNodeInfoWithNodeId);
                }
                SendCenterBeat();
                return (oNodeInfoWithNodeId.node_id());
            }
        }
    }

    void SessionOnlineNodes::RemoveNode(const std::string &strNodeIdentify)
    {
        LOG4_TRACE("%s() GetWorkerIdentify(%s) oNodeInfo(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), strNodeIdentify.c_str());
        auto identity_node_iter = m_mapIdentifyNodeId.find(strNodeIdentify);
        if (identity_node_iter != m_mapIdentifyNodeId.end())
        {
            auto node_type_iter = m_mapNode.find(identity_node_iter->second.second);
            if (node_type_iter != m_mapNode.end())
            {
                auto node_iter = node_type_iter->second.find(strNodeIdentify);
                if (node_iter != node_type_iter->second.end())
                {
                    uint32 uiNodeId = node_iter->second.node_id();
                    RemoveNodeId(uiNodeId);
                    RemoveNodeBroadcast(node_iter->second);
                    SendCenterBeat();
                    //                m_mapIdentifyNodeType.erase(strNodeIdentify);
                    node_type_iter->second.erase(node_iter);
                }
            }
            m_mapIdentifyNodeId.erase(strNodeIdentify);
        }
    }

    void SessionOnlineNodes::AddCenterBeat(const std::string &strNodeIdentify, const Election &oElection)
    {
        LOG4_TRACE("%s() WorkerIdentify(%s) strNodeIdentify(%s) beat.oElection(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(),
                   strNodeIdentify.c_str(), oElection.DebugString().c_str());
        if (!m_bIsLeader && oElection.is_leader() > 0)
        {
            if (oElection.last_node_id() > 0)
            {
                m_unLastNodeId = oElection.last_node_id();
            }
            for (const auto &node_id : oElection.added_node_id()) // int32 i = 0; i < oElection.added_node_id_size(); ++i
            {
                m_setNodeId.insert(node_id);
            }
            for (const auto &node_id : oElection.removed_node_id()) // int32 j = 0; j < oElection.removed_node_id_size(); ++j
            {
                m_setNodeId.erase(node_id);
            }
        }

        auto iter = m_mapCenterElection.find(strNodeIdentify);
        if (iter == m_mapCenterElection.end())
        {
            uint32 uiCenterAttr = mc_uiBeat; // 其他节点的心跳和管理权同步
            if (oElection.is_leader() > 0)
            {
                uiCenterAttr |= mc_uiLeader;
                LOG4_TRACE("%s() strNodeIdentify(%s) is leader", __FUNCTION__, strNodeIdentify.c_str());
            }
            m_mapCenterElection.insert(std::make_pair(strNodeIdentify, CenterElection(uiCenterAttr, 0)));
        }
        else
        {
            iter->second.uiElection |= mc_uiBeat;
            if (oElection.is_leader() > 0)
            {
                iter->second.uiElection |= mc_uiLeader;
                LOG4_TRACE("%s() strNodeIdentify(%s) is leader", __FUNCTION__, strNodeIdentify.c_str());
            }
            else
            {
                iter->second.uiElection &= ~mc_uiLeader;
            }
        }
    }

    void SessionOnlineNodes::GetIpWhite(util::CJsonObject &oIpWhite) const
    {
        /**
         * oIpWhite like this:
         * [
         *     "192.168.157.138", "192.168.157.139", "192.168.157.175"
         * ]
         */
        for (auto it : m_setIpwhite)
        {
            oIpWhite.Add(it);
        }
    }

    void SessionOnlineNodes::GetCenter(util::CJsonObject &oCenter) const
    {
        /**
         * oCenter like this:
         * [
         *     {"identify":"192.168.157.176:16000.1", "leader":true, "online":true},
         *     {"identify":"192.168.157.177:16000.1", "leader":false, "online":true}
         * ]
         */
        for (auto it : m_mapCenterElection)
        {
            util::CJsonObject oNode;
            oNode.Add("identify", it.first);
            if (it.second.uiElection & mc_uiLeader)
            {
                oNode.Add("leader", "yes");
            }
            else
            {
                oNode.Add("leader", "no");
            }
            if (it.second.uiElection & mc_uiAlive)
            {
                oNode.Add("online", "yes");
            }
            else
            {
                oNode.Add("online", "no");
            }
            oCenter.Add(oNode);
        }
    }

    void SessionOnlineNodes::GetSubscription(util::CJsonObject &oSubcription) const
    {
        /**
         * oSubcription like this:
         * [
         *     {"node_type":"INTERFACE", "subcriber":["LOGIC", "LOGGER"]},
         *     {"node_type":"LOGIC", "subcriber":["LOGIC", "MYDIS", "LOGGER"]}
         * ]
         */
        for (const auto &pub_iter : m_mapPublisher)
        {
            oSubcription.AddAsFirst(util::CJsonObject("{}"));
            oSubcription[0].Add("node_type", pub_iter.first);
            oSubcription[0].AddEmptySubArray("subcriber");
            for (const auto &it : pub_iter.second)
            {
                oSubcription[0]["subcriber"].Add(it);
            }
        }
    }

    void SessionOnlineNodes::GetSubscription(const std::string &strNodeType, util::CJsonObject &oSubcription) const
    {
        /**
         * oSubcription like this:
         * [
         *     "INTERFACE", "ACCESS"
         * ]
         */
        auto pub_iter = m_mapPublisher.find(strNodeType);
        if (pub_iter != m_mapPublisher.end())
        {
            for (auto it = pub_iter->second.begin(); it != pub_iter->second.end(); ++it)
            {
                oSubcription.Add(*it);
            }
        }
    }

    void SessionOnlineNodes::GetOnlineNode(util::CJsonObject &oOnlineNode) const
    {
        /**
         * oOnlineNode like this:
         * [
         *     {"node_type":"INTERFACE", "node":["192.168.157.131:16004", "192.168.157.132:16004"]},
         *     {"node_type":"LOGIC", "node":["192.168.157.131:16005", "192.168.157.132:16005"]},
         *     {"node_type":"DBAGENT", "node":["192.168.157.131:16007", "192.168.157.132:16007"]}
         * ]
         */
        for (const auto &node_iter : m_mapNode)
        {
            oOnlineNode.AddAsFirst(util::CJsonObject("{}"));
            oOnlineNode[0].Add("node_type", node_iter.first);
            oOnlineNode[0].AddEmptySubArray("node");
            for (const auto &it : node_iter.second)
            {
                oOnlineNode[0]["node"].Add(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
            }
        }
    }

    void SessionOnlineNodes::GetOnlineNode(const std::string &strNodeType, util::CJsonObject &oOnlineNode) const
    {
        /**
         * oOnlineNode like this:
         * [
         *     "192.168.157.131:16005", "192.168.157.132:16005"
         * ]
         */
        auto node_iter = m_mapNode.find(strNodeType);
        if (node_iter != m_mapNode.end())
        {
            for (const auto &it : node_iter->second)
            {
                oOnlineNode.Add(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
            }
        }
    }

    bool SessionOnlineNodes::GetNodeReport(const std::string &strNodeType, util::CJsonObject &oNodeReport) const
    {
        LOG4_TRACE("%s() strNodeType:%s", __FUNCTION__, strNodeType.c_str());
        auto node_iter = m_mapNode.find(strNodeType);
        if (node_iter == m_mapNode.end())
        {
            return (false);
        }
        else
        {
            util::CJsonObject oJson;
            for (const auto &it : node_iter->second)
            {
                if (net::Pb2Json(it.second, oJson))
                {
                    oJson.Delete("worker");
                    oNodeReport.AddAsFirst(oJson);
                }
            }
            return (true);
        }
    }

    bool SessionOnlineNodes::GetNodeReport(const std::string &strNodeType, const std::string &strIdentify, util::CJsonObject &oNodeReport) const
    {
        LOG4_TRACE("%s() strNodeType:%s strIdentify:%s", __FUNCTION__, strNodeType.c_str(), strIdentify.c_str());
        auto node_iter = m_mapNode.find(strNodeType);
        if (node_iter == m_mapNode.end())
        {
            return (false);
        }
        else
        {
            auto it = node_iter->second.find(strIdentify);
            if (it == node_iter->second.end())
            {
                return (false);
            }
            else
            {
                util::CJsonObject oJson;
                if (net::Pb2Json(it->second, oJson))
                {
                    oNodeReport.Add(oJson);
                }
                LOG4_TRACE("%s() oNodeReport:%s", __FUNCTION__, oNodeReport.ToString().c_str());
                return (true);
            }
        }
    }

    bool SessionOnlineNodes::GetOnlineNode(const std::string &strNodeType, std::vector<std::string> &vecNodes)
    {
        auto node_iter = m_mapNode.find(strNodeType);
        if (node_iter != m_mapNode.end())
        {
            for (const auto &it : node_iter->second)
            {
                vecNodes.push_back(it.second.node_ip() + ":" + std::to_string(it.second.node_port()));
            }
            return (true);
        }
        return (false);
    }

    void SessionOnlineNodes::CheckSendingNodeNotice()
    {
        m_uiServerTime = GetLabor()->GetNowTime();
        if (m_uiServerTime >= m_uiLastCheckSendingNodeNotice + 10)
        {
            m_uiLastCheckSendingNodeNotice = m_uiServerTime;
            for (auto &iter : m_mapSendingNodeNotice) // map<to_node_Identify,list<NodeNotice,timestamp>> 依赖list和tcp来保障对同节点通信顺序性
            {
                auto &sendingList = iter.second; // list<NodeNotice,timestamp>
                while (sendingList.size() > 0)
                {
                    if (sendingList.front().second + 10 <= m_uiServerTime) // 10s 后尝试重发
                    {
                        LOG4_TRACE("%s() sending to %s.timestamp(%u) uiServerTime(%u)  CMD_REQ_NODE_REG_NOTICE:%s!",
                                   __FUNCTION__, iter.first.c_str(), sendingList.front().second, m_uiServerTime, sendingList.front().first.DebugString().c_str());
                        SendNodeNotice(iter.first, sendingList.front().first, false);
                        break;
                    }
                    else if (sendingList.front().second + 30 <= m_uiServerTime) // 30s 后没响应的放弃重发//重发最多3次
                    {
                        LOG4_ERROR("%s() sending to %s failed.timestamp(%u) uiServerTime(%u)  CMD_REQ_NODE_REG_NOTICE:%s!",
                                   __FUNCTION__, iter.first.c_str(), sendingList.front().second, m_uiServerTime, sendingList.front().first.DebugString().c_str());
                        sendingList.pop_front();
                    }
                    else // 不到时间
                    {
                        break;
                    }
                }
            }
        }
    }

    void SessionOnlineNodes::SendNodeNotice(const std::string &strToNodeIdentify, const NodeNotice &oNodeNotice, bool boPushSendingList)
    {
        //(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,net::StepParam * data,net::Session*pSession)
        auto callback = [](const MsgHead &oInMsgHead, const MsgBody &oInMsgBody, net::StepParam *data, net::Session *pSession)
        {
            SessionOnlineNodes *pSessionOnlineNodes = (SessionOnlineNodes *)pSession;
            DataStepCustom *pdata = (DataStepCustom *)data;
            if (pSessionOnlineNodes && pdata && pdata->strToNodeIdentify.size() > 0)
            {
                pSessionOnlineNodes->RemoveSendingNodeNotice(pdata->strToNodeIdentify);
                LOG4_TRACE("send to %s succ CMD_REQ_NODE_REG_NOTICE.body:%s!", pdata->strToNodeIdentify.c_str(), oInMsgBody.body().c_str());
            }
            else
            {
                LOG4_WARN("send to %s succ CMD_REQ_NODE_REG_NOTICE.body:%s!", pdata->strToNodeIdentify.c_str(), oInMsgBody.body().c_str());
            }
        };
        LOG4_TRACE("%s() sending to %s CMD_REQ_NODE_REG_NOTICE.oNodeNotice:%s!", __FUNCTION__, strToNodeIdentify.c_str(), oNodeNotice.DebugString().c_str());
        // virtual bool SendToCallback(Session* pUpperSession,uint32 uiCmd,const std::string &strBody,SessionCallback callback,const std::string &nodeType,const std::string & strModFactor="",StepParam* pStepParam=nullptr){return false;}
        GetLabor()->SendToCallback(this, net::CMD_REQ_NODE_REG_NOTICE, oNodeNotice.SerializeAsString(), callback, strToNodeIdentify, "", new DataStepCustom(strToNodeIdentify)); // node_iter.first
        if (boPushSendingList)
        {
            AddSendingNodeNotice(strToNodeIdentify, oNodeNotice);
        }
    }

    void SessionOnlineNodes::AddNodeBroadcast(const NodeReport &oNodeReport)
    {
        bool boLeadership = IsLeadership();
        LOG4_TRACE("%s() WorkerIdentify(%s) IsLeadership(%d)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str(), boLeadership);
        if (!boLeadership)
        {
            return;
        }
        LOG4_TRACE("%s() %s", __FUNCTION__, oNodeReport.DebugString().c_str());
        NodeNotice oSubcribeNodeInfo;
        NodeNotice oAddNodes;
        NodeReport oAddedNodeInfo = oNodeReport;
        oAddedNodeInfo.clear_node();
        oAddedNodeInfo.clear_workers();

        (*oAddNodes.add_node_arry_reg()) = std::move(oAddedNodeInfo);
        for (const auto &sub_iter : m_mapPublisher)
        {
            for (const auto &node_type_iter : sub_iter.second)
            {
                /* send this node info to subscriber */
                if (sub_iter.first == oNodeReport.node_type())
                {
                    auto node_list_iter = m_mapNode.find(node_type_iter);
                    if (node_list_iter != m_mapNode.end())
                    {
                        LOG4_TRACE("m_mapNode[%s].size() = %u", node_type_iter.c_str(), node_list_iter->second.size());
                        for (const auto &node_iter : node_list_iter->second)
                        {
                            //						if (node_iter.second.node_id() == oNodeReport.node_id())//被订阅的是自己节点也发送
                            //						{
                            //							continue;
                            //						}
                            SendNodeNotice(node_iter.first, oAddNodes); // strToNodeIdentify
                        }
                    }
                }

                /* make subscribe node info */
                if ((node_type_iter) == oNodeReport.node_type())
                {
                    auto node_list_iter = m_mapNode.find(sub_iter.first);
                    if (node_list_iter != m_mapNode.end())
                    {
                        for (const auto &node_iter : node_list_iter->second)
                        {
                            NodeReport oExistNodeInfo = node_iter.second;
                            LOG4_TRACE("oExistNodeInfo(%s)", oExistNodeInfo.DebugString().c_str());
                            oExistNodeInfo.clear_node();
                            oExistNodeInfo.clear_workers();
                            (*oSubcribeNodeInfo.add_node_arry_reg()) = std::move(oExistNodeInfo);
                        }
                    }
                }
            }
        }

        /* send subscribe node info to this node */
        if (oSubcribeNodeInfo.node_arry_reg_size() > 0)
        {
            char szThisNodeIdentity[32];
            snprintf(szThisNodeIdentity, sizeof(szThisNodeIdentity), "%s:%u", oNodeReport.node_ip().c_str(), oNodeReport.node_port());

            SendNodeNotice(szThisNodeIdentity, oSubcribeNodeInfo);
        }
    }

    void SessionOnlineNodes::RemoveNodeBroadcast(const NodeReport &oNodeReport)
    {
        bool boLeadership = IsLeadership();
        LOG4_TRACE("%s() IsLeadership(%d)", __FUNCTION__, boLeadership);
        if (!boLeadership)
        {
            return;
        }
        LOG4_TRACE("%s() %s", __FUNCTION__, oNodeReport.DebugString().c_str());
        NodeNotice oDelNodes;
        NodeReport oDeletedNodeInfo = oNodeReport;
        //    oDeletedNodeInfo.clear_node();
        //    oDeletedNodeInfo.clear_workers();

        LOG4_TRACE("(%s)", oDeletedNodeInfo.DebugString().c_str());
        (*oDelNodes.add_node_arry_exit()) = std::move(oDeletedNodeInfo);
        LOG4_TRACE("(%s)", oDelNodes.DebugString().c_str());
        for (auto &sub_iter : m_mapPublisher)
        {
            for (auto &node_type_iter : sub_iter.second)
            {
                /* send this node info to subscriber */
                if (sub_iter.first == oNodeReport.node_type())
                {
                    auto node_list_iter = m_mapNode.find(node_type_iter);
                    if (node_list_iter != m_mapNode.end())
                    {
                        for (auto node_iter = node_list_iter->second.begin(); node_iter != node_list_iter->second.end(); ++node_iter)
                        {
                            SendNodeNotice(node_iter->first, oDelNodes);
                        }
                    }
                }
            }
        }
    }

    void SessionOnlineNodes::InitElection(const util::CJsonObject &oCenter)
    {
        //	LOG4_TRACE("%s() GetWorkerIdentify(%s) oCenter(%s)", __FUNCTION__,GetLabor()->GetWorkerIdentify().c_str(),oCenter.ToString().c_str());
        util::CJsonObject oCenterList = oCenter;
        for (int i = 0; i < oCenterList.GetArraySize(); ++i)
        {
            m_mapCenterElection.insert(std::make_pair(oCenterList(i) + ".0", CenterElection()));
        }
        if (m_mapCenterElection.size() == 0)
        {
            //    	LOG4_TRACE("%s() GetWorkerIdentify(%s) m_mapCenterElection.size() == 0 IsLeader", __FUNCTION__,GetLabor()->GetWorkerIdentify().c_str());
            BeLeader();
        }
        else if (m_mapCenterElection.size() == 1 && GetLabor()->GetWorkerIdentify() == m_mapCenterElection.begin()->first)
        {
            //    	LOG4_TRACE("%s() GetWorkerIdentify(%s) m_mapCenterElection.size() == 1 IsLeader", __FUNCTION__,GetLabor()->GetWorkerIdentify().c_str());
            BeLeader();
        }
        else
        {
            SendCenterBeat();
        }
    }

    bool SessionOnlineNodes::IsLeadership() const
    {
        if (m_boNeedLeadership)
        {
            if (m_bIsLeader)
            {
                return true;
            }
            bool isOtherLeader = false;
            for (auto &&iter : m_mapCenterElection)
            {
                if (mc_uiLeader & iter.second.uiElection)
                {
                    isOtherLeader = true;
                    break;
                }
            }
            return (false == isOtherLeader); // 在所有都不是leader时，需要暂时执行leader代理权，因为服务发现的路由可用性优先于路由数据一致性
        }
        else
        {
            return true;
        }
    }

    void SessionOnlineNodes::BeLeader()
    {
        if (!m_bIsLeader)
        {
            LOG4_TRACE("%s() WorkerIdentify(%s) BeLeader", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str());
            m_bIsLeader = true;
            m_uiBeLeaderTime = util::GetMicrosecond();
            m_mapCenterElection[GetLabor()->GetWorkerIdentify()].uiElection |= mc_uiLeader;
        }
    }

    void SessionOnlineNodes::RelievedLeader()
    {
        if (m_bIsLeader)
        {
            LOG4_TRACE("%s() WorkerIdentify(%s) RelievedLeader", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str());
            m_bIsLeader = false;
            m_uiBeLeaderTime = 0;
            m_mapCenterElection[GetLabor()->GetWorkerIdentify()].uiElection &= (~mc_uiLeader);
        }
    }

    /**
    检查并维护中心节点的 Leader 选举状态。
   */
    void SessionOnlineNodes::CheckLeader()
    {
        LOG4_TRACE("%s() WorkerIdentify(%s)", __FUNCTION__, GetLabor()->GetWorkerIdentify().c_str());
        std::multimap<uint64, std::string> mapLeaderElection; // 已经选举leader的,优先最早成为leader的
        std::string strFirstLeader;                           // 未选举的，选举存活节点中连接符值较小的
        for (auto &iter : m_mapCenterElection)
        {
            if (mc_uiAlive & iter.second.uiElection)
            {
                if (mc_uiLeader & iter.second.uiElection)
                {
                    mapLeaderElection.insert(std::make_pair(iter.second.uiBeLeaderTime, iter.first));
                }
                else if (strFirstLeader.size() == 0)
                {
                    strFirstLeader = iter.first;
                }
            }
            else
            {
                iter.second.uiElection &= (~mc_uiLeader);
            }
            { // 检查心跳
                uint32 uiLeaderBit = mc_uiLeader & iter.second.uiElection;
                iter.second.uiElection = ((iter.second.uiElection << 1) & mc_uiAlive) | uiLeaderBit; // 最近三次心跳任意一次成功则认为在线
                if (iter.first == GetLabor()->GetWorkerIdentify())
                {
                    iter.second.uiElection |= mc_uiBeat;
                }
            }
        }
        if (mapLeaderElection.size() > 0)// 已经选举leader的，根据最早成为leader的，判断是否选的本节点来处理
        {
            auto iter = mapLeaderElection.begin();
            if (iter->second == GetLabor()->GetWorkerIdentify())
            {
                BeLeader();
            }
            else
            {
                RelievedLeader();
            }
        }
        else if (strFirstLeader == GetLabor()->GetWorkerIdentify())// 未选举的，选举存活节点中连接符值较小的，判断是否选的本节点来处理
        {
            BeLeader();
        }
        //    else
        //    {
        //    	RelievedLeader();
        //    }
    }

    void SessionOnlineNodes::SendCenterBeat(uint32 uiServerTime)
    {
        //	LOG4_TRACE("%s() GetWorkerIdentify(%s)", __FUNCTION__,GetLabor()->GetWorkerIdentify().c_str());
        m_uiLastSendCenterBeat = uiServerTime;
        Election oElection;
        if (m_bIsLeader)
        {
            oElection.set_is_leader(1);
            oElection.set_last_node_id(m_unLastNodeId);
            for (const auto &it : m_setAddedNodeId)
            {
                oElection.add_added_node_id(it);
            }
            for (const auto &it : m_setRemovedNodeId)
            {
                oElection.add_removed_node_id(it);
            }
            oElection.set_beleadertime(m_uiBeLeaderTime);
        }
        else
        {
            oElection.set_is_leader(0);
        }
        m_setAddedNodeId.clear();
        m_setRemovedNodeId.clear();
        std::string strBody = oElection.SerializeAsString();
        //    LOG4_TRACE("%s() oElection(%s) %u", __FUNCTION__,oElection.DebugString().c_str(),strBody.size());
        for (auto &iter : m_mapCenterElection)
        {
            if (GetLabor()->GetWorkerIdentify() != iter.first)
            {
                auto callback = [](const MsgHead &oInMsgHead, const MsgBody &oInMsgBody, net::StepParam *data, net::Session *pSession)
                {
                    LOG4_TRACE("send succ CMD_REQ_LEADER_ELECTION:%s!", oInMsgBody.DebugString().c_str());
                };
                GetLabor()->SendToCallback(this, net::CMD_REQ_LEADER_ELECTION, strBody, callback, iter.first);
            }
        }
    }

    void SessionOnlineNodes::CheckNodesBeat()
    {
        if (m_uiNodeOverdue > 0)
        {
            m_uiServerTime = GetLabor()->GetNowTime();
            if (m_uiServerTime >= m_uiLastCheckNodesBeat + 10)
            {
                m_uiLastCheckNodesBeat = m_uiServerTime;
                std::vector<std::string> removeNodeIdentifys;
                for (const auto &iterMapNode : m_mapNode) ///< map<node_type, map<node_identify, NodeReport> >
                {
                    for (const auto &iterNodeIdentify : iterMapNode.second)
                    {
                        if (m_uiServerTime > (iterNodeIdentify.second.active_time() + m_uiNodeOverdue))
                        { // iterNodeIdentify(172.17.218.148:27021,1611802311.817800) uiServerTime(1611802317) uiNodeOverdue(35)
                            LOG4_TRACE("%s() removeNodeIdentify iterNodeIdentify(%s,%lf) uiServerTime(%u) uiNodeOverdue(%u)", __FUNCTION__,
                                       iterNodeIdentify.first.c_str(), iterNodeIdentify.second.active_time(), m_uiServerTime, m_uiNodeOverdue);
                            removeNodeIdentifys.push_back(iterNodeIdentify.first);
                        }
                        //				else
                        //				{
                        //					LOG4_TRACE("%s() iterNodeIdentify(%s,%lf) uiServerTime(%u) uiNodeOverdue(%u)", __FUNCTION__,
                        //							iterNodeIdentify.first.c_str(),iterNodeIdentify.second.active_time(),m_uiServerTime,m_uiNodeOverdue);
                        //				}
                    }
                }
                for (const auto &iterRemoveNodeIdentify : removeNodeIdentifys)
                {
                    RemoveNode(iterRemoveNodeIdentify);
                }
            }
        }
    }

}
