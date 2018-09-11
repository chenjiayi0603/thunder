/*
 * NodeSession.cpp
 *
 *  Created on: 2015年11月6日
 *      Author: chen
 */
#include "cmd/CW.hpp"
#include "Comm.hpp"
#include "NodeSession.h"

namespace core
{
#define LOAD_CENTER_CMD(centerconf,name,member) if (!centerconf.Get(name, member))  \
                { char errStr[64];snprintf(errStr,sizeof(errStr),"center cmd load(%s) failed",name);err = errStr;return false;}


virtual NodeSession::~NodeSession()
{
	if (m_pSyncMysqlDbi)
	{
		delete m_pSyncMysqlDbi;
		m_pSyncMysqlDbi = NULL;
	}
}
net::E_CMD_STATUS NodeSession::Timeout()
{//定时检查中心活跃状态
	LOG4_DEBUG("NodeSession::Timeout CheckCenterActive nNodeActiveTimeOut:%d",m_nNodeActiveTimeOut);
	CheckCenterActive();//定时检查中心活跃状态
	return net::STATUS_CMD_RUNNING;
}

bool NodeSession::ReadConfig(const std::string& configPath)
{
    util::CJsonObject oCenterConfJson;
    //配置文件路径查找
    std::string strConfFile = configPath + std::string("/CenterCmd.json");
    LOG4_DEBUG("CONF FILE = %s.",strConfFile.c_str());
    std::ifstream fin(strConfFile.c_str());
    //配置信息输入流
    if (fin.good())
    {
        //解析配置信息 JSON格式
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        if (!m_oCurrentConf.Parse(ssContent.str()))
        {
            //配置文件解析失败
            LOG4_ERROR("Read conf (%s) error,it's maybe not a json file!",
                            strConfFile.c_str());
            ssContent.str("");
            fin.close();
            return false;
        }
        ssContent.str("");
		fin.close();
    }
    else
    {
        //配置信息流读取失败
        LOG4_ERROR("Open conf (%s) error!",strConfFile.c_str());
        return false;
    }
    return true;
}

bool NodeSession::GetServerConfigFile(const std::string &nodeType,int configType,NodeConfigFile &nodeConfigFile)
{
    nodeConfigFile.clear();
	if(!LoadServerConfig())
	{
		LOG4_WARN("failed to LoadServerConfig");
		return false;
	}
    std::vector<NodeConfigFile>::const_iterator it = m_vecNodeConfigFile.begin();
    std::vector<NodeConfigFile>::const_iterator itEnd = m_vecNodeConfigFile.end();
    for(;it != itEnd;++it)
    {
        if (it->config_type == configType && it->node_type == nodeType)
        {
            nodeConfigFile = *it;
            return true;
        }
    }
    return false;
}

bool NodeSession::LoadServerConfig()
{
    if(!m_vecNodeConfigFile.empty())
    {
        LOG4_TRACE("LoadServerConfig already");
        return true;
    }
    auto mysqlCallback = [](net::StepState* state)
	{
		STAGE_TEST_PARAM_LOG(LoadConfigSendToMysqlParam,state,"mysqlCallback");
		net::MysqlStep* pMysqlState = (net::MysqlStep*)state;
		if (pMysqlState->m_pMysqlResSet)
		{
			util::T_vecResultSet vecRes;
			if (pMysqlState->m_pMysqlResSet->GetResultSet(vecRes) > 0)
			{
				pStageParam->pNodeSession->LoadServerConfig(vecRes);
				LOG4_TRACE_S(state,"LoadServerConfigStateSendToMysqlCallback ok vecRes size:%u",vecRes.size());
			}
		}
		return true;
	};
	//读取配置文件
	net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_select,"select * from %s", NODE_CONFIG_FILES_TABLE);//第一个任务
	pstep->AddStateFunc(mysqlCallback);//stage 0
	pstep->SetData(new LoadConfigSendToMysqlParam(this));
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return true;
}

bool NodeSession::LoadServerConfig(util::T_vecResultSet &vecRes)
{
	m_vecNodeConfigFile.clear();
	NodeConfigFile nodeConfigFile;
	for (util::T_vecResultSet::iterator it = vecRes.begin(); it != vecRes.end();
					++it)
	{
		nodeConfigFile.clear();
		util::T_mapRow& valmap = *it;
		//node_type
		util::T_mapRow::iterator mapit = valmap.find("node_type");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeConfigFile.node_type = valmap["node_type"];
		//config_type
		mapit = valmap.find("config_type");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeConfigFile.config_type = atoi(valmap["config_type"].c_str());
		//file_name
		mapit = valmap.find("file_name");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeConfigFile.file_name = valmap["file_name"];
		//check
		mapit = valmap.find("check");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeConfigFile.check = atoi(valmap["check"].c_str());
		//description
		mapit = valmap.find("description");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeConfigFile.description = valmap["description"];
		//cmd
		mapit = valmap.find("cmd");
		if (valmap.end() == mapit)
		{
			continue;
		}
		const std::string& cmds = valmap["cmd"];
		LOG4_DEBUG("LoadServerConfig node_type(%s) cmd(%s)",nodeConfigFile.node_type.c_str(),
						cmds.c_str());
		if (!cmds.empty())
		{
			util::CJsonObject jParse;
			if (jParse.Parse(cmds) && jParse.IsArray())
			{
				nodeConfigFile.cmds = cmds;
			}
			else
			{
				LOG4_WARN("LoadServerConfig parse cmd(%s) failed",
								cmds.c_str());
			}
		}
		//url_path
		mapit = valmap.find("url_path");
		if (valmap.end() == mapit)
		{
			continue;
		}
		const std::string& url_paths = valmap["url_path"];
		LOG4_DEBUG("LoadServerConfig node_type(%s) url_paths(%s)",nodeConfigFile.node_type.c_str(),
						url_paths.c_str());
		if (!url_paths.empty())
		{
			util::CJsonObject jParse;
			if (jParse.Parse(url_paths) && jParse.IsArray())
			{
				nodeConfigFile.url_paths = url_paths;
			}
			else
			{
				LOG4_WARN("LoadServerConfig parse url_paths(%s) failed",
								url_paths.c_str());
			}
		}
		//nessesary_fields
		mapit = valmap.find("nessesary_fields");
		if (valmap.end() == mapit)
		{
			continue;
		}
		const std::string& nessesary_fields = valmap["nessesary_fields"];
		LOG4_DEBUG("LoadServerConfig node_type(%s) nessesary_fields(%s)",nodeConfigFile.node_type.c_str(),
						nessesary_fields.c_str());
		if (!nessesary_fields.empty())
		{
			util::CJsonObject jParse;
			if (jParse.Parse(nessesary_fields) && jParse.IsArray())
			{
				for (int i = 0; i < jParse.GetArraySize(); ++i)
				{
					std::string str_nessesary_field = jParse[i].ToString();
					LOG4_DEBUG("LoadServerConfig parse nessesary_field(%s)",
									str_nessesary_field.c_str());
					RemoveFlag(str_nessesary_field);
					LOG4_DEBUG("LoadServerConfig parse str_nessesary_field(%s)",
									str_nessesary_field.c_str());
					nodeConfigFile.nessesary_fields.push_back(str_nessesary_field);
				}
			}
			else
			{
				LOG4_WARN("LoadServerConfig parse nessesary_fields(%s) failed",
								nessesary_fields.c_str());
			}
		}
		m_vecNodeConfigFile.push_back(nodeConfigFile);
	}
	return true;
}

bool NodeSession::Init(const std::string& configPath,std::string &err,bool boReload)
{
    if(!boReload)
    {
        if (boInit)
        {
            return true;
        }
    }
    SetCurrentTime();
    if(m_InitSessionTime + m_nNodeTimeBeat >= m_currentTime)
    {
        LOG4_INFO("m_InitSessionTime(%llu),NodeTimeBeat(%d),currentTime(%llu),Init too often",
                        m_InitSessionTime,m_nNodeTimeBeat,m_currentTime);
        return true;
    }
    m_InitSessionTime = m_currentTime;
    if(!ReadConfig(configPath))
    {
        LOG4_ERROR("Read conf (%s) error",configPath.c_str());
        err = "Read conf error";
        return false;
    }
    const util::CJsonObject& conf = m_oCurrentConf;
    LOAD_CENTER_CMD(conf,"dbip", m_dbip);//连接db地址
    LOAD_CENTER_CMD(conf,"dbuser", m_dbuser);
    LOAD_CENTER_CMD(conf,"dbpwd", m_dbpwd);
    LOAD_CENTER_CMD(conf,"dbname", m_dbname);
    LOAD_CENTER_CMD(conf,"dbcharacterset", m_dbcharacterset);
    LOAD_CENTER_CMD(conf,"dbport", m_dbport);
    snprintf(m_dbConnInfo.m_szDbHost,sizeof(m_dbConnInfo.m_szDbHost),m_dbip.c_str());
    snprintf(m_dbConnInfo.m_szDbUser,sizeof(m_dbConnInfo.m_szDbUser),m_dbuser.c_str());
    snprintf(m_dbConnInfo.m_szDbPwd,sizeof(m_dbConnInfo.m_szDbPwd),m_dbpwd.c_str());
    snprintf(m_dbConnInfo.m_szDbName,sizeof(m_dbConnInfo.m_szDbName),m_dbname.c_str());
    snprintf(m_dbConnInfo.m_szDbCharSet,sizeof(m_dbConnInfo.m_szDbCharSet),m_dbcharacterset.c_str());
    m_dbConnInfo.m_uiDbPort = m_dbport;
    m_dbConnInfo.uiTimeOut = 3;
    //节点负载配置
    LOAD_CENTER_CMD(conf,"deleteofflinenode_timeinterval", m_deleteOfflineNodeTimeInterval);//检查下线节点信息删除时间间隔
    LOAD_CENTER_CMD(conf,"nodeloadlog_timeinterval", m_nodeLoadLogTimeInterval);//节点负载日志写入时间间隔
    LOAD_CENTER_CMD(conf,"nodeloadlog_overdue", m_nodeLoadLogOverdue);//节点负载日志过时时间
    LOAD_CENTER_CMD(conf,"nodeloadstatistics_timeinterval", m_nodeLoadStatisticsTimeInterval);//节点负载统计时间间隔
    LOAD_CENTER_CMD(conf,"nodeloadstatistics_overdue", m_nodeLoadStatisticsOverdue);//节点负载统计过时时间
    LOAD_CENTER_CMD(conf,"nodeloadcheck_timeinterval", m_nodeLoadCheckTimeInterval);//节点负载检查时间(检查节点负载的日志和统计的时间间隔)
    LOAD_CENTER_CMD(conf,"serverdataloadstatuslog_overdue", m_serverDataLoadStatusLogOverdue);//服务器数据负载日志过时时间
    LOAD_CENTER_CMD(conf,"serverdataloadstatuslogcheck_timeinterval", m_serverDataLoadCheckTimeInterval);//服务器数据负载日志检查时间间隔
    //中心服务器本身配置
    LOAD_CENTER_CMD(conf,"node_recently_time", m_NodeRecentlyTime);//节点最近统计时间

    LOG4_INFO("gc_iBeatInterval:%d,NODE_BEAT:%f",net::gc_iBeatInterval,NODE_BEAT);
    m_nodeReportTimeInterval = net::gc_iBeatInterval;
    m_nodeOfflineTimeInterval = net::gc_iBeatInterval * 2 + 1;
    m_nodeStatusCheckTimeInterval = m_nodeReportTimeInterval;
    m_centerInnerPort = GetLabor()->GetPortForServer();
    m_centerInnerHost = GetLabor()->GetHostForServer();
    m_centerNodeType = GetLabor()->GetNodeType();
    m_centerProcessNum = 1;//中心服务器工作进程数只有一个
    LOG4_INFO("center InnerPort:%d InnerHost:%s NodeType:%s ProcessNum:%d",
    		m_centerInnerPort,m_centerInnerHost.c_str(),m_centerNodeType.c_str(),m_centerProcessNum);
    LOG4_INFO("nodereport_timeinterval:%d nodeOfflineTimeInterval:%d nodeStatusCheckTimeInterval:%d",
    		m_nodeReportTimeInterval,m_nodeOfflineTimeInterval,m_nodeStatusCheckTimeInterval);
    {//网关类型列表
        int iGatewaySize(0);
        util::CJsonObject gatewayTypesArray;
        if(conf.Get("gateway_list", gatewayTypesArray))
        {
            if (gatewayTypesArray.IsArray())
            {
                iGatewaySize = gatewayTypesArray.GetArraySize();
                for(int i = 0;i < iGatewaySize;++i)
                {
                    std::string gatewayType = gatewayTypesArray[i].ToString();
                    RemoveFlag(gatewayType);
                    LOG4_DEBUG("gatewayType (%s)",gatewayType.c_str());
                    m_GatewayTypeList.push_back(gatewayType);
                }
            }
        }
        LOG4_INFO("gatewayTypesArray size:%d",iGatewaySize);
    }
    {//加载中心服务器
        m_vecCenterServer.clear();
        util::CJsonObject center_servers;
        LOAD_CENTER_CMD(conf,"center_servers", center_servers);
        if(center_servers.IsEmpty())
        {
            err = "config_servers is empty";
            LOG4_ERROR("config_servers is empty");
            return false;
        }
        if (!center_servers.IsArray())
        {
            err = "center_servers is not array";
            LOG4_ERROR("center_servers is not array");
            return false;
        }
        std::string center_inner_host;
        int center_inner_port(0);
        int s = center_servers.GetArraySize();
        for(int i = 0;i < s;++i)
        {
            CenterServer centerServer;
            util::CJsonObject serverObj = center_servers[i];
            if(!serverObj.Get("center_inner_host",center_inner_host))
            {
                LOG4_WARN("failed to get center_inner_host(%s)",serverObj.ToFormattedString().c_str());
                return false;
            }
            if(!serverObj.Get("center_inner_port",center_inner_port))
            {
                LOG4_WARN("failed to get center_inner_port(%s)",serverObj.ToFormattedString().c_str());
                return false;
            }
            centerServer.center_inner_host = center_inner_host;
            centerServer.center_inner_port = center_inner_port;
            char identify[64];
            snprintf(identify,sizeof(identify),"%s:%d",center_inner_host.c_str(),center_inner_port);
            centerServer.server_identify = identify;
            LOG4_DEBUG("load center_inner_host(%s),center_inner_port(%d),server_identify(%s)",
                            center_inner_host.c_str(),center_inner_port,centerServer.server_identify.c_str());
            m_vecCenterServer.push_back(centerServer);
        }
    }
    if (m_pSyncMysqlDbi)//曾经建立连接的在初始化时重新建立连接
    {
        delete m_pSyncMysqlDbi;
        m_pSyncMysqlDbi = NULL;
    }
    //连接db用户、db密码、db库名、db字符集、db端口
    m_pSyncMysqlDbi = new util::CMysqlDbi(m_dbip.c_str(), m_dbuser.c_str(),
    		m_dbpwd.c_str(), m_dbname.c_str(),
			m_dbcharacterset.c_str(),
			m_dbport);
    if (NULL == m_pSyncMysqlDbi)
    {
        LOG4_ERROR("center cmd load DB failed");
        err = "failed to new util::CMysqlDbi";
        return false;
    }
    if(m_pSyncMysqlDbi->GetErrno())
    {
        LOG4_ERROR("mysql error(%d:%s)",m_pSyncMysqlDbi->GetErrno(),m_pSyncMysqlDbi->GetError().c_str());
        err = "CMysqlDbi connect failed";
    }
    LOG4_DEBUG("NodeSession conneted db(%s,%d),NodeActiveTimeOut(%d),NodeTimeBeat(%d),"
               "InitSessionTime(%llu),NodeTimeBeat(%d),currentTime(%llu)",
			   m_dbip.c_str(),m_dbport,m_nNodeActiveTimeOut,m_nNodeTimeBeat,m_InitSessionTime,m_nNodeTimeBeat,m_currentTime);

    snprintf(m_CenterActive.inner_ip,sizeof(m_CenterActive.inner_ip),"%s",m_centerInnerHost.c_str());
    m_CenterActive.inner_port = m_centerInnerPort;
    m_CenterActive.status = eOfflineStatus;
    CheckCenterActive();
    if(!LoadServerConfig())//加载其他类型节点配置文件
    {
        LOG4_ERROR("failed to LoadServerConfig");
    }
    boInit = true;
    return true;
}

