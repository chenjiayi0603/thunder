/*
 * NodeSession.h
 *
 *  Created on: 2015年10月21日
 *      Author: chen
 */
#ifndef CODE_CENTERSERVER_SRC_NODESESSION_H_
#define CODE_CENTERSERVER_SRC_NODESESSION_H_
#include <string>
#include <map>
#include <list>
#include <time.h>
#include <utility>
#include "server.pb.h"
#include "user_basic.pb.h"
#include "dbi/MysqlDbi.hpp"
#include "session/Timer.hpp"
#include "step/StepState.hpp"
#include "step/MysqlStep.hpp"
#include "Comm.hpp"

#define NODE_LOAD_STATUS_TABLE "tb_nodeload_status"
#define NODE_LOAD_LOG_TABLE "tb_nodeload_log"
#define NODE_LOAD_STATISTICS_TABLE "tb_nodeload_statistics"
#define NODE_SERVER_CONFIG_TABLE "tb_serverconfig"
#define NODE_SERVER_CONFIG_LOG_TABLE "tb_serverconfig_log"
#define NODE_IPWHITE_TABLE "tb_ipwhite"
#define NODE_TYPE_TABLE "tb_nodetype"
#define NODE_CENTER_ACTIVE_TABLE "tb_center_active"
#define NODE_SERVER_DATA_STATUS_TABLE "tb_serverdata_status"
#define NODE_SERVER_DATA_LOG_TABLE "tb_serverdata_log"
#define NODE_CONFIG_FILES_TABLE "tb_config_files"

enum NodeStatus
{
    eNodeStatus_Online = 1,
    eNodeStatus_Offline = 2,
};

//中心主节点处理服务器内部消息，中心从节点作为热备份。两个节点都处理外部请求业务
enum CenterStatus
{
    eOfflineStatus = 0,
    eMasterStatus = 1,
    eSlaveStatus = 2,
};

