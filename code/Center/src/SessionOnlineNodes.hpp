/*******************************************************************************
 * Project:  Center
 * @file     SessionOnlineNodes.hpp
 * @brief 
 * @author   cjy
 * @date:     11.22, 2019
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_SESSIONNODESHOLDER_HPP_
#define SRC_SESSIONNODESHOLDER_HPP_
#include "google/protobuf/util/json_util.h"
#include "protocol/oss_sys.pb.h"
#include "Comm.hpp"

namespace coor
{

/**
 * @brief report node status
 * @note node info：
 * {
 *     "node_type":"ACCESS",
 *     "node_ip":"192.168.11.12",
 *     "node_port":9988,
 *     "gate_ip":"120.234.2.106",
 *     "gate_port":10001,
 *     "node_id":0,
 *     "worker_num":10,
 *     "active_time":16879561651.06,
 *     "node":{
 *         "load":1885792, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "sent_num":154846322, "sent_byte":648469320222,"client":495870
 *     },
 *     "worker":
 *     [
 *          {"load":655666, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "sent_num":154846322, "sent_byte":648469320222,"client":195870}},
 *          {"load":655235, "connect":485872, "recv_num":98755266, "recv_byte":98856648832, "sent_num":154846322, "sent_byte":648469320222,"client":195870}},
 *          {"load":585696, "connect":415379, "recv_num":98755266, "recv_byte":98856648832, "sent_num":154846322, "sent_byte":648469320222,"client":195870}}
 *     ]
 * }
 */
#define NODE_ID_MAX (256)

struct CenterElection
{
	CenterElection() = default;
	CenterElection(uint32 _uiElection,uint64 _uiBeLeaderTime):uiElection(_uiElection),uiBeLeaderTime(_uiBeLeaderTime){}
	uint32 uiElection = 0;
	uint64 uiBeLeaderTime = 0;
};

struct DataStepCustom:public net::StepParam
{
	explicit DataStepCustom(const std::string &strNodeIdentify):strToNodeIdentify(strNodeIdentify){LOG4_TRACE("%s() strToNodeIdentify(%s)!",__FUNCTION__,strToNodeIdentify.c_str());}
	~DataStepCustom(){LOG4_TRACE("%s() strToNodeIdentify(%s)!",__FUNCTION__,strToNodeIdentify.c_str());}
	std::string strToNodeIdentify;
};

class SessionOnlineNodes: public net::Session
{
public:
    SessionOnlineNodes(const std::string& strSessionId , ev_tstamp dSessionTimeout,const std::string& strSessionClass)
    		: net::Session(strSessionId, dSessionTimeout,strSessionClass) {}
    virtual ~SessionOnlineNodes() = default;
    static const std::string SessionClass() {return std::string("coor::SessionOnlineNodes");}
    virtual net::E_CMD_STATUS Timeout();
    virtual bool Init(const util::CJsonObject& conf)override;//"CenterCmd.json"
    bool InitFromLocal(util::CJsonObject conf);
public:
    void InitElection(const util::CJsonObject& oCenter);
    void AddIpwhite(const std::string& strIpwhite);
    void AddSubscribe(const std::string& strNodeType, const std::string& strBeSubscribeNodeType);

    uint16 AddNode(const NodeReport &oNodeReport,bool boRegister = false);
    void RemoveNode(const std::string& strNodeIdentify);
    void AddCenterBeat(const std::string& strNodeIdentify, const Election& oElection);

    void GetIpWhite(util::CJsonObject& oIpWhite) const;
    void GetCenter(util::CJsonObject& oCenter) const;
    void GetSubscription(util::CJsonObject& oSubcription) const;
    void GetSubscription(const std::string& strNodeType, util::CJsonObject& oSubcription) const;
    void GetOnlineNode(util::CJsonObject& oOnlineNode) const;
    void GetOnlineNode(const std::string& strNodeType, util::CJsonObject& oOnlineNode) const;
    bool GetNodeReport(const std::string& strNodeType, util::CJsonObject& oNodeReport) const;
    bool GetNodeReport(const std::string& strNodeType, const std::string& strIdentify, util::CJsonObject& oNodeReport) const;

    bool GetOnlineNode(const std::string& strNodeType, std::vector<std::string>& vecNodes);

    bool IsLeadership()const;//leader或者代理leader
    void BeLeader();
    void RelievedLeader();

    void AddSendingNodeNotice(const std::string &strToNodeIdentify,const NodeNotice &oNodeNotice)
	{
		m_mapSendingNodeNotice[strToNodeIdentify].push_back(std::make_pair(oNodeNotice,GetLabor()->GetNowTime()));
	}

	void RemoveSendingNodeNotice(const std::string &strToNodeIdentify)
	{
		auto iter = m_mapSendingNodeNotice.find(strToNodeIdentify);
		if (iter != m_mapSendingNodeNotice.end())
		{
			iter->second.pop_front();
		}
	}
protected:
	void CheckSendingNodeNotice();
	uint32 m_uiLastCheckSendingNodeNotice = GetLabor()->GetNowTime();

	void SendNodeNotice(const std::string& strToNodeIdentify,const NodeNotice& oNodeNotice,bool boPushSendingList = true);
    void AddNodeBroadcast(const NodeReport& oNodeReport,bool boRegister);
    void RemoveNodeBroadcast(const NodeReport& oNodeReport);
    void CheckLeader();
    void SendCenterBeat(uint32 uiServerTime = GetLabor()->GetNowTime());
    uint32 m_uiLastSendCenterBeat = 0;
    uint32 m_uiLastCheckLeader = 0;
    uint32 m_uiServerTime = GetLabor()->GetNowTime();

    void CheckNodesBeat();
    uint32 m_uiNodeOverdue = 35;//节点心跳10s，超时35s
    uint32 m_uiLastCheckNodesBeat = 0;

private:
    bool m_bInit = false;

    static const uint32 mc_uiBeat;
    static const uint32 mc_uiLeader;
    static const uint32 mc_uiAlive;
    bool m_bIsLeader = false;
    uint64 m_uiBeLeaderTime = 0;
    bool m_boNeedLeadership = true;
    uint32 m_uiCenterBeat = 3;
    
    std::unordered_set<std::string> m_setIpwhite;
    std::unordered_map<std::string, std::unordered_set<std::string> > m_mapPublisher;               ///< map<node_type, set<subscribers_node_type> >
    std::unordered_map<std::string, std::string> m_mapIdentifyNodeId;  ///< map<Identify, node_type>
    std::unordered_map<std::string, std::unordered_map<std::string, NodeReport> > m_mapOnlineNodes;  ///< map<node_type, map<node_identify, NodeReport> >
    std::map<std::string, CenterElection> m_mapCenterElection;

    std::map<std::string, std::list<std::pair<NodeNotice,uint32> >> m_mapSendingNodeNotice;//map<to_node_Identify,list<NodeNotice,timestamp>>
};

inline SessionOnlineNodes* GetSessionOnlineNodes() {return net::GetGlobalConfigSession<SessionOnlineNodes>("CenterCmd.json",1);}

}

#endif /* SRC_SESSIONNODESHOLDER_HPP_ */