//加载服务器节点类型信息
bool NodeSession::LoadNodeType()
{
    if (m_vecNodeTypes.size() > 0)return true;
    auto mysqlCallback = [](net::StepState* state)
	{
		STAGE_TEST_PARAM_LOG(LoadConfigSendToMysqlParam,state,"mysqlCallback");
		net::MysqlStep* pMysqlState = (net::MysqlStep*)state;
		if (pMysqlState->m_pMysqlResSet)
		{
			util::T_vecResultSet vecRes;
			if (pMysqlState->m_pMysqlResSet->GetResultSet(vecRes) > 0)
			{
				pStageParam->pNodeSession->LoadNodeType(vecRes);
				LOG4_TRACE_S(state,"LoadNodeType callback ok vecRes size:%u",vecRes.size());
			}
		}
		return true;
	};
	//读取配置文件
	net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_select,"select * from %s", NODE_TYPE_TABLE);//第一个任务
	pstep->AddStateFunc(mysqlCallback);//stage 0
	pstep->SetData(new LoadConfigSendToMysqlParam(this));
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}

bool NodeSession::LoadNodeType(util::T_vecResultSet &vecRes)
{
	m_vecNodeTypes.clear();
	NodeType nodeType;
	for (util::T_vecResultSet::iterator it = vecRes.begin(); it != vecRes.end();
					++it)
	{
		nodeType.clear();
		util::T_mapRow& valmap = *it;
		//服务器类型
		util::T_mapRow::iterator mapit = valmap.find("nodetype");
		if (valmap.end() == mapit)
		{
			continue;
		}
		nodeType.nodetype = valmap["nodetype"];
		//需要获取的服务器信息
		mapit = valmap.find("noticeservers");
		if (valmap.end() == mapit)
		{
			continue;
		}
		const std::string& noticeservers = valmap["noticeservers"];
		LOG4_DEBUG("LoadNodeType nodetype(%s),noticeservers(%s)",
						nodeType.nodetype.c_str(), noticeservers.c_str());
		if (!noticeservers.empty())
		{
			util::CJsonObject jParse;
			if (jParse.Parse(noticeservers) && jParse.IsArray())
			{
				for (int i = 0; i < jParse.GetArraySize(); ++i)
				{
					std::string nodeTypeStr = jParse[i].ToString();
					LOG4_DEBUG("LoadNodeType parse noticeservers(%s)",
									nodeTypeStr.c_str());
					std::string::iterator nodeTypeStrIt = std::remove(
									nodeTypeStr.begin(), nodeTypeStr.end(),
									'\"');
					nodeTypeStr.erase(nodeTypeStrIt, nodeTypeStr.end());
					LOG4_DEBUG(
									"LoadNodeType parse noticeservers(%s)",
									nodeTypeStr.c_str());
					nodeType.neededServers.push_back(nodeTypeStr);
				}
			}
			else
			{
				LOG4_WARN("LoadNodeType parse noticeservers(%s) failed",
								noticeservers.c_str());
			}
		}
		m_vecNodeTypes.push_back(nodeType);
	}
	LOG4_TRACE("LoadNodeType ok m_vecNodeTypes size:%u",m_vecNodeTypes.size());
	return true;
}

bool NodeSession::LoadWhiteList()
{
	if (m_vecWhiteNode.size() > 0)return true;
    auto mysqlCallback = [](net::StepState* state)
	{
		STAGE_TEST_PARAM_LOG(LoadConfigSendToMysqlParam,state,"mysqlCallback");
		net::MysqlStep* pMysqlState = (net::MysqlStep*)state;
		if (pMysqlState->m_pMysqlResSet)
		{
			util::T_vecResultSet vecRes;
			if (pMysqlState->m_pMysqlResSet->GetResultSet(vecRes) > 0)
			{
				pStageParam->pNodeSession->LoadWhiteList(vecRes);
				LOG4_TRACE_S(state,"LoadWhiteList callback ok vecRes size:%u",vecRes.size());
			}
		}
		return true;
	};
	//读取配置文件
	net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_select,"select * from %s", NODE_IPWHITE_TABLE);//第一个任务
	pstep->AddStateFunc(mysqlCallback);//stage 0
	pstep->SetData(new LoadConfigSendToMysqlParam(this));
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return true;
}

bool NodeSession::LoadWhiteList(util::T_vecResultSet &vecRes)
{
	m_vecWhiteNode.clear();
	util::T_vecResultSet::iterator it = vecRes.begin();
	util::T_vecResultSet::iterator itEnd = vecRes.end();
	for(;it != itEnd;++it)
	{
		WhiteNode whiteNode;
		if(whiteNode.loadFromMapRow(*it))
		{
			m_vecWhiteNode.push_back(whiteNode);
			LOG4_DEBUG("server whitelist inner_ip(%s)",whiteNode.inner_ip);
		}
	}
	LOG4_TRACE("LoadWhiteList ok m_vecWhiteNode size:%u",m_vecWhiteNode.size());
	return true;
}

bool NodeSession::CheckCenterActive()
{
    SetCurrentTime();
    LOG4_DEBUG("CheckCenterActive currenttime(%llu)",m_currentTime);
    auto checkCenterActiveCallback = [](net::StepState* state)
	{
		STAGE_TEST_PARAM_LOG(LoadConfigSendToMysqlParam,state,"mysqlCallback");
		net::MysqlStep* pMysqlState = (net::MysqlStep*)state;
		if (pMysqlState->m_pMysqlResSet)
		{
			util::T_vecResultSet vecRes;
			pMysqlState->m_pMysqlResSet->GetResultSet(vecRes);
			pStageParam->pNodeSession->CheckCenterActive(vecRes);
			LOG4_TRACE_S(state,"CheckCenterActive callback ok vecRes size:%u",vecRes.size());
			pStageParam->pNodeSession->SelectCenterMaster();
		}
		return true;
	};
	net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_select,"SELECT * from  %s",NODE_CENTER_ACTIVE_TABLE);//第一个任务
	pstep->AddStateFunc(checkCenterActiveCallback);//stage 0
	pstep->SetData(new LoadConfigSendToMysqlParam(this));
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return SelectCenterMaster();
}

bool NodeSession::CheckCenterActive(util::T_vecResultSet &vecRes)
{
	LOG4_TRACE("%s",__FUNCTION__);
	m_vecCenterActive.clear();
    {//加载中心活跃状态
        util::T_vecResultSet::iterator it = vecRes.begin();
        util::T_vecResultSet::iterator itEnd = vecRes.end();
        for(;it != itEnd;++it)
        {
            CenterActive centerActive;
            if(centerActive.loadFromMapRow(*it))
            {
                LOG4_DEBUG("server centerActive inner_ip(%s),inner_port(%d),status(%d),activetime(%llu)",
                                centerActive.inner_ip,centerActive.inner_port,centerActive.status,centerActive.activetime);
                m_vecCenterActive.push_back(centerActive);
            }
            else
            {
                LOG4_WARN("failed to load centerActive");
            }
        }
    }
    return true;
}

bool NodeSession::SelectCenterMaster()
{
	LOG4_TRACE("%s",__FUNCTION__);
	//检查中心活跃状态并选举
	bool boIsMaster(true);//是否是主模式
	{//选举主服务器（目前选举方式适用于两个中心节点）
		std::vector<CenterActive>::const_iterator it = m_vecCenterActive.begin();
		std::vector<CenterActive>::const_iterator itEnd = m_vecCenterActive.end();
		for(;it != itEnd;++it)
		{
			if(eMasterStatus == it->status)//只检查主节点状态
			{
				if(it->activetime + m_nNodeActiveTimeOut >= m_currentTime)
				{//有效节点
					if(0 == strncmp(m_CenterActive.inner_ip,it->inner_ip,sizeof(m_CenterActive.inner_ip))\
									&& m_CenterActive.inner_port == it->inner_port)
					{//主中心服务器就是本节点
						LOG4_DEBUG("it's master already(%s),activetime(%llu),nodebeat(%d),currentTime(%llu)",
										m_CenterActive.inner_ip,m_CenterActive.activetime,m_nNodeActiveTimeOut,m_currentTime);
						break;
					}
					else
					{
						//只要其他的有效节点为主的则本节点为从
						boIsMaster = false;
						LOG4_DEBUG("master(%s) already exist,activetime(%llu),nodebeat(%d),currentTime(%llu)",
										it->inner_ip,it->activetime,m_nNodeActiveTimeOut,m_currentTime);
					}
				}
				else
				{
					LOG4_DEBUG("node(%s)status(%u) has been timeout,activetime(%llu),nodebeat(%d),currentTime(%llu)",
										it->inner_ip,it->status,it->activetime,m_nNodeActiveTimeOut,m_currentTime);
				}
			}
		}
	}
	//切换服务器状态并上报
	if(boIsMaster)
	{//服务器更新状态
		bool boSwitch = false;
		if (eMasterStatus != m_CenterActive.status)
		{
			LOG4_DEBUG("switch to be master:inner_ip(%s),inner_port(%d)",
							m_CenterActive.inner_ip,m_CenterActive.inner_port);
			boSwitch = true;
		}
		else
		{
			LOG4_DEBUG("it's master already:inner_ip(%s),inner_port(%d)",
							m_CenterActive.inner_ip,m_CenterActive.inner_port);
		}
		return UpdateCenterStatus(eMasterStatus,boSwitch);
	}
	else
	{
		bool boSwitch = false;
		if (eSlaveStatus != m_CenterActive.status)
		{
			LOG4_DEBUG("switch to be slave:inner_ip(%s),inner_port(%d)",
							m_CenterActive.inner_ip,m_CenterActive.inner_port);
			boSwitch = true;
		}
		else
		{
			LOG4_DEBUG("it's slave already:inner_ip(%s),inner_port(%d)",
							m_CenterActive.inner_ip,m_CenterActive.inner_port);
		}
		return UpdateCenterStatus(eSlaveStatus,boSwitch);
	}
	return true;
}

bool NodeSession::UpdateCenterStatus(CenterStatus status,bool boSwitch)
{
    /*
        inner_ip varchar
        inner_port  smallint
        status  tinyint
        activetime  bigint
     * */
    ++m_nCheckActiveCounter;
    if(m_nCheckActiveCounter > 3 || boSwitch)//某节点变动时需要检查删除无效节点
    {
        m_nCheckActiveCounter = 0;
        {//设置过时节点状态
            //有效时间 activetime + m_nNodeActiveTimeOut(9s) >= m_currentTime
            uint64 validActiveTime = m_currentTime - m_nNodeActiveTimeOut;
            LOG4_DEBUG("CheckActive CenterStatus validActiveTime:%llu",validActiveTime);
			net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
			pstep->SetTask(util::eSqlTaskOper_exec,"update %s set status=%d where activetime < %llu",
                    NODE_CENTER_ACTIVE_TABLE,eOfflineStatus,validActiveTime);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
			if (!net::MysqlStep::Launch(GetLabor(),pstep))
			{
				LOG4_WARN("MysqlStep::Launch failed");
				return (false);
			}
        }
        {//删除长时间无效节点
            //保存记录时间 activetime + m_nNodeActiveTimeOut(9s) * 10 >= m_currentTime
            uint64 keepRecordTime = m_currentTime - m_nNodeActiveTimeOut * 10;
            LOG4_DEBUG("CheckActive CenterStatus keepRecordTime:%llu",keepRecordTime);
			net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
			pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s where activetime < %llu and status=%d",
                    NODE_CENTER_ACTIVE_TABLE,keepRecordTime,eOfflineStatus);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
			if (!net::MysqlStep::Launch(GetLabor(),pstep))
			{
				LOG4_WARN("MysqlStep::Launch failed");
				return (false);
			}
        }
    }
    {//更新本节点状态
        m_CenterActive.activetime = m_currentTime;
        net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
		pstep->SetTask(util::eSqlTaskOper_exec,"replace into %s values('%s',%d,%d,%d)",
                NODE_CENTER_ACTIVE_TABLE, m_CenterActive.inner_ip,
                m_CenterActive.inner_port,status,m_currentTime);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
		if (!net::MysqlStep::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("MysqlStep::Launch failed");
			return (false);
		}
        LOG4_INFO("UpdateCenterStatus status:%d Switch:%s",status,boSwitch ?"Switched":"not Switch");
        m_CenterActive.status = status;//直接先修改内存的状态
    }
    return true;
}

bool NodeSession::LoadNodeRoute()
{
    if(m_vecNodeTypes.empty())
    {
        if(!LoadNodeType())
        {
            LOG4_WARN("failed to LoadNodeType");
        }
    }
    if(m_vecWhiteNode.empty())
    {
        if(!LoadWhiteList())
        {
            LOG4_WARN("failed to LoadWhiteList");
        }
    }
    return true;
}

bool NodeSession::CheckNodeType(const std::string& nodeType)
{//检查节点类型
    NodeTypesVec::const_iterator it = m_vecNodeTypes.begin();
    NodeTypesVec::const_iterator itEnd = m_vecNodeTypes.end();
    for (;it != itEnd; ++it)
    {
        if (it->nodetype == nodeType)
        {
            return true;
        }
    }
    LOG4_ERROR("nodeinfo type(%s) invalid",nodeType.c_str());
    return false;
}