namespace core
{
struct LoadConfigSendToMysqlParam:public net::StepStateParam//mysql访问参数
{
	LoadConfigSendToMysqlParam(NodeSession* pSess):pNodeSession(pSess){}
	NodeSession* pNodeSession;
};

class NodeSession: public net::Timer
{
public:
    NodeSession(uint32 ulSessionId, ev_tstamp dSessionTimeout,
                    const std::string& strSessionName="net::NodeSession"):
        Timer(ulSessionId, dSessionTimeout, strSessionName),
            boInit(false),m_nCheckActiveCounter(0),m_nNodeActiveTimeOut(10),m_nNodeTimeBeat(0),m_nodeId(0),
            m_currentTime(0),m_centerInnerPort(0), m_centerProcessNum(
            0), m_NodeRecentlyTime(30),m_nodeOfflineTimeInterval(0), m_deleteOfflineNodeTimeInterval(
            0), m_nodeReportTimeInterval(0), m_nodeStatusCheckTimeInterval(
            0), m_nodeLoadLogTimeInterval(0), m_nodeLoadLogOverdue(
            0), m_nodeLoadStatisticsTimeInterval(0), m_nodeLoadStatisticsOverdue(
            0), m_nodeLoadCheckTimeInterval(0), m_serverDataLoadCheckTimeInterval(
            0), m_serverDataLoadStatusLogOverdue(0), m_nodeLoadLogInsertLastTime(
            0), m_nodeLoadStatisticsInsertLastTime(0), m_nodeLoadCheckLastTime(
            0), m_nodeStatusCheckLastTime(0),m_serverDataLoadCheckLastTime(0)
    {
        SetCurrentTime();
        m_dbport = 0;
        m_InitSessionTime = 0;
        m_CenterActive.activetime = m_currentTime;
        m_CenterActive.status = eOfflineStatus;//根据仲裁来判断
        m_nNodeActiveTimeOut = dSessionTimeout * 2 + 1;//4*2 + 1= 9s 中心活跃超时时间
        m_nNodeTimeBeat = dSessionTimeout;//中心活跃上报时间
        m_pSyncMysqlDbi = NULL;
    }
    virtual ~NodeSession();
    net::E_CMD_STATUS Timeout();
    //读取配置
    bool ReadConfig(const std::string& configPath);
    //初始化
    bool Init(const std::string& configPath,std::string &err,bool boReload=false);
public:
    /* ******************* 服务器路由功能     * */
    //加载服务路由配置
	bool LoadNodeRoute();
    //加载节点类型
	bool LoadNodeType();
	bool LoadNodeType(util::T_vecResultSet &vecRes);
	//加载服务器白名单
	bool LoadWhiteList();
	bool LoadWhiteList(util::T_vecResultSet &vecRes);
    //注册节点
    int RegNode(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,const MsgBody& oInMsgBody, const NodeStatusInfo& nodeinfo);
    //更新节点
    int UpdateNode(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo& nodeinfo);
    //删除节点
    bool DelNode(const std::string& delNodeIdentify);
	//注册节点路由(已有节点会更新).注册失败返回错误码
	//注册成功会发送以下消息：	//(1)发送返回注册响应,会分配节点id	//(2)发送注册服务器的配置信息(注册成功后才调用)	//(3)发送其他服务器给注册者、发送注册者给其它服务
	//注册返回信息：（1）注册响应,会分配节点id（2）发送注册服务器的配置信息(注册成功后才调用)（3）给注册者发其他服务器通知、注册者的给其它服务发通知    //返回注册响应,会分配节点id
	int RegNodeRoute(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo& nodeinfo);
	//注销节点路由
	int UnregNodeRoute(const NodeStatusInfo& nodeinfo);
	//检查节点类型
	bool CheckNodeType(const std::string& nodeType);
	//发送返回注册响应,会分配节点id
	bool ResponseNodeReg(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,int iRet,const NodeStatusInfo &regNodeStatus);
	//发送其他服务器给注册者
	int SendOthersToReg(const net::tagMsgShell& stMsgShell,const NodeStatusInfo &regNodeStatus);
	//发送注册者给其它服务
	int SendRegToOthers(const NodeStatusInfo &regNodeStatus);
	//发送中心服务器给注册者
	int SendCenterToReg(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo &regNodeStatus);
	//发送连接断开通知到其它服务
	int SendUnregToOthers(const NodeStatusInfo &delNodeInfo);
	//获取新的节点id
	uint32 GetNewNodeID();
	// 添加标识的节点信息到在线节点管理器中去
	void AddNodeInfo(const std::string& NodeKey, const NodeStatusInfo& Info);
	//从在线节点管理器删除节点信息
	bool DelNodeInfo(const std::string& NodeKey);
	//从在线节点管理器中获取节点信息
	bool GetNodeInfo(const std::string &NodeKey, NodeStatusInfo &nInfo);
	//从在线节点管理器中获取节点信息
	NodeStatusInfo *GetNodeInfo(const std::string &NodeKey);
	//获取指定类型节点的在线数量
	uint32 GetNodeCountByType(const std::string &nodeType);
	//获取节点数量
	uint32 GetMapNodeInfoSize(){return m_mapNodesStatus.size();}
	//检查白名单
	bool CheckWhiteNode(const std::string& nodeInnerIp);
	//根据节点类型获取所有节点状态
	bool GetNodeStatusByNodeType(const std::string & nodetype,std::vector<NodeLoadStatus>& vecNodeStatus);
	//获取服务器类型的配置信息
	const NodeType* GetNodeTypeServerInfo(const std::string &nodeType);
	//获取需要的已注册的服务器的节点状态(json格式)
	bool GetNeededNodesStatus(const std::vector<std::string>& neededServers,util::CJsonObject &jObj);
	//获取中心服务器的节点状态(json格式)
	bool GetCenterNodesStatus(util::CJsonObject &jObj);
	//写节点到数据库，如果是报告信息需要设置boReport = true
	bool WriteNodeDataToDB(const NodeStatusInfo& nodeInfo, bool boReport = false);
	//设置服务器为下线
	bool SetNodeDataOfflineToDBByNodeId(int node_id);
    //删除超时的下线节点状态到数据库
    bool ClearOverdueOfflineNodeStatusToDB();
    //检查设置超时节点为下线到数据库
    bool CheckOfflineNodeStatusToDB();
    //替换插入节点的状态（为上线）到数据库
    bool ReplaceNodeStatusToDB(const NodeStatusInfo& nodeInfo);
    //(根据节点ip和端口)删除指定节点的状态到数据库
    bool ClearNodeStatusToDB(const NodeStatusInfo& nodeInfo);
    //写当前的节点状态
    bool WriteNodeStatus(const NodeStatusInfo& nodeInfo);
    //检查节点状态
    bool CheckNodesStatus();
    //写入节点日志到数据库
    bool WriteNodeLog(const NodeStatusInfo& nodeStatusInfo);
    //插入节点日志到数据库
    bool InsertNodeLogToDB();
    //写入节点统计到数据库
    bool WriteNodeStatistics(const NodeStatusInfo& nodeInfo);
    //插入数据到节点统计表
    bool InsertNodeStatisticsToDB();
    //节点负载检查
    bool CheckNodeload();
    //清理数据库中的节点日志数据中的超时信息
    bool ClearOverdueOfflineNodeLogToDB();
    //清理数据库中的统计节点数据中的超时信息
    bool ClearOverdueOfflineNodeStatisticsToDB();
    //获取最小负载节点
	int GetLoadMinNode(const std::string& serverType,NodeLoadStatus &nodeLoadStatus);