bool NodeSession::CheckWhiteNode(const std::string& nodeInnerIp)
{
    std::vector<WhiteNode>::const_iterator it = m_vecWhiteNode.begin();
    std::vector<WhiteNode>::const_iterator itEnd = m_vecWhiteNode.end();
    for(;it!=itEnd;++it)
    {
        if(it->inner_ip == nodeInnerIp)
        {
            return true;
        }
    }
    LOG4_ERROR("nodeinfo inner_ip(%s) invalid",nodeInnerIp.c_str());
    return false;
}

bool NodeSession::CheckNodeSuspend(NodeStatusInfo& nodeinfo)
{
	NodeStatusInfo *pNodeStatusInfo = GetNodeInfo(nodeinfo.getNodeKey());
	if (pNodeStatusInfo)
	{
		nodeinfo.suspend = pNodeStatusInfo->suspend;
	}
    return (true);
}

//检查节点状态
bool NodeSession::CheckNodeStatus(const NodeStatusInfo& nodeinfo)
{
    if(!LoadNodeRoute())
    {
        LOG4_ERROR("failed to LoadNodeRoute");
    }
    //检查节点类型
    if(!CheckNodeType(nodeinfo.nodeType))
    {
        LOG4_ERROR("nodeinfo type(%s) invalid",nodeinfo.nodeType.c_str());
        return false;
    }
    //检查白名单
    if(!CheckWhiteNode(nodeinfo.nodeInnerIp))
    {
        LOG4_ERROR("nodeinfo inner_ip(%s) invalid",nodeinfo.nodeInnerIp.c_str());
        return false;
    }
    return true;
}

net::uint32 NodeSession::GetNewNodeID()
{
    uint32 tmpID(0);
    for (NodesStatusMapIT it = m_mapNodesStatus.begin(); it != m_mapNodesStatus.end();
                    ++it)
    {
        if (it->second.nodeId > (int) tmpID)
        {
            tmpID = it->second.nodeId;
        }
    }
    ++tmpID;
    m_nodeId = tmpID;
    return m_nodeId;
}
void NodeSession::AddNodeInfo(const std::string& NodeKey,
                const NodeStatusInfo& Info)
{
    NodesStatusMapIT iter = m_mapNodesStatus.find(NodeKey);
    if (iter == m_mapNodesStatus.end())
    {
        m_mapNodesStatus.insert(make_pair(NodeKey, Info));
    }
}
bool NodeSession::DelNodeInfo(const std::string& NodeKey)
{
    std::map<std::string, NodeStatusInfo>::iterator iter = m_mapNodesStatus.find(
                    NodeKey);
    if (iter != m_mapNodesStatus.end())
    {
        m_mapNodesStatus.erase(iter);
        return true;
    }
    return false;
}
bool NodeSession::GetNodeInfo(const std::string &NodeKey, NodeStatusInfo &nInfo)
{
    NodeStatusInfo *pInfo = NULL;
    std::map<std::string, NodeStatusInfo>::iterator iter = m_mapNodesStatus.find(
                    NodeKey);
    if (iter != m_mapNodesStatus.end())
    {
        pInfo = &iter->second;
        nInfo = *pInfo;
        return true;
    }
    return false;
}
NodeStatusInfo *NodeSession::GetNodeInfo(const std::string &NodeKey)
{
    std::map<std::string, NodeStatusInfo>::iterator iter = m_mapNodesStatus.find(
                    NodeKey);
    if (iter != m_mapNodesStatus.end())
    {
        return &iter->second;
    }
    return NULL;
}

uint32 NodeSession::GetNodeCountByType(const std::string &nodeType)
{
    uint32 count(0);
    std::map<std::string, NodeStatusInfo>::iterator iter = m_mapNodesStatus.begin();
    std::map<std::string, NodeStatusInfo>::iterator iterEnd = m_mapNodesStatus.end();
    for(;iter != iterEnd;++iter)
    {
        if(iter->second.nodeType == nodeType)
        {
            ++count;
        }
    }
    return count;
}

//写节点到数据库，如果是报告信息需要设置boReport = true
bool NodeSession::WriteNodeDataToDB(const NodeStatusInfo& nodeInfo, bool boReport)
{
    SetCurrentTime();
    if (!WriteNodeStatus(nodeInfo))
    {
        LOG4_WARN( "WriteNodeStatus failed");
        return (false);
    }
//    if (boReport)
//    {
//        if (!CheckNodeload())
//        {
//            LOG4_WARN( "CheckNodeload failed");
//            return (false);
//        }
//        if (!WriteNodeLog(nodeInfo))
//        {
//            LOG4_WARN( "WriteNodeLog failed");
//            return (false);
//        }
//        if (!WriteNodeStatistics(nodeInfo))
//        {
//            LOG4_WARN( "WriteNodeStatistics failed");
//            return (false);
//        }
//    }
    return (true);
}
bool NodeSession::SetNodeDataOfflineToDBByNodeId(int node_id)
{
    SetCurrentTime();
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"update %s set serverstatus=%d WHERE nodeid=%d",
            NODE_LOAD_STATUS_TABLE, eNodeStatus_Offline, node_id);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}
const NodeType* NodeSession::GetNodeTypeServerInfo(const std::string &nodeType)
{
    NodeTypesVec::iterator it = m_vecNodeTypes.begin();
    NodeTypesVec::iterator itEnd = m_vecNodeTypes.end();
    for (; it != itEnd; ++it)
    {
        const NodeType& nodetype = *it;
        if (nodeType == nodetype.nodetype)
        {
            return (&nodetype);
        }
    }
    return (NULL);
}

//删除超时的下线节点状态到数据库
bool NodeSession::ClearOverdueOfflineNodeStatusToDB()
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s WHERE activetime <= %llu and serverstatus=%d",
            NODE_LOAD_STATUS_TABLE,
            (uint64)(m_currentTime - m_deleteOfflineNodeTimeInterval),
            eNodeStatus_Offline);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}
bool NodeSession::CheckOfflineNodeStatusToDB()
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"update %s set serverstatus=%d WHERE activetime <= %llu",
            NODE_LOAD_STATUS_TABLE, eNodeStatus_Offline,
            (uint64)(m_currentTime - m_nodeOfflineTimeInterval));//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}
bool NodeSession::ReplaceNodeStatusToDB(const NodeStatusInfo& nodeInfo)
{
    char szSql[3096];
    snprintf(szSql, sizeof(szSql) - 1,
                    "replace into %s values(%d,'%s',%d,'%s',%d,'%s',%d,%d,%llu,%d,%d,%d,%d,%d,%d,%d,'%s',%d)",
                    NODE_LOAD_STATUS_TABLE, nodeInfo.nodeId,
                    nodeInfo.nodeType.c_str(), nodeInfo.nodeInnerPort,
                    nodeInfo.nodeInnerIp.c_str(), nodeInfo.nodeAccessPort,
                    nodeInfo.nodeAccessIp.c_str(), eNodeStatus_Online,
                    nodeInfo.workerNum, m_currentTime, nodeInfo.load,
                    nodeInfo.connect, nodeInfo.client, nodeInfo.recvNum,
                    nodeInfo.sendNum, nodeInfo.recvByte, nodeInfo.sendByte,
                    nodeInfo.worker.c_str(),nodeInfo.suspend); //时间以服务器时间为准
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("net::MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}

bool NodeSession::ClearNodeStatusToDB(const NodeStatusInfo& nodeInfo)
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s WHERE innerport=%d and innerip='%s'",
            NODE_LOAD_STATUS_TABLE, nodeInfo.nodeInnerPort,
            nodeInfo.nodeInnerIp.c_str());//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("MysqlStep::Launch failed");
		return (false);
	}
    return (true);
}

bool NodeSession::GetNodeStatusByNodeType(const std::string & nodetype,std::vector<NodeLoadStatus>& vecNodeStatus)
{
    vecNodeStatus.clear();
    NodeLoadStatus nodeStatus;
    std::map<std::string, NodeStatusInfo>::iterator iter = m_mapNodesStatus.begin();
    for (; iter != m_mapNodesStatus.end(); ++iter)
    {
        if (nodetype == iter->second.nodeType)
        {
            nodeStatus.clear();
            nodeStatus = iter->second;
            LOG4_DEBUG("GetNodeStatusByNodeType(innerip:%s,innerport:%d,outerip:%s,outerport:%d)",
                            nodeStatus.innerip.c_str(), nodeStatus.innerport,
                            nodeStatus.outerip.c_str(), nodeStatus.outerport);
            vecNodeStatus.push_back(nodeStatus);
        }
    }
    if (vecNodeStatus.empty())
    {
        return (false);
    }
    return (true);
}

//写当前的节点状态
bool NodeSession::WriteNodeStatus(const NodeStatusInfo& nodeInfo)
{
    if (!ReplaceNodeStatusToDB(nodeInfo))
    {
        return (false);
    }
    CheckNodesStatus();
    return (true);
}
//检查节点状态
bool NodeSession::CheckNodesStatus()
{
    if (m_currentTime >= m_nodeStatusCheckLastTime + m_nodeStatusCheckTimeInterval)
    {
        m_nodeStatusCheckLastTime = m_currentTime;
        ClearOverdueOfflineNodeStatusToDB(); //删除长时间下线的节点信息
    }
    CheckOfflineNodeStatusToDB(); //检查一段时间不活跃的节点信息，设置其为下线
    return true;
}
//节点日志操作
bool NodeSession::WriteNodeLog(const NodeStatusInfo& nodeStatusInfo)
{
    //写日志时先把节点状态统计到节点日志中
    {
        //节点的key(包括节点状态和节点日志的key)都是innerIp:innerPort组成，可以直接查找
        const std::string nodeKey = nodeStatusInfo.getNodeKey();
        NodesLogMapIT logIter = m_mapNodesLog.find(
                    nodeKey);
        if (logIter != m_mapNodesLog.end())
        {
            logIter->second.AddUp(nodeStatusInfo);//日志需要累加状态
//            logIter->second.Debug(GetLogger());
        }
        else
        {
            NodeLogInfo nodeLogInfo(nodeStatusInfo);//新建节点日志
//            nodeLogInfo.Debug(GetLogger());
            m_mapNodesLog.insert(make_pair(nodeKey, nodeLogInfo));
        }
    }
    //60s写一次日志
    if (m_currentTime >= m_nodeLoadLogInsertLastTime + m_nodeLoadLogTimeInterval)
    {
        LOG4_DEBUG("InsertNodeLogToDB m_mapNodesLog size(%u),"
                        "m_currentTime(%llu),m_nodeLoadLogInsertLastTime(%llu),m_nodeLoadLogTimeInterval(%u)",
                        m_mapNodesLog.size(),m_currentTime,m_nodeLoadLogInsertLastTime,m_nodeLoadLogTimeInterval);
        m_nodeLoadLogInsertLastTime = m_currentTime;
        return InsertNodeLogToDB();
    }
    return (true);
}
//插入节点日志
bool NodeSession::InsertNodeLogToDB()
{
    /*
     * 表tb_nodeload_log
        node_id
        node_type
        inner_port
        inner_ip
        outer_port
        outer_ip
        worker_num
        active_time
        统计数据：
        server_load (1)
        max_server_load
        connect (2)
        max_connect
        client (3)
        max_client
        recv_num (4)
        max_recv_num
        send_num (5)
        max_send_num
        recv_byte (6)
        max_recv_byte
        send_byte (7)
        max_send_byte

        worker
     * */
    char szSql[3096];
    NodesLogMapIT it = m_mapNodesLog.begin();
    NodesLogMapIT itEnd = m_mapNodesLog.end();
    for (; it != itEnd; ++it)
    {
        NodeLogInfo& nodeLog = it->second;
        snprintf(szSql, sizeof(szSql) - 1,
                        "insert into %s values(%d,'%s',%d,'%s',%d,'%s',%d,%llu,"
                        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                        "'%s')",
                        NODE_LOAD_LOG_TABLE, nodeLog.nodeId,
                        nodeLog.nodeType.c_str(), nodeLog.nodeInnerPort,
                        nodeLog.nodeInnerIp.c_str(), nodeLog.nodeAccessPort,
                        nodeLog.nodeAccessIp.c_str(), nodeLog.workerNum,
                        m_currentTime,
                        nodeLog.GetAverageLoad(),nodeLog.maxLoad,nodeLog.GetAverageConnect(),nodeLog.maxConnect,
                        nodeLog.GetAverageClient(),nodeLog.maxClient, nodeLog.GetAverageRecvNum(), nodeLog.maxRecvNum,
                        nodeLog.GetAverageSendNum(),nodeLog.maxSendNum,nodeLog.GetAverageRecvByte(), nodeLog.maxRecvByte,
                        nodeLog.GetAverageSendByte(),nodeLog.maxSendByte,
                        nodeLog.worker.c_str());
        net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
		pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
		if (!net::MysqlStep::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
			return (false);
		}
        nodeLog.cleanLoad();
    }
    return (true);
}
//节点统计操作
bool NodeSession::WriteNodeStatistics(const NodeStatusInfo& nodeInfo)
{
    //统计信息
    NodesStatisticsMapIT it = m_mapNodesStatistics.find(
                    nodeInfo.getNodeKey());
    if (it == m_mapNodesStatistics.end())
    {
        NodesStaisticsInfo statisticsinfo(nodeInfo);
        m_mapNodesStatistics.insert(
                        make_pair(nodeInfo.getNodeKey(), statisticsinfo));
    }
    else
    {
        it->second += nodeInfo;
    }
    //300s写一次统计日志
    if (m_currentTime >= m_nodeLoadStatisticsInsertLastTime
                                    + m_nodeLoadStatisticsTimeInterval)
    {
        return InsertNodeStatisticsToDB();
    }
    return (true);
}
bool NodeSession::InsertNodeStatisticsToDB()
{
    m_nodeLoadStatisticsInsertLastTime = m_currentTime;
    /*
     * 表tb_nodeload_statistics
     *  节点基础状态
        nodeid nodetype innerport innerip outerport outerip workernum
        //更新状态
        currenttime currentload currentclient currentconnect
        //统计数据
        totalrecvnum totalsendnum totalrecvbyte totalsendbyte
     * */
    char szSql[256];
    NodesStatisticsMapIT it = m_mapNodesStatistics.begin();
    NodesStatisticsMapIT itEnd = m_mapNodesStatistics.end();
    for (; it != itEnd; ++it)
    {
        NodesStaisticsInfo& nodeInfo = it->second;
        snprintf(szSql, sizeof(szSql) - 1,
                        "insert into %s values(%d,'%s',%d,'%s',%d,'%s',%d,%llu,%d,%d,%d,%d,%d,%d,%d)",
                        NODE_LOAD_STATISTICS_TABLE, nodeInfo.nodeId,
                        nodeInfo.nodeType.c_str(), nodeInfo.nodeInnerPort,
                        nodeInfo.nodeInnerIp.c_str(), nodeInfo.nodeAccessPort,
                        nodeInfo.nodeAccessIp.c_str(), nodeInfo.workerNum,
                        m_currentTime, nodeInfo.load, nodeInfo.client,
                        nodeInfo.connect, nodeInfo.recvNum, nodeInfo.sendNum,
                        nodeInfo.recvByte, nodeInfo.sendByte);
        net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
		pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
		if (!net::MysqlStep::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
			return (false);
		}
        nodeInfo.clearLoad();
    }
    return (true);
}
bool NodeSession::CheckNodeload()
{
    if (m_currentTime >= m_nodeLoadCheckLastTime + m_nodeLoadCheckTimeInterval)
    {
        m_nodeLoadCheckLastTime = m_currentTime;
        ClearOverdueOfflineNodeLogToDB();
        ClearOverdueOfflineNodeStatisticsToDB();
    }
    return (true);
}
bool NodeSession::ClearOverdueOfflineNodeLogToDB()
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s WHERE active_time <= %llu",
            NODE_LOAD_LOG_TABLE,
            (uint64)(m_currentTime - m_nodeLoadLogOverdue));//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
		return (false);
	}
    LOG4_DEBUG( "ClearOverdueOfflineNodeLogToDB ok");
    return (true);
}
bool NodeSession::ClearOverdueOfflineNodeStatisticsToDB()
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s WHERE currenttime <= %llu",
            NODE_LOAD_STATISTICS_TABLE,
            (uint64)(m_currentTime - m_nodeLoadStatisticsOverdue));//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("%s net::MysqlStep::Launch failed",__FUNCTION__);
		return (false);
	}
    LOG4_DEBUG( "ClearOverdueOfflineNodeStatisticsToDB ok");
    return (true);
}