    /* ********************灰度功能（以及热备份功能）* */
	//下线节点
	int OfflineNode(const std::string& sOfflineNodeIdentify);
	//上线节点
	int OnlineNode(const std::string& sOnlineNodeIdentify);
	//检查能否操作上线
	int CanOnlineNode(const std::string& sOnlineNodeIdentify);
	//检查中心活跃信息
	bool CheckCenterActive();
	bool CheckCenterActive(util::T_vecResultSet &vecRes);
	bool SelectCenterMaster();
	//更新中心活跃状态
	bool UpdateCenterStatus(CenterStatus status,bool boPromote=false);
	//检查正在运行并被挂起的节点
	bool CheckNodeSuspend(NodeStatusInfo& nodeinfo);
	//发送下线通知到网关服务
	int SendOfflineToGateway(const NodeStatusInfo &offlineNodeInfo);
	//发送上线通知到网关服务
	int SendOnlineToGateway(const NodeStatusInfo& onlineNodeInfo);
	//是否是网关类型
	bool IsGatewayType(const std::string& nodetype);
	bool IsMaster()const {return (eMasterStatus == m_CenterActive.status);}
	bool IsSlave()const{return (eSlaveStatus == m_CenterActive.status);}
	//获取自身的标识符ip:port
	std::string GetSelfNodeIdentify()const;
	bool IsSelfNodeIdentify(const std::string& identify)const;
	bool IsCenterServer(const std::string& identify);
	bool HasIdentifyAuthority(const std::string& sNodeIdentify);


	/* ********************服务器配置管理功能* */
	//加载配置文件
	bool LoadServerConfig();
	bool LoadServerConfig(util::T_vecResultSet &vecRes);
    //更新节点配置
	int UpdateServerConfig(const server::update_server_config_req &oUpdateServerConfigReq,server::update_server_config_ack &oUpdateServerConfigAck);
	//检查管理者消息
	int CheckMgrMsg(const MsgBody& oInMsgBody,server::user_basic &basicInfo);

    //检查能否操作重新加载逻辑配置
    int CanReloadConfigNode(const std::string& sOnlineNodeIdentify,std::string &nodeType);
    //检查节点状态
    bool CheckNodeStatus(const NodeStatusInfo& nodeinfo);
    //检查配置内容
    int CheckServerConfigContent(const NodeConfigFile &nodeConfigFile,const util::CJsonObject &checkConfigContent,uint32 config_type,std::string &config_file);
	//加载服务器配置
	//node_type:节点类型   //config_type:节点配置类型   //config_content:节点内容   //config_file:节点配置文件名   //update_time:节点配置更新时间    //auto_send:是否自动下发    //reload_config:是否在线已加载配置，0：不是，1：是
	bool LoadServerConfig(const std::string &node_type,uint32 config_type,std::string& config_content,std::string &configFile,uint32 &update_time,uint32 &auto_send,uint32 &reload_config);
	//检查db中的更新配置
	int CheckServerConfigFromDB(const std::string &node_type,uint32 config_type,const std::string& config_content,const std::string &config_file,uint32 auto_send);
	//更新服务器配置
	//node_type:节点类型//config_type:节点配置类型    //config_content:节点内容	//config_file:节点配置文件名    //auto_send:是否自动下发0：不是，1：是    //reload_config:是否在线已加载配置，0：不是，1：是
	int UpdateServerConfigToDB(const std::string &node_type,uint32 config_type,const std::string& config_content,const std::string &config_file,uint32 auto_send,uint32 reload_config);
	//获取服务器配置文件
	bool GetServerConfigFile(const std::string &nodeType,int configType,NodeConfigFile &nodeConfigFile);
	//发送注册服务器的配置信息(注册成功后才调用)
	bool SendServerConfig(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo &regNodeStatus);
	//发送服务器的配置信息到指定类型节点
	bool SendServerConfigToType(const std::string& node_type,int config_type,const util::CJsonObject& objConfigContent,const std::string& config_file,const std::string &sNodeIdentify);
	//广播加载逻辑配置
	bool ReloadServerConfigToType(const std::string& node_type,const NodeConfigFile &nodeConfigFile);