bool NodeSession::WriteServerDataToDB(const char* nodetype, int innerport,
                const char* innerip, int outerport, const char* outerip,
                const char* status)
{
    SetCurrentTime();
//    ServerDataLoadCheck();
    if(!ReplaceServerDataLoadStatusToDB(nodetype, innerport, innerip, outerport,
            outerip, status))
    {
    	LOG4_WARN("failed to ReplaceServerDataLoadStatusToDB nodetype:%s",nodetype);
    	return false;
    }
//    if(!WriteServerDataLoadLogToDB(nodetype, innerport, innerip, outerport, outerip,
//            status))
//    {
//    	LOG4_WARN("failed to WriteServerDataLoadLogToDB nodetype:%s",nodetype);
//    	return false;
//    }
    LOG4_DEBUG("succeeded to WriteServerDataToDB nodetype:%s",nodetype);
    return (true);
}

bool NodeSession::ServerDataLoadCheck()
{
    if (m_currentTime >= m_serverDataLoadCheckLastTime + m_serverDataLoadCheckTimeInterval)
    {
        m_serverDataLoadCheckLastTime = m_currentTime;
        ClearOverdueServerDataLogToDB();
    }
    return (true);
}
bool NodeSession::ReplaceServerDataLoadStatusToDB(const char* nodetype,
                int innerport, const char* innerip, int outerport,
                const char* outerip, const char* status)
{
    char szSql[4096];
    snprintf(szSql, sizeof(szSql) - 1,
                    "replace into %s values('%s',%d,'%s',%d,'%s','%s','%s')",
                    NODE_SERVER_DATA_STATUS_TABLE, nodetype, innerport, innerip,
                    outerport, outerip, status,util::time_t2TimeStr(m_currentTime).c_str());
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("%s net::MysqlStep::Launch failed",__FUNCTION__);
		return (false);
	}
    return (true);
}
bool NodeSession::WriteServerDataLoadLogToDB(const char* nodetype,
                int innerport, const char* innerip, int outerport,
                const char* outerip, const char* status)
{
    char szSql[4096];
    snprintf(szSql, sizeof(szSql) - 1,
                    "insert into %s values('%s',%d,'%s',%d,'%s','%s','%s')",
                    NODE_SERVER_DATA_LOG_TABLE, nodetype, innerport, innerip,
                    outerport, outerip, status,util::time_t2TimeStr(m_currentTime).c_str());
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("%s net::MysqlStep::Launch failed",__FUNCTION__);
		return (false);
	}
    return (true);
}

bool NodeSession::ClearOverdueServerDataLogToDB()
{
    net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
	pstep->SetTask(util::eSqlTaskOper_exec,"delete from %s WHERE time <= %llu",NODE_SERVER_DATA_LOG_TABLE,
						(uint64)(m_currentTime - m_serverDataLoadStatusLogOverdue));//第一个任务(无需回调处理函数,也就无需设置自定义参数)
	if (!net::MysqlStep::Launch(GetLabor(),pstep))
	{
		LOG4_WARN("net::MysqlStep::Launch failed");
		return (false);
	}
    LOG4_DEBUG( "ClearOverdueOfflineNodeLogToDB ok");
    return (true);
}

int NodeSession::RegNode(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, const NodeStatusInfo& nodeinfo)
{
    NodeStatusInfo tmpRegNodeStatus = nodeinfo;
    CheckNodeSuspend(tmpRegNodeStatus);
    if(eMasterStatus == m_CenterActive.status)
    {
        LOG4_DEBUG("Master(%s,%d) RegNodeRoute(%s)",m_CenterActive.inner_ip,m_CenterActive.inner_port,
                        tmpRegNodeStatus.getNodeKey().c_str());
        return RegNodeRoute(stMsgShell,oInMsgHead,oInMsgBody,tmpRegNodeStatus);
    }
    else if (eSlaveStatus == m_CenterActive.status)
    {//从节点更新内存，不更新db
        NodeStatusInfo *pNodeInfo = GetNodeInfo(tmpRegNodeStatus.getNodeKey());
        if (pNodeInfo) //注册过的只需要更新节点信息
        {
            if (!pNodeInfo->update(tmpRegNodeStatus))
            {
                LOG4_WARN("pNodeInfo(%d) update failed", tmpRegNodeStatus.nodeId);
            }
        }
        else
        {
            //加入到节点管理MAP
            LOG4_DEBUG("(%s):before AddNodeInfo(%s) size(%u)",
                            __FUNCTION__,tmpRegNodeStatus.getNodeKey().c_str(),
                            GetMapNodeInfoSize());
            AddNodeInfo(tmpRegNodeStatus.getNodeKey(), tmpRegNodeStatus);
            LOG4_DEBUG("(%s):after AddNodeInfo(%s) size(%u)",
                            __FUNCTION__,tmpRegNodeStatus.getNodeKey().c_str(),
                            GetMapNodeInfoSize());
        }
        //从中心节点只会发布自己的路由
        return SendCenterToReg(stMsgShell,oInMsgHead,oInMsgBody,tmpRegNodeStatus);
    }
    else
    {
        LOG4_WARN("m_CenterStatus(%d),wait for center",m_CenterActive.status);
        return ERR_OK;//不返回错误消息
    }
}

int NodeSession::UpdateNode(const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,
					const NodeStatusInfo& nodeinfo)
{
    NodeStatusInfo *pNodeInfo = GetNodeInfo(nodeinfo.getNodeKey());
    if (!pNodeInfo) //没有该节点则注册
    {
        int iRet = RegNode(stMsgShell, oInMsgHead,oInMsgBody, nodeinfo);
        if (iRet)
        {
            LOG4_ERROR("CmdNodeReport RegNode msg jsonbuf[%s] is wrong,error code(%d)",
                            oInMsgBody.body().c_str(), iRet);
            return iRet;
        }
        return ERR_OK;
    }
    else //更新上报状态
    {
        if (!pNodeInfo->update(nodeinfo))
        {
            LOG4_WARN("pNodeInfo(%s,%d,%s,%d) update failed",
                            pNodeInfo->nodeType.c_str(),
                            pNodeInfo->nodeId,pNodeInfo->nodeInnerIp.c_str(),pNodeInfo->nodeInnerPort);
            return ERR_SERVERINFO;
        }
        if(eMasterStatus == m_CenterActive.status)
        {//只有主节点更新db的节点状态
            LOG4_DEBUG("Master(%s,%d) WriteNodeDataToDB(%s)",m_CenterActive.inner_ip,m_CenterActive.inner_port,
                            pNodeInfo->getNodeKey().c_str());
            if (!WriteNodeDataToDB(*pNodeInfo, true))
            {
                LOG4_WARN("WriteNodeDataToDB nodeid(%d) false",pNodeInfo->nodeId);
                return ERR_SERVERINFO_RECORD;
            }
        }
        return ERR_OK;
    }
}

bool NodeSession::DelNode(const std::string& delNodeIdentify)
{
	NodeStatusInfo delNodeInfo;//节点信息
	//获取对应的节点信息.delNodeIdentify(IP:端口)
	if (!GetNodeInfo(delNodeIdentify, delNodeInfo))
	{
		LOG4_WARN("No such node.del node identity(%s)!",
						delNodeIdentify.c_str());
		return false;
	}
	LOG4_DEBUG("(%s):before DelNodeInfo size(%u),disconnect server type(%s)",
					__FUNCTION__, GetMapNodeInfoSize(),delNodeInfo.nodeType.c_str());
	//从map中删除节点信息
	if(!DelNodeInfo(delNodeIdentify))
	{
		LOG4_WARN("failed to del delNodeInfo.nodeId(%d)!",
				delNodeInfo.nodeId);
		return false;
	}
	LOG4_DEBUG("(%s):after DelNodeInfo size(%u),disconnect server type(%s)",
						__FUNCTION__, GetMapNodeInfoSize(),delNodeInfo.nodeType.c_str());
	if(eMasterStatus == m_CenterActive.status)
	{//主节点才写数据库和下发消息
		UnregNodeRoute(delNodeInfo);
	}
	return true;
}

int NodeSession::GetLoadMinNode(const std::string& serverType,NodeLoadStatus &nodeLoadStatus)
{//主从节点都允许分配服务器节点
    if(!CheckNodeType(serverType))
    {
        LOG4_WARN("no such nodetype(%s)",serverType.c_str());
        return ERR_SERVERINFO;
    }
    //获取指定类型的节点状态列表
    std::vector<NodeLoadStatus> vecNodeStatus;
    if(!GetNodeStatusByNodeType(serverType,vecNodeStatus))
    {
        LOG4_ERROR("get server node failed(%s)",serverType.c_str());
        return ERR_SERVERINFO;
    }
    if(vecNodeStatus.empty())
    {
        LOG4_ERROR("serverType vecNodeStatus empty!");
        return ERR_SERVERINFO;
    }
    //获取节点状态列表中负载最小的节点
    {
        std::vector<NodeLoadStatus>::iterator itNode = vecNodeStatus.begin();
        std::vector<NodeLoadStatus>::iterator itEndNode = vecNodeStatus.end();
        nodeLoadStatus = *itNode;
        ++itNode;
        for(;itNode != itEndNode;++itNode)
        {
            if(nodeLoadStatus.serverload > itNode->serverload)//获取负载最小的节点
            {
                nodeLoadStatus = *itNode;
            }
        }
    }
    return ERR_OK;
}

int NodeSession::UpdateServerConfig(const server::update_server_config_req &oUpdateServerConfigReq,
    server::update_server_config_ack &oUpdateServerConfigAck)
{
    //更新配置操作根据哈希服务器列表发送，不用检查中心模式
    const ::server::node_config& oUpdateNodeConfig = oUpdateServerConfigReq.config();
    std::string strConfigFile;//配置文件名
    util::CJsonObject checkConfigContentObj;//检查配置内容
    NodeConfigFile oNodeConfigFile; //被更新节点的当前配置
    std::string sUpdateNodeIdentify;//更新节点标识符
	{//检查配置格式
        /*
                         更新服务器配置请求
        message update_server_config_req
        {
          node_config config =1;//服务器配置
          string inner_ip = 2;//指定修改节点ip（可选）
          uint32 inner_port = 3;//指定修改节点端口（可选）
        }
         * */
	    {//检查发送指定节点
            if(oUpdateServerConfigReq.inner_ip().size() > 0 && oUpdateServerConfigReq.inner_port() > 0)
            {
                char strNodeIdentify[32];
                snprintf(strNodeIdentify,sizeof(strNodeIdentify),"%s:%u",
                                oUpdateServerConfigReq.inner_ip().c_str(),oUpdateServerConfigReq.inner_port());
                sUpdateNodeIdentify = strNodeIdentify;
            }
	    }
	    if(oUpdateNodeConfig.node_type() != "CENTER")
        {//非中心节点配置操作由主中心节点处理
            if(!IsMaster())
            {
                LOG4_DEBUG("is not master,opertate node_type(%s)",
                                oUpdateNodeConfig.node_type().c_str());
                return ERR_SERVER_NODE_NOT_MASTER;
            }
            if(sUpdateNodeIdentify.size() > 0)
            {
                NodeStatusInfo *pNodeStatusInfo = GetNodeInfo(sUpdateNodeIdentify);
                if(!pNodeStatusInfo)
                {
                    LOG4_WARN("no such a online(%s)",sUpdateNodeIdentify.c_str());
                    return ERR_SERVER_NO_SUCH_ONLINE_NODE;
                }
                if(oUpdateNodeConfig.node_type() != pNodeStatusInfo->nodeType)
                {
                    LOG4_WARN("no such a online(%s) for %s",
                                    sUpdateNodeIdentify.c_str(),oUpdateNodeConfig.node_type().c_str());
                    return ERR_SERVER_NO_SUCH_ONLINE_NODE;
                }
            }
        }
	    else
	    {//中心节点操作由各自中心节点处理
	        if(sUpdateNodeIdentify.size() == 0)
	        {//中心节点操作需要指定节点
	            LOG4_DEBUG("opertate node_type(%s),must name target node",oUpdateNodeConfig.node_type().c_str());
                return ERR_SERVER_CENTER_OPERATION_NO_TARGET;
	        }
	        else
	        {//指定中心节点操作由指定中心节点处理
	            if(!IsCenterServer(sUpdateNodeIdentify))
	            {
	                LOG4_DEBUG("opertate node_type(%s),must name valid target node(%s)",oUpdateNodeConfig.node_type().c_str(),sUpdateNodeIdentify.c_str());
                    return ERR_SERVER_CENTER_OPERATION_NO_TARGET;
	            }
	            if (!IsSelfNodeIdentify(sUpdateNodeIdentify))
                {
                    LOG4_DEBUG("is not self,opertate node_type(%s)",oUpdateNodeConfig.node_type().c_str());
                    return ERR_SERVER_CENTER_NO_OPERATION;
                }
	        }
	    }
	    /*
	     message node_config
        {
            //查询和更新发送
            string node_type = 1;//节点类型 ,”LOGIC”
            uint32 config_type = 2;//配置类型，0:服务器配置，其他类型为逻辑配置
            string config_content = 3;//配置内容（目前更新内容的字段名为"so"、"module"、"log_level",可更新其中之一，或者一起更新）
            uint32 auto_send = 4;//是否自动下发0：不是，1：是
            uint32 reload_config = 5;//是否在线已加载配置，0：不是，1：是
            //查询时Server发送
            string config_file  = 6;//配置文件名，如LogicServer.json
            uint32 update_time = 7;//更新时间
        }
	     * */
	    if (oUpdateNodeConfig.config_content().size() == 0)
	    {
	        LOG4_ERROR("config_type is null");
            return ERR_REQ_MISS_PARAM;
	    }
	    if (oUpdateNodeConfig.auto_send() != eAutoSendConfig_no
                && oUpdateNodeConfig.auto_send() != eAutoSendConfig_yes)
        {
            LOG4_ERROR("config_type is null");
            return ERR_REQ_MISS_PARAM;
        }
        if(!GetServerConfigFile(oUpdateNodeConfig.node_type(),
                        oUpdateNodeConfig.config_type(),oNodeConfigFile))
        {
            LOG4_ERROR("%s() failed to get nodeConfigFile,node_type(%s),config_type(%d)",
                            __FUNCTION__,oUpdateNodeConfig.node_type().c_str(),
                            oUpdateNodeConfig.config_type());
            return ERR_REQ_MISS_PARAM;
        }
        if (!checkConfigContentObj.Parse(oUpdateServerConfigReq.config().config_content()))
        {
            LOG4_ERROR("%s() config_content(%s) invalid",__FUNCTION__,oUpdateServerConfigReq.config().config_content().c_str());
            return ERR_REQ_MISS_PARAM;
        }
        int nRet = CheckServerConfigContent(oNodeConfigFile,checkConfigContentObj,
                        oUpdateNodeConfig.config_type(),strConfigFile);
        if(nRet)
        {
            LOG4_ERROR("%s() failed to CheckServerConfigContent,config_type(%d),config_file(%s)",
                            __FUNCTION__,oUpdateNodeConfig.config_type(),oUpdateNodeConfig.config_file().c_str());
            return nRet;
        }
        if(strConfigFile.empty())
        {
            LOG4_ERROR("%s() config_file is empty",__FUNCTION__);
            return ERR_REQ_MISS_PARAM;
        }
	}

	{//检查已更新配置
        int nRet = CheckServerConfigFromDB(oUpdateNodeConfig.node_type(),
                        oUpdateNodeConfig.config_type(),checkConfigContentObj.ToString(),strConfigFile,oUpdateNodeConfig.auto_send());
        if(ERR_SERVER_CONFIG_EXIST == nRet)//要更新的配置目前已存在同样的
        {
            if(oUpdateNodeConfig.reload_config() == 1)
            {//需要重新加载逻辑配置
                LOG4_DEBUG("try to reload");
                if (oNodeConfigFile.cmds.empty() && oNodeConfigFile.url_paths.empty())
                {
                    LOG4_WARN("none so to reload for %s",oNodeConfigFile.file_name.c_str());
                    return ERR_SERVER_LOGIC_CONFIG_NONE_RELOAD;
                }
                if (!ReloadServerConfigToType(oUpdateNodeConfig.node_type(),oNodeConfigFile))
                {
                    LOG4_ERROR("failed to SendServerConfigToType %s",oUpdateNodeConfig.node_type().c_str());
                    return ERR_DATA_TRANSFER;
                }
                //生效已更新配置
                return UpdateServerConfigToDB(oUpdateNodeConfig.node_type(),oUpdateNodeConfig.config_type(),
                                checkConfigContentObj.ToString(),strConfigFile,oUpdateNodeConfig.auto_send(),
                                oUpdateNodeConfig.reload_config());
            }
            else
            {
                LOG4_WARN("%s() same config already updated:%s",__FUNCTION__,oUpdateNodeConfig.config_content().c_str());
                return nRet;
            }
        }
        else if (ERR_OK == nRet)
        {//内容与最近更新不一样
            if(oUpdateNodeConfig.reload_config() == 1)
            {
                LOG4_DEBUG("reload content is not the same:%s",checkConfigContentObj.ToString().c_str());
                return ERR_SERVER_LOGIC_CONFIG_RELOAD_FAIL;
            }
        }
        if (nRet)
        {
            LOG4_WARN("failed to CheckServerConfigFromDB");
            return nRet;
        }
    }
	{//更新新的配置
        int nRet = UpdateServerConfigToDB(oUpdateNodeConfig.node_type(),
                        oUpdateNodeConfig.config_type(),checkConfigContentObj.ToString(),strConfigFile,
                        oUpdateNodeConfig.auto_send(),oUpdateNodeConfig.reload_config());
        if(nRet)
        {
            LOG4_ERROR("failed to UpdateServerConfigToDB");
            return nRet;
        }
        //更新服务器配置
        if (!SendServerConfigToType(oUpdateNodeConfig.node_type(),oUpdateNodeConfig.config_type(),
                        checkConfigContentObj,strConfigFile,sUpdateNodeIdentify))
        {
            LOG4_ERROR("failed to SendServerConfigToType %s",oUpdateNodeConfig.node_type().c_str());
            return ERR_DATA_TRANSFER;
        }
	}
	return ERR_OK;
}

int NodeSession::CheckServerConfigContent(const NodeConfigFile &nodeConfigFile,const util::CJsonObject &checkConfigContentObj,
                uint32 config_type,std::string &config_file)
{
    config_file = nodeConfigFile.file_name;
    if(config_file.empty())
    {
        LOG4_ERROR("failed to get config_file");
        return ERR_REQ_MISS_PARAM;
    }
    //检查服务器的修改配置
    if(0 == config_type)
    {//检查objConfigContent
        bool boCheckConfigContent(false);
        {//检查so
		//"so":  [{
		// "cmd":  1001,"so_path":  "plugins/Logic/CmdLogin.so","entrance_symbol":  "create","load": false,"version":  1
		//}]
            util::CJsonObject jParseSoObj;
            if(checkConfigContentObj.Get("so",jParseSoObj))
            {
                boCheckConfigContent = true;
                LOG4_DEBUG("so:(%s)",jParseSoObj.ToString().c_str());
                if(!jParseSoObj.IsArray())
                {
                    LOG4_ERROR("jParseSoObj is not array");
                    return ERR_REQ_MISS_PARAM;
                }
                int cmd;
                std::string so_path;
                std::string entrance_symbol;
                bool load;
                int version;
                for(int i = jParseSoObj.GetArraySize() - 1;i > -1;--i)
                {
                    const util::CJsonObject& soObj = jParseSoObj[i];
                    if(!soObj.Get("cmd",cmd))
                    {
                        LOG4_ERROR("soObj don't has cmd");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!soObj.Get("so_path",so_path))
                    {
                        LOG4_ERROR("soObj don't has so_path");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!soObj.Get("entrance_symbol",entrance_symbol))
                    {
                        LOG4_ERROR("soObj don't has entrance_symbol");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!soObj.Get("load",load))
                    {
                        LOG4_ERROR("soObj don't has load");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!soObj.Get("version",version))
                    {
                        LOG4_ERROR("soObj don't has version");
                        return ERR_REQ_MISS_PARAM;
                    }
                }
            }
        }
        {//检查module
//       "module":[
//        {"url_path":"/im/test_worker_block","so_path":"plugins/Interface/ModuleHello.so","entrance_symbol":"create", "load":true, "version":1}]
            util::CJsonObject jParseModuleObj;
            if(checkConfigContentObj.Get("module",jParseModuleObj))
            {
                boCheckConfigContent = true;
                LOG4_DEBUG("module:(%s)",jParseModuleObj.ToString().c_str());
                if(!jParseModuleObj.IsArray())
                {
                    LOG4_ERROR("jParseModuleObj is not array");
                    return ERR_REQ_MISS_PARAM;
                }
                std::string url_path;
                std::string so_path;
                std::string entrance_symbol;
                bool load;
                int version;
                for(int i = jParseModuleObj.GetArraySize() - 1;i > -1;--i)
                {
                    const util::CJsonObject& moduleObj = jParseModuleObj[i];
                    if(!moduleObj.Get("url_path",url_path))
                    {
                        LOG4_ERROR("moduleObj don't has url_path");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!moduleObj.Get("so_path",so_path))
                    {
                        LOG4_ERROR("moduleObj don't has so_path");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!moduleObj.Get("entrance_symbol",entrance_symbol))
                    {
                        LOG4_ERROR("moduleObj don't has entrance_symbol");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!moduleObj.Get("load",load))
                    {
                        LOG4_ERROR("moduleObj don't has load");
                        return ERR_REQ_MISS_PARAM;
                    }
                    if(!moduleObj.Get("version",version))
                    {
                        LOG4_ERROR("moduleObj don't has version");
                        return ERR_REQ_MISS_PARAM;
                    }
                }
            }
        }
        {//检查日志等级
            int log_level(0);
            if(checkConfigContentObj.Get("log_level",log_level))
            {
                boCheckConfigContent = true;
                LOG4_DEBUG("log_level:(%d)",log_level);
                if(log_level != log4cplus::TRACE_LOG_LEVEL && log_level != log4cplus::DEBUG_LOG_LEVEL && \
                    log_level != log4cplus::INFO_LOG_LEVEL && log_level != log4cplus::WARN_LOG_LEVEL && \
                    log_level != log4cplus::ERROR_LOG_LEVEL && log_level != log4cplus::FATAL_LOG_LEVEL && \
                    log_level != log4cplus::OFF_LOG_LEVEL)
                {
                    LOG4_ERROR("log_level:(%d) error",log_level);
                    return core::ERR_INVALID_DATA;
                }
            }
        }
        if(!boCheckConfigContent)
        {
            LOG4_WARN("checkConfigContentObj is not valid",checkConfigContentObj.ToFormattedString().c_str());
            return ERR_REQ_MISS_PARAM;
        }
        //检查必要字段
        bool boCheck = nodeConfigFile.check ? true :false;
        if(boCheck)
        {
            for(int i = nodeConfigFile.nessesary_fields.size() - 1; i > -1;--i)
            {
                std::string key = nodeConfigFile.nessesary_fields[i];
                if(checkConfigContentObj(key).empty())
                {
                    LOG4_DEBUG("checkConfigContentObj don't has atom field(%s)",key.c_str());
                    util::CJsonObject obj;
                    if(!checkConfigContentObj.Get(key,obj))
                    {
                        LOG4_WARN("checkConfigContentObj field(%s) don't exist,checkConfigContentObj:%s",
                                    key.c_str(),checkConfigContentObj.ToFormattedString().c_str());
                        return ERR_REQ_MISS_PARAM;
                    }
                }
            }
        }
    }
    else//其他类型配置检查
    {
        //检查必要字段
        bool boCheck = nodeConfigFile.check ? true :false;
        if(boCheck)
        {
            for(int i = nodeConfigFile.nessesary_fields.size() - 1; i > -1;--i)
            {
                std::string key = nodeConfigFile.nessesary_fields[i];
                if(checkConfigContentObj(key).empty())
                {
                    LOG4_DEBUG("checkConfigContentObj don't has atom field(%s)",key.c_str());
                    util::CJsonObject obj;
                    if(!checkConfigContentObj.Get(key,obj))
                    {
                        LOG4_WARN("checkConfigContentObj field(%s) don't exist,checkConfigContentObj:%s",
                                    key.c_str(),checkConfigContentObj.ToFormattedString().c_str());
                        return ERR_REQ_MISS_PARAM;
                    }
                }
            }
        }
    }
    return ERR_OK;
}