    /* ***********服务数据接口*/
    bool WriteServerDataToDB(const char* nodetype, int innerport,const char* innerip, int outerport, const char* outerip,const char* status);
    bool ServerDataLoadCheck();
	bool ClearOverdueServerDataLogToDB();
	bool ReplaceServerDataLoadStatusToDB(const char* nodetype,int innerport, const char* innerip, int outerport,const char* outerip, const char* status);
	bool WriteServerDataLoadLogToDB(const char* nodetype,int innerport, const char* innerip, int outerport,const char* outerip, const char* status);

	/* ***********通用接口   */
	//广播给所有指定类型的服务器节点的管理者
	bool SendToNodeType(const std::string& strNodeType, const MsgHead& oOutMsgHead, const MsgBody& oOutMsgBody);
	//定时检查时间
	int GetNodeBeat()const{return m_nNodeActiveTimeOut;}
	void SetCurrentTime(){m_currentTime = ::time(NULL);}
	//获取新的seq
	int GetSequence(){return GetLabor()->GetSequence();}
	//获取mysql last error
	const std::string& GetSyncLastMysqlError() const{ return m_pSyncMysqlDbi->GetError();}
	int GetSyncLastMysqlErrno() const {return m_pSyncMysqlDbi->GetErrno();}
	//获取 mysql连接对象
	util::CMysqlDbi* GetSyncMysqlDbi() const{return m_pSyncMysqlDbi;}
	//去掉符号
	void RemoveFlag(std::string &str, char flag = '\"')const;
private:
    //中心活跃状态
    CenterActive m_CenterActive;
    //节点类型配置
    NodeTypesVec m_vecNodeTypes;
    //服务器白名单
    std::vector<WhiteNode> m_vecWhiteNode;
    //中心服务器状态
    std::vector<CenterActive> m_vecCenterActive;
    //节点状态管理器（key为节点类型：IP：端口，value为节点状态信息）
    typedef std::map<std::string, NodeStatusInfo> NodesStatusMap;
    typedef NodesStatusMap::iterator NodesStatusMapIT;
    typedef NodesStatusMap::const_iterator NodesStatusMapCIT;
    NodesStatusMap m_mapNodesStatus;
    //节点日志管理器（key为节点类型：IP：端口，value为节点日志信息）
    typedef std::map<std::string, NodeLogInfo> NodesLogMap;
    typedef NodesLogMap::iterator NodesLogMapIT;
    typedef NodesLogMap::const_iterator NodesLogMapCIT;
    NodesLogMap m_mapNodesLog;
    //节点状态统计管理器（key为节点类型：IP：端口，value为节点统计信息）
    typedef std::map<std::string, NodesStaisticsInfo> NodesStatisticsMap;
    typedef NodesStatisticsMap::iterator NodesStatisticsMapIT;
    typedef NodesStatisticsMap::const_iterator NodesStatisticsMapCIT;
    NodesStatisticsMap m_mapNodesStatistics;

    bool boInit;
    int m_nCheckActiveCounter;//检查活跃计数器
    int m_nNodeActiveTimeOut;//节点活跃超时时间
    int m_nNodeTimeBeat;//节点心跳时间间隔

    //中心服务器配置CenterCmd.json
    util::CJsonObject m_oCurrentConf;       ///< 当前加载的配置

    std::string m_dbip;
    std::string m_dbuser;
    std::string m_dbpwd;
    std::string m_dbname;
    std::string m_dbcharacterset;
    int m_dbport;
    util::tagDbConnInfo m_dbConnInfo;

    //config
    std::vector<std::string> m_configIpwhite;
    util::CJsonObject m_configNodetype;

    //节点分配器
    uint32 m_nodeId;
    //当前时间
    uint64 m_currentTime;
    //初始化session时间
    uint64 m_InitSessionTime;
    /*数据库连接配置,如：
       "dbip":"192.168.18.68", "dbport":3395, "dbuser":"analysis", "dbpwd":"robot123456", "dbname":"db_im3_center", "dbcharacterset":"utf8",
     * */
    util::CMysqlDbi* m_pSyncMysqlDbi; //数据库连接(同步,目前只有运维操作使用)
    /*路由配置，如：
    * "center_inner_host":"192.168.18.68",  "center_inner_port":15000,  "center_node_type":"CENTER",  "center_process_num":1
    * */
    std::string m_centerInnerHost;//中心服务器内网地址
    int m_centerInnerPort;//中心服务器内网端口
    std::string m_centerNodeType;//中心服务器节点类型
    int m_centerProcessNum;//中心服务器工作进程数