int NodeSession::CheckMgrMsg(const MsgBody& oInMsgBody,server::user_basic &basicInfo)
{
	if(oInMsgBody.session_id() == 0)//session_id   appid << 32 | userid
	{
		LOG4_ERROR("%s() session_id not exist",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	if(oInMsgBody.additional().size() == 0)
	{
		LOG4_ERROR("%s() additional not exist",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	if(!basicInfo.ParseFromString(oInMsgBody.additional()))
	{
		LOG4_ERROR("%s() Parse additional failed",__FUNCTION__);
		return(ERR_INVALID_PROTOCOL);
	}
	if(basicInfo.user_type() != eUserType_supermanager)
	{
		LOG4_ERROR("%s() no operate right",__FUNCTION__);
		return(ERR_NO_OPERATION_PERMISSIONS);
	}
	return ERR_OK;
}

int NodeSession::OfflineNode(const std::string& sOfflineNodeIdentify)
{
    if (sOfflineNodeIdentify == GetSelfNodeIdentify())
    {//下线的是自己
        LOG4_WARN("try to OfflineNode self");
        return ERR_SERVER_SELF_OFFLINE;
    }
	//获取对应的节点信息.delNodeIdentify(IP:端口)
	NodeStatusInfo *pOfflineNodeInfo = GetNodeInfo(sOfflineNodeIdentify);//下线节点信息
	if (!pOfflineNodeInfo)
	{
		LOG4_WARN("OfflineNode No such sOfflineNodeIdentify(%s)!",
		                sOfflineNodeIdentify.c_str());
		return ERR_SERVER_NODE_NO_EXIST;
	}
	if(eNodeStatusInfoSuspend == pOfflineNodeInfo->suspend)
	{
	    LOG4_WARN("OfflineNode already sOfflineNodeIdentify(%s)!",
                        sOfflineNodeIdentify.c_str());
        return ERR_SERVER_NODE_ALREADY_OFFLINE;
	}
	uint32 nodeCount = GetNodeCountByType(pOfflineNodeInfo->nodeType);
    if(nodeCount <2)
    {
        LOG4_WARN("nodeCount(%u),need no less then 2 nodes");
        return ERR_SERVER_NODE_OFFLINE_NEED_MORE_NODES;
    }
	pOfflineNodeInfo->suspend = eNodeStatusInfoSuspend;//挂起的节点依然处理服务器内部消息，但是不处理新的业务请求
	if (!WriteNodeDataToDB(*pOfflineNodeInfo))//记录到数据库
    {
        LOG4_WARN("WriteNodeDataToDB false,error:(%d,%s)",
        		GetSyncLastMysqlErrno(),GetSyncLastMysqlError().c_str());
        return ERR_SERVERINFO_RECORD;
    }
	//下线者给其它服务发通知
    int nRet = SendUnregToOthers(*pOfflineNodeInfo);
    if(nRet)
    {
        LOG4_WARN("failed to SendUnregToOthers:%s",pOfflineNodeInfo->nodeType.c_str());
        return nRet;
    }
    LOG4_INFO("%s() OfflineNode sOfflineNodeIdentify(%s) ok",__FUNCTION__,sOfflineNodeIdentify.c_str());
    return core::ERR_OK;
}

int NodeSession::OnlineNode(const std::string& sOnlineNodeIdentify)
{
    if (sOnlineNodeIdentify == GetSelfNodeIdentify())
    {//上线的是自己
        return ERR_SERVER_CENTER_NO_ROUTES_RESTORE;
    }
    //获取对应的节点信息.sOnlineNodeIdentify(IP:端口)
    NodeStatusInfo *pOnlineNodeInfo = GetNodeInfo(sOnlineNodeIdentify);//上线节点信息
    if (!pOnlineNodeInfo)
    {
        LOG4_WARN("OnlineNode No such sOnlineNodeIdentify(%s)!",
                        sOnlineNodeIdentify.c_str());
        return ERR_SERVER_NODE_NO_EXIST;
    }
    if(eNodeStatusInfoNormal == pOnlineNodeInfo->suspend)
    {
        LOG4_WARN("OnlineNode already online(%s)!",
                        sOnlineNodeIdentify.c_str());
        return ERR_SERVER_NODE_ALREADY_ONLINE;
    }
    pOnlineNodeInfo->suspend = eNodeStatusInfoNormal;
    if (!WriteNodeDataToDB(*pOnlineNodeInfo))//记录到数据库
    {
        LOG4_WARN("WriteNodeDataToDB false");
        return ERR_SERVERINFO_RECORD;
    }
    //上线者给其它服务发通知
    int nRet = SendRegToOthers(*pOnlineNodeInfo);
    if(nRet)
    {
        LOG4_WARN("failed to SendRegToOthers:%s",pOnlineNodeInfo->nodeType.c_str());
        return nRet;
    }
    LOG4_INFO("%s() OnlineNode sOnlineNodeIdentify(%s) ok",__FUNCTION__,sOnlineNodeIdentify.c_str());
    return ERR_OK;
}

int NodeSession::CanOnlineNode(const std::string& sOnlineNodeIdentify)
{
    if (sOnlineNodeIdentify == GetSelfNodeIdentify())
    {//上线的是自己
        return ERR_SERVER_SELF_ONLINE;
    }
    //获取对应的节点信息.sOnlineNodeIdentify(IP:端口)
    NodeStatusInfo *pOnlineNodeInfo = GetNodeInfo(sOnlineNodeIdentify);//上线节点信息
    if (!pOnlineNodeInfo)
    {
        LOG4_WARN("OnlineNode No such sOnlineNodeIdentify(%s)!",
                        sOnlineNodeIdentify.c_str());
        return ERR_SERVER_NODE_NO_EXIST;
    }
    if(eNodeStatusInfoNormal == pOnlineNodeInfo->suspend)
    {
        LOG4_WARN("OnlineNode already online(%s)!",
                        sOnlineNodeIdentify.c_str());
        return ERR_SERVER_NODE_ALREADY_ONLINE;
    }
    return ERR_OK;
}

int NodeSession::CanReloadConfigNode(const std::string& sOnlineNodeIdentify,std::string &nodeType)
{
    if (sOnlineNodeIdentify == GetSelfNodeIdentify())
    {//上线的是自己
        return ERR_SERVER_SELF_ONLINE;
    }
    //获取对应的节点信息.sOnlineNodeIdentify(IP:端口)
    NodeStatusInfo *pOnlineNodeInfo = GetNodeInfo(sOnlineNodeIdentify);//上线节点信息
    if (!pOnlineNodeInfo)
    {
        LOG4_WARN("OnlineNode No such sOnlineNodeIdentify(%s)!",
                        sOnlineNodeIdentify.c_str());
        return ERR_SERVER_NODE_NO_EXIST;
    }
    nodeType = pOnlineNodeInfo->nodeType;
    return ERR_OK;
}

int NodeSession::RegNodeRoute(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo& nodeinfo)
{
    NodeStatusInfo *pNodeInfo = GetNodeInfo(nodeinfo.getNodeKey());
    if (pNodeInfo) //注册过的只需要更新节点信息
    {
        if (!pNodeInfo->update(nodeinfo))
        {
            LOG4_WARN("pNodeInfo(%d) update failed", nodeinfo.nodeId);
        }
        LOG4_DEBUG("Master(%s,%d) WriteNodeDataToDB(%s)",m_CenterActive.inner_ip,m_CenterActive.inner_port,
                                                nodeinfo.getNodeKey().c_str());
        if (!WriteNodeDataToDB(nodeinfo,true))//服务器上报时，记录到数据库
        {
            LOG4_WARN("WriteNodeDataToDB false(%s)",
            		GetSyncLastMysqlError().c_str());
            return ERR_SERVERINFO_RECORD;
        }
        //返回注册响应
        ResponseNodeReg(stMsgShell, oInMsgHead, oInMsgBody, ERR_OK,*pNodeInfo);
        //发送注册服务器的配置信息(注册成功后才调用)
        SendServerConfig(stMsgShell, oInMsgHead, oInMsgBody,*pNodeInfo);
        //给注册者发其他服务器通知
        SendOthersToReg(stMsgShell, *pNodeInfo);
        //注册者给其它服务发通知
        SendRegToOthers(*pNodeInfo);
        return (ERR_OK);
    }
    else //没有注册过的需要检查,然后分配节点并注册
    {
        NodeStatusInfo tmpRegNodeStatus = nodeinfo;
        tmpRegNodeStatus.nodeId = GetNewNodeID(); //需用节点id分配器来分配节点ID
        LOG4_DEBUG("Master(%s,%d) WriteNodeDataToDB(%s),new nodeId(%d)",m_CenterActive.inner_ip,m_CenterActive.inner_port,
                                                nodeinfo.getNodeKey().c_str(),tmpRegNodeStatus.nodeId);
        if (!WriteNodeDataToDB(tmpRegNodeStatus))//记录到数据库
        {
            LOG4_WARN("WriteNodeDataToDB false(%s)",
            		GetSyncLastMysqlError().c_str());
            return ERR_SERVERINFO_RECORD;
        }
        LOG4_DEBUG("(%s):before AddNodeInfo size(%u)",
                        __FUNCTION__, GetMapNodeInfoSize());
        //加入到节点管理MAP
        AddNodeInfo(tmpRegNodeStatus.getNodeKey(), tmpRegNodeStatus);
        LOG4_DEBUG("(%s):after AddNodeInfo(%s) size(%u)",
                        __FUNCTION__,tmpRegNodeStatus.getNodeKey().c_str(),
                        GetMapNodeInfoSize());
        //返回注册响应
        ResponseNodeReg(stMsgShell, oInMsgHead, oInMsgBody, ERR_OK,tmpRegNodeStatus);
        //发送注册服务器的配置信息(注册成功后才调用)
        SendServerConfig(stMsgShell, oInMsgHead, oInMsgBody,tmpRegNodeStatus);
        //给注册者发其他服务器通知
        SendOthersToReg(stMsgShell,tmpRegNodeStatus);
        //注册者给其它服务发通知
        SendRegToOthers(tmpRegNodeStatus);
        return (ERR_OK);
    }
}

bool NodeSession::ResponseNodeReg(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, int iRet,const NodeStatusInfo &regNodeStatus)
{
	LOG4_DEBUG("");
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(net::CMD_RSP_NODE_REGISTER);//注册节点应答
    oOutMsgHead.set_seq(oInMsgHead.seq());
    util::CJsonObject jObjReturn;
    jObjReturn.Add("errcode", iRet);
    jObjReturn.Add("node_id", iRet ? 0 : regNodeStatus.nodeId);
    oOutMsgBody.set_body(jObjReturn.ToString());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (iRet)
    {
        LOG4_WARN("register node(%s)false",regNodeStatus.nodeType.c_str());
    }
    else
    {
        LOG4_DEBUG("register node(%s) ok nodeid(%d)",regNodeStatus.nodeType.c_str(),regNodeStatus.nodeId);
    }
    return GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}
bool NodeSession::SendServerConfig(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo &regNodeStatus)
{
	LOG4_DEBUG("");
    const std::string& nodeType = regNodeStatus.nodeType;
    if (!nodeType.empty())//目前注册时自动下发的只检查服务器配置
    {
        //从DB加载服务器配置
        std::string configContent;
        std::string configFile;
        uint32 updateTime(0);
        uint32 auto_send(0);
        uint32 reload_config(0);
        if (!LoadServerConfig(nodeType,0,configContent,configFile,updateTime,auto_send,reload_config))
        {
            LOG4_DEBUG("register node type(%s) don't has config type for %d",
                            nodeType.c_str(),0);
            return false;
        }
        if(1 != auto_send)
        {
            LOG4_DEBUG("register node type(%s) don't has auto config type for %d",
                            nodeType.c_str(),0);
            return false;
        }
        if(configContent.empty())
        {
            LOG4_WARN("register node type(%s) don't has configContent",
                            nodeType.c_str());
            return false;
        }
        if(configFile.empty())
        {
            LOG4_WARN("register node type(%s) don't has configFile",
                            nodeType.c_str());
            return false;
        }
        util::CJsonObject objConfigContent;
        if (!objConfigContent.Parse(configContent))
        {
            LOG4_WARN("reg node(%s)load server coinfig from DB,it 's not json,check configContent: %s",
                            nodeType.c_str(), configContent.c_str());
            return false;
        }
        util::CJsonObject objConfig;
        objConfig.Add("config_content",objConfigContent);
        objConfig.Add("config_file",configFile);
        objConfig.Add("config_type",0);
        //发送配置
        std::string objConfigStr = objConfig.ToString();
        LOG4_DEBUG("send server config[%s]",objConfigStr.c_str());
        MsgHead oOutMsgHead;
        MsgBody oOutMsgBody;
        oOutMsgHead.set_cmd(net::CMD_REQ_SERVER_CONFIG);
        oOutMsgHead.set_seq(GetLabor()->GetSequence());
        oOutMsgBody.set_body(objConfigStr);
        oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
        if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead,oOutMsgBody))
        {
            LOG4_ERROR("failed to send to send server config");
            return (false);
        }
        LOG4_DEBUG("succ to send server objConfigStr[%s] to node(%s)",
                        objConfigStr.c_str(),nodeType.c_str());
    }
    else
    {
        LOG4_WARN("reg nodeType is empty");
        return (false);
    }
    return (true);
}

bool NodeSession::SendServerConfigToType(const std::string& node_type,int config_type,
                const util::CJsonObject& objConfigContent,const std::string& config_file,const std::string &sNodeIdentify)
{
    util::CJsonObject objConfig;
    objConfig.Add("config_content",objConfigContent);
    objConfig.Add("config_file",config_file);
    objConfig.Add("config_type",config_type);
    const std::string& strConfig = objConfig.ToString();
    if(node_type == "CENTER")
    {
        if(sNodeIdentify.size() > 0)
        {
            if (GetSelfNodeIdentify() != sNodeIdentify)
            {
                LOG4_DEBUG("GetSelfNodeIdentify(%s),sNodeIdentify(%s)",GetSelfNodeIdentify().c_str(),sNodeIdentify.c_str());
                return (false);
            }
        }
        MsgHead oMsgHead;
		MsgBody oMsgBody;
		oMsgHead.set_cmd(net::CMD_REQ_SERVER_CONFIG);
		oMsgHead.set_seq(GetSequence());
		oMsgBody.set_body(strConfig);
		oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
		if (!GetLabor()->SendToParent(oMsgHead,oMsgBody))
		{
			LOG4_WARN("failed to SendToParent");
			return (false);
		}
		LOG4_DEBUG("succ to SendToParent");
    }
    else
    {
        if(sNodeIdentify.size() > 0)
        {
        	net::StepState* pstep = new net::StepState();
			pstep->AsyncSend(sNodeIdentify,strConfig,net::CMD_REQ_SERVER_CONFIG);
			if (!net::StepState::Launch(GetLabor(),pstep))
			{
				LOG4_WARN("%s StepState::Launch failed",__FUNCTION__);
				return false;
			}
			LOG4_DEBUG("succ to send to node(%s,%s)",node_type.c_str(),sNodeIdentify.c_str());
        }
        else
        {
        	MsgHead oMsgHead;
			MsgBody oMsgBody;
			oMsgHead.set_cmd(net::CMD_REQ_SERVER_CONFIG);
			oMsgHead.set_seq(GetSequence());
			oMsgBody.set_body(strConfig);
			oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
			if (!SendToNodeType(node_type, oMsgHead,oMsgBody))
			{
				LOG4_ERROR("failed to send to send server config to node_type(%s)",node_type.c_str());
				return (false);
			}
        }
    }
    LOG4_DEBUG("succ to send to send server config to node_type(%s)",node_type.c_str());
    return true;
}

bool NodeSession::ReloadServerConfigToType(const std::string& node_type,const NodeConfigFile &nodeConfigFile)
{
    /*
     {
         "cmd":[505,506],
         "url_path":["path1","path2"]
     }
     * */
    util::CJsonObject objConfig;
    util::CJsonObject objCmds;
    util::CJsonObject objUrlPath;
    if(!nodeConfigFile.cmds.empty())
    {
        if(!objCmds.Parse(nodeConfigFile.cmds))
        {
            LOG4_WARN("failed to Parse cmds(%s)",nodeConfigFile.cmds.c_str());
        }
    }
    if(!nodeConfigFile.url_paths.empty())
    {
        if(!objUrlPath.Parse(nodeConfigFile.url_paths))
        {
            LOG4_WARN("failed to Parse url_paths(%s)",nodeConfigFile.url_paths.c_str());
        }
    }
    bool boHasSo(false);
    if(!objCmds.IsEmpty())
    {
        objConfig.Add("cmd",objCmds);
        boHasSo = true;
    }
    if(!objUrlPath.IsEmpty())
    {
        objConfig.Add("url_path",objUrlPath);
        boHasSo = true;
    }
    if(!boHasSo)
    {
        LOG4_DEBUG("no so to reload");
        return false;
    }
    MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgHead.set_cmd(net::CMD_REQ_RELOAD_LOGIC_CONFIG);
	oOutMsgHead.set_seq(GetLabor()->GetSequence());
	oOutMsgBody.set_body(objConfig.ToString());
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if(node_type == "CENTER")
    {
    	if (!GetLabor()->SendToParent(oOutMsgHead,oOutMsgBody))
    	{
    		LOG4_WARN("failed to send to node(%s)",node_type.c_str());
			return (false);
    	}
    	LOG4_DEBUG("succ to send to node(%s)",node_type.c_str());
    }
    else if (!SendToNodeType(node_type, oOutMsgHead,oOutMsgBody))
    {
        LOG4_ERROR("failed to send to send server config to node_type(%s)",node_type.c_str());
        return (false);
    }
    LOG4_DEBUG("succ to send to send server config to node_type(%s)",node_type.c_str());
    return true;
}

//广播给所有指定类型的服务器节点的管理者
bool NodeSession::SendToNodeType(const std::string& strNodeType, const MsgHead& oOutMsgHead, const MsgBody& oOutMsgBody)
{
    bool boSent(false);
    for (NodesStatusMapCIT it_iter = m_mapNodesStatus.begin();it_iter != m_mapNodesStatus.end(); ++it_iter) //已注册服务器
    {
        const NodeStatusInfo& info = it_iter->second;
        if(info.nodeType == strNodeType)
        {
        	const std::string& strNodeKey = info.getNodeKey();
            net::StepState* pstep = new net::StepState();
			pstep->AsyncSend(strNodeKey,oOutMsgHead,oOutMsgBody);
			if (!net::StepState::Launch(GetLabor(),pstep,3,1))
			{
				LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
			}
			else
			{
				boSent = true;
			}
			LOG4_DEBUG("succ to send to node(%s,%s)",strNodeType.c_str(),strNodeKey.c_str());
        }
    }
    return boSent;
}

//发送其他服务器给注册者
int NodeSession::SendOthersToReg(const net::tagMsgShell& stMsgShell,const NodeStatusInfo &regNodeStatus)
{
	LOG4_DEBUG("");
    const std::string& strRegNodeType = regNodeStatus.nodeType;
    //发送其他服务器给注册者
    //发送格式{\"node_arry_reg\":[{\\"node_type\\":\"LOGIC\",\\"node_ip\\":\"192.168.18.22\",\\"node_port\\":40120,\\"worker_num\\":2}]}
	const NodeType* pNodeType = GetNodeTypeServerInfo(strRegNodeType);
	if (pNodeType)
	{
		util::CJsonObject jRegisteredNodesObj;
		GetNeededNodesStatus(pNodeType->neededServers, jRegisteredNodesObj);//获取需要的已注册的服务
		net::StepState* pstep = new net::StepState();
		pstep->AsyncSend(stMsgShell,jRegisteredNodesObj.ToString(),net::CMD_REQ_NODE_REG_NOTICE);
		if (!net::StepState::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("MysqlStep::Launch failed,strRegNodeType:%s",strRegNodeType.c_str());
			return (ERR_SERVER_ERROR);
		}
	}
	else
	{
		LOG4_DEBUG("node type(%s) don't need other server routes",strRegNodeType.c_str());
	}
    return (ERR_OK);
}

//发送注册者给其它服务
int NodeSession::SendRegToOthers(const NodeStatusInfo &regNodeStatus)
{
	LOG4_DEBUG("");
    if(regNodeStatus.suspend)//正常状态的节点才把自己的路由发布出去
    {
        LOG4_DEBUG("regNodeStatus(%s,%s) is suspend",regNodeStatus.getNodeKey().c_str(),regNodeStatus.nodeType.c_str());
        return (ERR_OK);
    }
    const std::string& strRegNodeType = regNodeStatus.nodeType;
    //发送格式 {\"node_arry_reg\":[{\\"node_type\\":\"ACCESS\",\\"node_ip\\":\"192.168.18.22\",\\"node_port\\":40111,\\"worker_num\\":2}]}
	util::CJsonObject jRegNodeObj;
	jRegNodeObj.AddEmptySubArray("node_arry_reg");
	util::CJsonObject tmember;
	tmember.Add("node_type", regNodeStatus.nodeType);
	tmember.Add("node_ip", regNodeStatus.nodeInnerIp);
	tmember.Add("node_port", regNodeStatus.nodeInnerPort);
	tmember.Add("worker_num", regNodeStatus.workerNum);
	jRegNodeObj["node_arry_reg"].Add(tmember);
	const std::string& toNoticeMsgBody = jRegNodeObj.ToString();
	for (NodesStatusMapCIT it_iter = m_mapNodesStatus.begin(); it_iter != m_mapNodesStatus.end(); ++it_iter) //已注册服务器
	{
		const NodeStatusInfo& info = it_iter->second;
		//给其他服务发送通知(包括同一个节点的其他子进程)
		const NodeType* pNodeType = GetNodeTypeServerInfo(info.nodeType);
		if (pNodeType)    //已注册的节点需要的节点类型
		{
			if (pNodeType->neededServers.size() > 0)
			{
				const NodeType::ServersList& neededServersTypeVec = pNodeType->neededServers;
				for (NodeType::ServersListCIT it = neededServersTypeVec.begin();it != neededServersTypeVec.end(); ++it)
				{
					if (strRegNodeType == *it)  //刚注册的服务是已注册服务需要的节点类型，则发送通知
					{
						//通知已注册的服务
						net::StepState* pstep = new net::StepState();
						pstep->AsyncSend(info.getNodeKey(),toNoticeMsgBody,net::CMD_REQ_NODE_REG_NOTICE);
						if (!net::StepState::Launch(GetLabor(),pstep))
						{
							LOG4_WARN("MysqlStep::Launch failed,strRegNodeType:%s",strRegNodeType.c_str());
						}
					}
				}
			}
			else
			{
				LOG4_DEBUG("registed node type(%s) don't need other node routes",
								info.nodeType.c_str());
			}
		}
		else
		{
			LOG4_WARN("node type(%s) don't have such server,please check table tb_nodetype",
							info.nodeType.c_str());
		}
	}
    return (ERR_OK);
}

int NodeSession::SendCenterToReg(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,const NodeStatusInfo &regNodeStatus)
{
    //发送中心服务器给注册者
    //发送格式{\"node_arry_reg\":[{\\"node_type\\":\"CENTER\",\\"node_ip\\":\"192.168.18.22\",\\"node_port\\":40120,\\"worker_num\\":1}]}
	const NodeType* pNodeType = GetNodeTypeServerInfo(regNodeStatus.nodeType);
	if (pNodeType)
	{
		util::CJsonObject jRegisteredNodesObj;
		GetCenterNodesStatus(jRegisteredNodesObj);//获取本中心节点信息
		net::StepState* pstep = new net::StepState();
		pstep->AsyncSend(stMsgShell,jRegisteredNodesObj.ToString(),net::CMD_REQ_NODE_REG_NOTICE);
		if (!net::StepState::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("MysqlStep::Launch failed,strRegNodeType:%s",regNodeStatus.nodeType.c_str());
			return (ERR_SERVER_ERROR);
		}
	}
	else
	{
		LOG4_DEBUG("node type(%s) don't need other server routes",regNodeStatus.nodeType.c_str());
	}
    return (ERR_OK);
}
//加载服务器配置
bool NodeSession::LoadServerConfig(const std::string &nodetype,uint32 configtype,
                std::string& config_content,std::string &config_file,uint32 &update_time,uint32 &auto_send,uint32 &reload_config)
{
    if (nodetype.empty())
    {
        LOG4_ERROR("nodetype empty when LoadServerConfig");
        return (false);
    }
    config_content.clear();
    config_file.clear();
    char szSql[128];
    snprintf(szSql, sizeof(szSql) - 1,
                    "SELECT * from  %s WHERE node_type='%s' AND config_type=%d",
                    NODE_SERVER_CONFIG_TABLE, nodetype.c_str(), configtype);
    util::T_vecResultSet vecRes;
    if (0 != GetSyncMysqlDbi()->ExecSql(szSql, vecRes))
    {
        LOG4_ERROR("SELECT from %s error,errno(%d:%s),szSql(%s)",
                        NODE_SERVER_CONFIG_TABLE,
						GetSyncMysqlDbi()->GetErrno(),
						GetSyncMysqlDbi()->GetError().c_str(),
                        szSql);
        return (false);
    }
    if(GetSyncMysqlDbi()->GetErrno())
    {
        LOG4_WARN("failed to select from %s empty,check config table,nodetype(%s),configtype(%d),szSql(%s)",
                        NODE_SERVER_CONFIG_TABLE, nodetype.c_str(), configtype,
                        szSql);
        return (false);
    }
    if (vecRes.empty())
    {
        LOG4_INFO("select from %s empty,check config table,nodetype(%s),configtype(%d),szSql(%s)",
                        NODE_SERVER_CONFIG_TABLE, nodetype.c_str(), configtype,
                        szSql);
        return (true);
    }
    util::T_mapRow& valmap = *vecRes.begin();
    if (valmap.end() == valmap.find("config_content"))
    {
        LOG4_ERROR("%s loadFromMapRow don't find config_content",
                        NODE_SERVER_CONFIG_TABLE);
        return (false);
    }
    config_content = valmap["config_content"];
    if(valmap.end() == valmap.find("config_file"))
    {
        LOG4_ERROR("%s loadFromMapRow don't find config_file",
                        NODE_SERVER_CONFIG_TABLE);
        return (false);
    }
    config_file = valmap["config_file"];
    if(valmap.end() == valmap.find("update_time"))
    {
        LOG4_ERROR("%s loadFromMapRow don't find update_time",
                        NODE_SERVER_CONFIG_TABLE);
        return (false);
    }
    update_time = util::TimeStr2time_t(valmap["update_time"]);
    if(valmap.end() == valmap.find("auto_send"))
    {
        LOG4_ERROR("%s loadFromMapRow don't find auto_send",
                        NODE_SERVER_CONFIG_TABLE);
        return (false);
    }
    auto_send = ::atoi(valmap["auto_send"].c_str());
    if(valmap.end() == valmap.find("reload_config"))
    {
        LOG4_ERROR("%s loadFromMapRow don't find reload_config",
                        NODE_SERVER_CONFIG_TABLE);
        return (false);
    }
    reload_config = ::atoi(valmap["reload_config"].c_str());
    return (true);
}

int NodeSession::CheckServerConfigFromDB(const std::string &node_type,uint32 config_type,
        const std::string& config_content,const std::string &config_file,uint32 auto_send)
{
    /*
     *  node_type
        config_type
        config_content
        config_file
     * */
    {//检查配置格式
        //  {"cmd":1001,"so_path":"plugins/Logic/CmdLogin.so","entrance_symbol":"create","load":false,"version":1}
        if(node_type.empty())
        {
            LOG4_WARN("nodeType is empty");
            return ERR_REQ_MISS_PARAM;
        }
        if(config_content.empty())
        {
            LOG4_WARN("configContent is empty");
            return ERR_REQ_MISS_PARAM;
        }
        if(config_file.empty())
        {
            LOG4_WARN("configFile is empty");
            return ERR_REQ_MISS_PARAM;
        }
    }
    SetCurrentTime();
    LOG4_INFO("node_type:%s,config_file:%s,config_content:%s",node_type.c_str(),config_file.c_str(),config_content.c_str());
    {//检查配置
        std::string checkConfigContent;
        std::string checkConfigFile;
        uint32 checkUpdateTime(0);
        uint32 checkAutoSend(0);
        uint32 checkReloadConfig(0);
        //检查新旧配置是否一样
        if(!LoadServerConfig(node_type,config_type,checkConfigContent,checkConfigFile,checkUpdateTime,checkAutoSend,checkReloadConfig))
        {
            LOG4_ERROR("failed to LoadServerConfig(%s,%d)",node_type.c_str(),config_type);
            return ERR_SERVERINFO_RECORD;
        }
        {//检查时间间隔
            int timeElapse = m_currentTime - checkUpdateTime;
            if(timeElapse <= m_nNodeTimeBeat)
            {
                LOG4_ERROR("timeElapse(%d),m_nNodeTimeBeat(%d),update config too often",timeElapse,m_nNodeTimeBeat);
                return core::ERR_REQ_FREQUENCY;
            }
            LOG4_DEBUG("update config:timeElapse(%d),m_nNodeTimeBeat(%d),checkUpdateTime(%u),nowTime(%s)",
                            timeElapse,m_nNodeTimeBeat,checkUpdateTime,util::time_t2TimeStr(m_currentTime).c_str());
        }
        if(config_content == checkConfigContent && config_file == checkConfigFile && auto_send == checkAutoSend)
        {
            LOG4_DEBUG("config_content and config_file and auto_send are the same,config_file(%s,%s),auto_send(%d,%d)",
                            config_file.c_str(),checkConfigFile.c_str(),auto_send,checkAutoSend);
            return ERR_SERVER_CONFIG_EXIST;
        }
    }
    return ERR_OK;
}

int NodeSession::UpdateServerConfigToDB(const std::string &node_type,uint32 config_type,
		const std::string& config_content,const std::string &config_file,uint32 auto_send,uint32 reload_config)
{
    if(0 == config_type)
    {//服务器在线加载配置是立刻生效
        reload_config = 1;
    }
	char szSql[20064];
	{//写入配置
	    snprintf(szSql, sizeof(szSql) - 1,
                        "replace into %s values('%s',%d,'%s','%s','%s',%d,%d)",
                        NODE_SERVER_CONFIG_TABLE, node_type.c_str(),config_type,config_content.c_str(),config_file.c_str(),
                        util::time_t2TimeStr(m_currentTime).c_str(),auto_send,reload_config);
        net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
		pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
		if (!net::MysqlStep::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("net::MysqlStep::Launch failed");
		}
	}
	{//写修改配置日志
	    snprintf(szSql, sizeof(szSql) - 1,
                        "insert into %s values('%s',%d,'%s','%s','%s',%d)",
                        NODE_SERVER_CONFIG_LOG_TABLE, node_type.c_str(),config_type,config_content.c_str(),config_file.c_str(),
                        util::time_t2TimeStr(m_currentTime).c_str(),auto_send);
        net::MysqlStep* pstep = new net::MysqlStep(m_dbConnInfo);
		pstep->SetTask(szSql,util::eSqlTaskOper_exec);//第一个任务(无需回调处理函数,也就无需设置自定义参数)
		if (!net::MysqlStep::Launch(GetLabor(),pstep))
		{
			LOG4_WARN("net::MysqlStep::Launch failed");
		}
	}
	return ERR_OK;
}

//获取需要的已注册的服务器的节点状态(json格式)
bool NodeSession::GetNeededNodesStatus(const std::vector<std::string>& neededServers,util::CJsonObject &jObj)
{
    const NodeStatusInfo *pInfo = NULL;
    jObj.AddEmptySubArray("node_arry_reg");
    util::CJsonObject tmember;
    for (NodesStatusMapCIT it_iter =
                    m_mapNodesStatus.begin();
                    it_iter != m_mapNodesStatus.end(); ++it_iter) //已注册服务器
    {
        pInfo = &it_iter->second;
        LOG4_TRACE("online nodetype[%s] suspend(%d)",pInfo->nodeType.c_str(),pInfo->suspend);
        for (std::vector<std::string>::const_iterator it =
                        neededServers.begin(); it != neededServers.end(); ++it)
        {
        	LOG4_TRACE("need nodetype[%s] checking online nodetype[%s] suspend(%d)",it->c_str(),pInfo->nodeType.c_str(),pInfo->suspend);
            if (*it == pInfo->nodeType) //判断是否是需要的服务器类型
            {
                if(eNodeStatusInfoNormal == pInfo->suspend)//正常节点才能下发路由
                {
                	LOG4_TRACE("add nodetype[%s]",pInfo->nodeType.c_str());
                    tmember.Clear();
                    tmember.Add("node_type", pInfo->nodeType);
                    tmember.Add("node_ip", pInfo->nodeInnerIp);
                    tmember.Add("node_port", pInfo->nodeInnerPort);
                    tmember.Add("worker_num", pInfo->workerNum);
                    jObj["node_arry_reg"].Add(tmember);
                }
                else
                {
                	LOG4_TRACE("need nodetype[%s] suspend(%d)",pInfo->nodeType.c_str(),pInfo->suspend);
                }
            }
            else
            {
            	LOG4_TRACE("need nodetype[%s] skip online nodetype[%s] suspend(%d)",it->c_str(),pInfo->nodeType.c_str(),pInfo->suspend);
            }
        }
    }
    //中心服务器信息
    tmember.Clear();
    tmember.Add("node_type", m_centerNodeType);
    tmember.Add("node_ip", m_centerInnerHost);
    tmember.Add("node_port", m_centerInnerPort);
    tmember.Add("worker_num", m_centerProcessNum);
    jObj["node_arry_reg"].Add(tmember);
    LOG4_TRACE("GetNeededNodesStatus[%s],center_node_type(%s)",
                    tmember.ToString().c_str(),
                    m_centerNodeType.c_str());
    return (true);
}

bool NodeSession::GetCenterNodesStatus(util::CJsonObject &jObj)
{
    jObj.AddEmptySubArray("node_arry_reg");
    util::CJsonObject tmember;
    //中心服务器信息
    tmember.Add("node_type", m_centerNodeType);
    tmember.Add("node_ip", m_centerInnerHost);
    tmember.Add("node_port", m_centerInnerPort);
    tmember.Add("worker_num", m_centerProcessNum);
    jObj["node_arry_reg"].Add(tmember);
    LOG4_DEBUG("GetNeededNodesStatus[%s],center_node_type(%s)",
                    tmember.ToString().c_str(),
                    m_centerNodeType.c_str());
    return (true);
}

int NodeSession::UnregNodeRoute(const NodeStatusInfo& nodeinfo)
{
	if (!SetNodeDataOfflineToDBByNodeId(nodeinfo.nodeId))
	{
		LOG4_WARN( "SetNodeDataOfflineToDBByNodeId false(%d)!",nodeinfo.nodeId);
		return ERR_SERVERINFO_RECORD;
	}
	//给其它模块发下线通知
	return SendUnregToOthers(nodeinfo);
}

//发送连接断开通知到其它服务
int NodeSession::SendUnregToOthers(const NodeStatusInfo &delNodeInfo)
{
    util::CJsonObject jNodeExitObj,tmember;
    jNodeExitObj.AddEmptySubArray("node_arry_exit");
    tmember.Add("node_type", delNodeInfo.nodeType);
    tmember.Add("node_ip", delNodeInfo.nodeInnerIp);
    tmember.Add("node_port", delNodeInfo.nodeInnerPort);
    tmember.Add("worker_num", delNodeInfo.workerNum);
    jNodeExitObj["node_arry_exit"].Add(tmember);
    const std::string& strDisConnectBody = jNodeExitObj.ToString();
    LOG4_DEBUG("SendUnregToOthers!jNodeExitObj[%s]",strDisConnectBody.c_str());
    bool boSendedNotice(false);
    //遍历管理器内存的node列表,如果断开连接的服务是它们需要的服务,则通知它们注销该断开连接的服务
    {
        //注销的服务类型
        const std::string& unRegServerType = delNodeInfo.nodeType;
        //在线节点管理器（key为节点类型：IP：端口，value为节点信息）
        const NodeSession::NodesStatusMap& mapNodeInfo = m_mapNodesStatus;
        for (NodeSession::NodesStatusMapCIT it_iter =mapNodeInfo.begin();it_iter != mapNodeInfo.end(); ++it_iter)//已注册的服务器
        {
            const NodeStatusInfo& nodeInfo = it_iter->second;
            //获取其他服务的配置(同一个节点的其他子进程也通知)
            const NodeType* pNodeType = GetNodeTypeServerInfo(nodeInfo.nodeType);
            if(pNodeType)
            {
                //其他服务需要的服务
                for(NodeType::ServersListCIT it = pNodeType->neededServers.begin();
                                        it != pNodeType->neededServers.end();++it)//该类服务器需要的服务器类型
                {
                    if(unRegServerType == *it)//注销的服务器是该类服务器需要的服务器,则通知该类服务器注销
                    {
                        LOG4_DEBUG("SendUnregToOthers nodeInfo.getNodeKey(%s)!jNodeExitObj[%s]",
                        		nodeInfo.getNodeKey().c_str(),jNodeExitObj.ToString().c_str());
                        net::StepState* pstep = new net::StepState();
						pstep->AsyncSend(nodeInfo.getNodeKey(),
								strDisConnectBody,net::CMD_REQ_NODE_REG_NOTICE);
						if (!net::StepState::Launch(GetLabor(),pstep))
						{
							LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
						}
						else
						{
							boSendedNotice = true;
						}
                    }
                }
            }
            else
            {
                LOG4_WARN("SendUnregToOthers!node type(%s) don't have server config,please check table tb_nodetype",
                            nodeInfo.nodeType.c_str());
            }
        }
    }
    if (!boSendedNotice)
    {
        LOG4_DEBUG( "did not send any notices to unregister node.jNodeExitObj(%s)",jNodeExitObj.ToString().c_str());
    }
    return (net::ERR_OK);
}
//关闭网关服务器（INTREFACE、ACCESS、OSSI）到指定更新节点路由信息
int NodeSession::SendOfflineToGateway(const NodeStatusInfo &offlineNodeInfo)
{
    util::CJsonObject jNodeExitObj,tmember;
    jNodeExitObj.AddEmptySubArray("node_arry_exit");
    tmember.Add("node_type", offlineNodeInfo.nodeType);
    tmember.Add("node_ip", offlineNodeInfo.nodeInnerIp);
    tmember.Add("node_port", offlineNodeInfo.nodeInnerPort);
    tmember.Add("worker_num", offlineNodeInfo.workerNum);
    jNodeExitObj["node_arry_exit"].Add(tmember);
    const std::string& strOfflineBody = jNodeExitObj.ToString();
    LOG4_DEBUG("SendOfflineToGateway!jNodeExitObj[%s]",strOfflineBody.c_str());
    bool boSendedNotice(false);
    //遍历管理器内存的node列表,如果断开连接的服务是它们需要的服务,则通知它们注销该断开连接的服务
    {
        //注销的服务类型
        const std::string& offlineNodeType = offlineNodeInfo.nodeType;
        //在线节点管理器（key为节点类型：IP：端口，value为节点信息）
        for (NodeSession::NodesStatusMapCIT it_iter =
        		m_mapNodesStatus.begin();
                        it_iter != m_mapNodesStatus.end(); ++it_iter)//已注册的服务器
        {
            const NodeStatusInfo& nodeInfo = it_iter->second;
            if(!IsGatewayType(nodeInfo.nodeType))//如果不是网关类型服务则不发送
            {
                continue;
            }
            //获取其他服务的配置
            const NodeType* pNodeType = GetNodeTypeServerInfo(nodeInfo.nodeType);
            if(pNodeType)
            {
                //其他服务需要的服务
                const std::vector<std::string>& neededServers = pNodeType->neededServers;
                for(std::vector<std::string>::const_iterator it = neededServers.begin();
                                        it != neededServers.end();++it)//该类服务器需要的服务器类型
                {
                    if(offlineNodeType == *it)//下线的服务是该类服务器需要的服务器,则通知该类服务器注销路由
                    {
                    	net::StepState* pstep = new net::StepState();
						pstep->AsyncSend(nodeInfo.getNodeKey(),
								strOfflineBody,net::CMD_REQ_NODE_REG_NOTICE);
						if (!net::StepState::Launch(GetLabor(),pstep))
						{
							LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
						}
						else
						{
							boSendedNotice = true;
						}
                    }
                }
            }
            else
            {
                LOG4_WARN("SendOfflineToGateway!notify node type(%s) don't have server config,please check table tb_nodetype",
                            nodeInfo.nodeType.c_str());
            }
        }
    }
    if (!boSendedNotice)
    {
        LOG4_DEBUG( "did not send any notices to unregister node.jNodeExitObj(%s)",jNodeExitObj.ToString().c_str());
    }
    return (net::ERR_OK);
}

//发送注册者给网关服务
int NodeSession::SendOnlineToGateway(const NodeStatusInfo& onlineNodeInfo)
{
    const std::string& strOnlineNodeType = onlineNodeInfo.nodeType;
    //发送格式 {\"node_arry_reg\":[{\\"node_type\\":\"ACCESS\",\\"node_ip\\":\"192.168.18.22\",\\"node_port\\":40111,\\"worker_num\\":2}]}
	util::CJsonObject jRegNodeObj;
	jRegNodeObj.AddEmptySubArray("node_arry_reg");
	util::CJsonObject tmember;
	tmember.Add("node_type", onlineNodeInfo.nodeType);
	tmember.Add("node_ip", onlineNodeInfo.nodeInnerIp);
	tmember.Add("node_port", onlineNodeInfo.nodeInnerPort);
	tmember.Add("worker_num", onlineNodeInfo.workerNum);
	jRegNodeObj["node_arry_reg"].Add(tmember);
	const std::string& strOnlineBody = jRegNodeObj.ToString();
	for (NodesStatusMapCIT it_iter = m_mapNodesStatus.begin();it_iter != m_mapNodesStatus.end(); ++it_iter) //已注册服务器
	{
		const NodeStatusInfo& nodeInfo = it_iter->second;
		if(!IsGatewayType(nodeInfo.nodeType))//如果不是网关类型服务则不发送
		{
			continue;
		}
		//给其他服务发送通知
		const NodeType* pNodeType =
						GetNodeTypeServerInfo(nodeInfo.nodeType);
		if (pNodeType)    //已注册的节点需要的节点类型
		{
			const std::vector<std::string>& neededServersTypeVec = pNodeType->neededServers;
			if (!neededServersTypeVec.empty())
			{
				for (std::vector<std::string>::const_iterator it =
								neededServersTypeVec.begin();
								it != neededServersTypeVec.end(); ++it)
				{
					if (strOnlineNodeType == *it)  //上线服务是已注册服务需要的节点类型，则发送通知
					{
						net::StepState* pstep = new net::StepState();//通知已注册的服务
						pstep->AsyncSend(nodeInfo.getNodeKey(),strOnlineBody,net::CMD_REQ_NODE_REG_NOTICE);
						if (!net::StepState::Launch(GetLabor(),pstep))
						{
							LOG4_WARN("%s MysqlStep::Launch failed",__FUNCTION__);
						}
					}
				}
			}
			else
			{
				LOG4_DEBUG("online node type(%s) don't need other node routes",
								nodeInfo.nodeType.c_str());
			}
		}
		else
		{
			LOG4_WARN("notify node type(%s) don't have such server,please check table tb_nodetype",
							nodeInfo.nodeType.c_str());
		}
	}
    return (ERR_OK);
}


bool NodeSession::IsGatewayType(const std::string& nodetype)
{
    int s = m_GatewayTypeList.size();
    for(int i = 0;i < s;++i)
    {
        if (nodetype == m_GatewayTypeList[i])
        {
            LOG4_DEBUG("SendOfflineToGateway:%s is gateway type",nodetype.c_str());
            return true;
        }
    }
    return false;
}

void NodeSession::RemoveFlag(std::string &str, char flag)const
{
	std::string::iterator it = std::remove(str.begin(), str.end(), flag);
	str.erase(it, str.end());
}

std::string NodeSession::GetSelfNodeIdentify()const
{
	char strSelfNodeKey[100] = { 0 };
	sprintf(strSelfNodeKey, "%s:%d", m_centerInnerHost.c_str(), m_centerInnerPort);
	return std::string(strSelfNodeKey);
}
bool NodeSession::IsSelfNodeIdentify(const std::string& identify)const
{
	char strSelfNodeKey[100] = { 0 };
	sprintf(strSelfNodeKey, "%s:%d", m_centerInnerHost.c_str(), m_centerInnerPort);
	if(identify == strSelfNodeKey)
	{
		return true;
	}
	return false;
}
bool NodeSession::IsCenterServer(const std::string& identify)
{
	std::vector<CenterServer>::const_iterator it = m_vecCenterServer.begin();
	std::vector<CenterServer>::const_iterator itEnd = m_vecCenterServer.end();
	for(;it != itEnd;++it)
	{
		if (identify == it->server_identify)
		{
			return true;
		}
		LOG4_DEBUG("server_identify(%s),identify(%s)",it->server_identify.c_str(),identify.c_str());
	}
	return false;
}
bool NodeSession::HasIdentifyAuthority(const std::string& sNodeIdentify)
{
	LOG4_DEBUG("HasAuthority sNodeIdentify(%s)",sNodeIdentify.c_str());
	if(IsCenterServer(sNodeIdentify))
	{//中心节点操作由中心节点自己来完成
		if(!IsSelfNodeIdentify(sNodeIdentify))
		{
			LOG4_DEBUG("IsSelfNodeIdentify failed(%s)",sNodeIdentify.c_str());
			return false;
		}
	}
	else//非中心节点操作，由中心主节点完成
	{
		if(!IsMaster())
		{
			LOG4_DEBUG("it 's not master(%s)",sNodeIdentify.c_str());
			return false;
		}
	}
	return true;
}


NodeSession* GetNodeSession(net::Labor* pLabor,const std::string &configPath,bool boReload)
{
    NodeSession* pSess = (NodeSession*) pLabor->GetSession(1, "net::NodeSession");
    if (pSess)
    {
        if(boReload)
        {//重新加载的重新初始化session
            std::string err;
            if(!pSess->Init(configPath,err,boReload))
            {
                LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"NodeSession init error!%s",err.c_str());
                pLabor->DeleteCallback(pSess);
                return NULL;
            }
        }
        return (pSess);
    }
    int nodeSessionTimeOut = (net::gc_iBeatInterval/2 -1) > 0  ? (net::gc_iBeatInterval/2 -1) :1;
    //注册节点会话
    pSess = new NodeSession(1,nodeSessionTimeOut);
    if (pSess == NULL)
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"error %d: new NodeSession() error!", ERR_NEW);
        return (NULL);
    }
    LOG4CPLUS_INFO_FMT(pLabor->GetLogger(),"new NodeSession(1,%d) NodeSession timeout:%d",nodeSessionTimeOut,nodeSessionTimeOut);
    if(!pLabor->Pretreat(pSess))
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"GetLabor failed to Pretreat pSess");
        return (NULL);
    }
    std::string err;
    if(!pSess->Init(configPath,err))
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"NodeSession init error!%s",err.c_str());
        delete pSess;
        pSess = NULL;
        return (NULL);
    }
    if (pLabor->RegisterCallback(pSess))
    {
        LOG4CPLUS_DEBUG_FMT(pLabor->GetLogger(),"register NodeSession ok!");
        if(!pSess->LoadNodeRoute())
        {
            LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"failed to LoadNodeRoute");
        }
        return (pSess);
    }
    else
    {
        LOG4CPLUS_ERROR_FMT(pLabor->GetLogger(),"register NodeSession error!");
        delete pSess;
        pSess = NULL;
    }
    return (NULL);
}




}
;
//name space analysis