    int m_NodeRecentlyTime;//最近统计时间

    std::vector<std::string> m_GatewayTypeList;//网关类型列表

    //节点配置文件
    std::vector<NodeConfigFile> m_vecNodeConfigFile;
    //中心服务器列表
    std::vector<CenterServer> m_vecCenterServer;

    /*负载统计配置（后面的是对应配置文件中的字段）*/
    //检查节点下线时间间隔。节点一段时间不活跃，则设置其为下线状态
    //"nodeoffline_timeinterval":20
    int m_nodeOfflineTimeInterval;
    //下线节点信息过期时间，下线节点过期后需要删除	"deleteofflinenode_timeinterval":86400（1天）
    int m_deleteOfflineNodeTimeInterval;
    //节点上报时间间隔"nodereport_timeinterval":10 (目前未使用，暂时设置为10)
    int m_nodeReportTimeInterval;
    //节点状态检查时间间隔,每隔一段时间才检查一次数据表tb_nodeload_status "nodestatuscheck_timeinterval":60
    int m_nodeStatusCheckTimeInterval;
    //节点负载日志写入时间间隔，每隔一段时间才写入表tb_nodeload_log "nodeloadlog_timeinterval":60
    int m_nodeLoadLogTimeInterval;
    //节点负载日志过时时间，表tb_nodeload_log中的数据过期需要删除 "nodeloadlog_overdue":5184000（60天）
    int m_nodeLoadLogOverdue;
    //节点负载统计时间间隔，每隔一段时间写入表tb_nodeload_statistics "nodeloadstatistics_timeinterval":300
    int m_nodeLoadStatisticsTimeInterval;
    //节点负载统计过时时间,表tb_nodeload_statistics中的数据过期需要删除 "nodeloadstatistics_overdue":5184000（60天）
    int m_nodeLoadStatisticsOverdue;
    //节点负载检查时间(检查节点负载的日志和统计的时间间隔),表tb_nodeload_log和tb_nodeload_statistics每隔一段时间才检查过期  "nodeloadcheck_timeinterval":86400（1天）
    int m_nodeLoadCheckTimeInterval;
    //服务器数据负载日志检查时间间隔,表tb_serverdata_log每隔一段时间才检查过期  "serverdataloadstatuslogcheck_timeinterval":60
    int m_serverDataLoadCheckTimeInterval;
    //服务器数据负载日志过时时间,表tb_serverdata_log中的数据过期需要删除  "serverdataloadstatuslog_overdue":5184000（60天）
    int m_serverDataLoadStatusLogOverdue;
    /*记录时间，分钟在线数据修改为统计当前一分钟内各秒的值的平均值和最大值，分别记录每分钟在线用户的平均值和最大值*/
    //节点负载最后写表tb_nodeload_log的时间    写表时间间隔m_nodeLoadLogTimeInterval（60s）
    uint64 m_nodeLoadLogInsertLastTime;
    //节点负载统计最后写表tb_nodeload_statistics的时间   写表时间间隔m_nodeLoadStatisticsTimeInterval（300s）
    uint64 m_nodeLoadStatisticsInsertLastTime;
    //节点负载最后检查时间
    //（1）每隔一段时间m_nodeLoadCheckTimeInterval（1天）才检查 节点负载记录
    //(1)需要清除 tb_nodeload_log 中过期数据，根据m_nodeLoadLogOverdue过时时间  (2)需要清除 tb_nodeload_statistics 中过期数据 ,根据 m_nodeLoadStatisticsOverdue过时时间（60天）
    uint64 m_nodeLoadCheckLastTime;
    //节点状态最后检查时间
    //每隔一段时间m_nodeStatusCheckTimeInterval（60s）才检查节点状态表tb_nodeload_status  需要清除表 tb_nodeload_status 中过期的下线节点，过期时间根据  m_deleteOfflineNodeTimeInterval（1天）
    uint64 m_nodeStatusCheckLastTime;
    //节点数据负载最后检查时间
	//每隔一段时间m_serverDataLoadCheckTimeInterval（60s）才检查过期服务器数据负载日志表tb_serverdata_log  需要清除tb_serverdata_log中过期日志，过期时间根据m_serverDataLoadStatusLogOverdue（60天）
	uint64 m_serverDataLoadCheckLastTime;
};

NodeSession* GetNodeSession(net::Labor* pLabor,const std::string &configPath,bool boReload=false);

}
;

#endif /* CODE_CENTERSERVER_SRC_NODESESSION_H_ */
