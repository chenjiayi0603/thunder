/*******************************************************************************
 * Project:  DbAgent
 * @file     CmdPgOper.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月28日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdPgOper.hpp"
#include "PgAgentSession.h"

#ifdef __cplusplus
extern "C" {
#endif
    net::Cmd* create()
    {
        net::Cmd* pCmd = new net::CmdPgOper();
        return(pCmd);
    }
#ifdef __cplusplus
}
#endif

namespace net
{

CmdPgOper::CmdPgOper()
    : m_iConnectionTimeout(gc_iMaxBeatTimeInterval),
      m_uiSectionFrom(0), m_uiSectionTo(0), m_uiHash(0), m_uiDivisor(1),m_uiSync(0)
{
}

CmdPgOper::~CmdPgOper()
{
    for (std::map<std::string, tagConnection*>::iterator iter = m_mapDbiPool.begin();
        iter != m_mapDbiPool.end(); ++iter)
    {
        if (iter->second != NULL)
        {
            delete iter->second;
            iter->second = NULL;
        }
    }
    m_mapDbiPool.clear();
}

bool CmdPgOper::Init()
{
    //配置文件路径查找
    std::string strConfFile = GetConfigPath() + std::string("Agent/CmdPgOper.json");
    LOG4_DEBUG("CONF FILE = %s.", strConfFile.c_str());

    std::ifstream fin(strConfFile.c_str());
    if (fin.good())
    {
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        fin.close();
        if (m_oDbConf.Parse(ssContent.str()))
        {
            LOG4_TRACE("m_oDbConf pasre OK");
            char szInstanceGroup[64] = {0};
            char szFactorSectionKey[32] = {0};
            uint32 uiDataType = 0;
            uint32 uiFactor = 0;
            uint32 uiFactorSection = 0;
            if (m_oDbConf["table"].IsEmpty())
            {
                LOG4_ERROR("m_oDbConf[\"table\"] is empty!");
                return(false);
            }
            if (m_oDbConf["cluster"].IsEmpty())
            {
                LOG4_ERROR("m_oDbConf[\"cluster\"] is empty!");
                return(false);
            }
            if (m_oDbConf["db_group"].IsEmpty())
            {
                LOG4_ERROR("m_oDbConf[\"db_group\"] is empty!");
                return(false);
            }

            for (int i = 0; i < m_oDbConf["data_type"].GetArraySize(); ++i)
            {
                if (m_oDbConf["data_type_enum"].Get(m_oDbConf["data_type"](i), uiDataType))
                {
                    for (int j = 0; j < m_oDbConf["section_factor"].GetArraySize(); ++j)
                    {
                        if (m_oDbConf["section_factor_enum"].Get(m_oDbConf["section_factor"](j), uiFactor))
                        {
                            if (m_oDbConf["factor_section"][m_oDbConf["section_factor"](j)].IsArray())
                            {
                                std::set<uint32> setFactorSection;
                                for (int k = 0; k < m_oDbConf["factor_section"][m_oDbConf["section_factor"](j)].GetArraySize(); ++k)
                                {
                                    if (m_oDbConf["factor_section"][m_oDbConf["section_factor"](j)].Get(k, uiFactorSection))
                                    {
                                        snprintf(szInstanceGroup, sizeof(szInstanceGroup), "%u:%u:%u", uiDataType, uiFactor, uiFactorSection);
                                        snprintf(szFactorSectionKey, sizeof(szFactorSectionKey), "LE_%u", uiFactorSection);
                                        setFactorSection.insert(uiFactorSection);
                                        LOG4_DEBUG("szInstanceGroup(%s) add uiFactorSection(%u)",szInstanceGroup,uiFactorSection);
                                        util::CJsonObject* pInstanceGroup
                                            = new util::CJsonObject(m_oDbConf["cluster"][m_oDbConf["data_type"](i)][m_oDbConf["section_factor"](j)][szFactorSectionKey]);
                                        LOG4_TRACE("%s : %s", szInstanceGroup, pInstanceGroup->ToString().c_str());
                                        m_mapDbInstanceInfo.insert(std::pair<std::string, util::CJsonObject*>(szInstanceGroup, pInstanceGroup));
                                    }
                                    else
                                    {
                                        LOG4_ERROR("m_oDbConf[\"factor_section\"][%s](%d) is not exist!",
                                                        m_oDbConf["section_factor"](j).c_str(), k);
                                        continue;
                                    }
                                }
                                snprintf(szInstanceGroup, sizeof(szInstanceGroup), "%u:%u", uiDataType, uiFactor);
                                LOG4_DEBUG("szInstanceGroup(%s),factor size %u", szInstanceGroup, setFactorSection.size());
                                m_mapFactorSection.insert(std::pair<std::string, std::set<uint32> >(szInstanceGroup, setFactorSection));
                            }
                            else
                            {
                                LOG4_ERROR("m_oDbConf[\"factor_section\"][%s] is not a json array!",
                                                m_oDbConf["section_factor"](j).c_str());
                                continue;
                            }
                        }
                        else
                        {
                            LOG4_ERROR("missing %s in m_oDbConf[\"section_factor_enum\"]", m_oDbConf["section_factor"](j).c_str());
                            continue;
                        }
                    }
                }
                else
                {
                    LOG4_ERROR("missing %s in m_oDbConf[\"data_type_enum\"]", m_oDbConf["data_type"](i).c_str());
                    continue;
                }
            }
        }
        else
        {
            LOG4_ERROR("m_oDbConf pasre error");
            return(false);
        }
    }
    else
    {
        //配置信息流读取失败
        LOG4_ERROR("Open conf \"%s\" error!", strConfFile.c_str());
        return(false);
    }
    PgAgentSession* pDbAgentSession = GetPgAgentSession(this->GetLabor());
    if (!pDbAgentSession)
    {
        LOG4_ERROR("GetPgAgentSession error!");
        return false;
    }
    return(true);
}

bool CmdPgOper::AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    DataMem::MemOperate oMemOperate;
    if (!oMemOperate.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("Parse protobuf msg error!");
        Response(stMsgShell,oInMsgHead,ERR_PARASE_PROTOBUF, "DataMem::MemOperate ParseFromString() failed!");
        return(false);
    }
    pqxx::connection* pMasterDbi = NULL;
	pqxx::connection* pSlaveDbi = NULL;
    bool bConnected = GetDbConnection(stMsgShell,oInMsgHead,oMemOperate, &pMasterDbi, &pSlaveDbi);
    if (bConnected)
    {
        LOG4_TRACE("succeed in getting db connection");
        {
        	int iResult = 0;
			if (DataMem::MemOperate::DbOperate::SELECT == oMemOperate.db_operate().query_type())
			{
				iResult = SyncQuery(stMsgShell,oInMsgHead,oMemOperate, pSlaveDbi);
				if (0 == iResult)
				{//异步返回结果
					return(true);
				}
				else
				{
					iResult = SyncQuery(stMsgShell,oInMsgHead,oMemOperate, pMasterDbi);
					return (0 == iResult);
				}
			}
			else
			{
				if (NULL == pMasterDbi)
				{//异步返回结果
					Response(stMsgShell,oInMsgHead,iResult, "");
					return(false);
				}
				iResult = SyncQuery(stMsgShell,oInMsgHead,oMemOperate, pMasterDbi);
				return (0 == iResult);
			}
        }
    }
    return(false);
}

bool CmdPgOper::GetDbConnection(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,const DataMem::MemOperate& oQuery,
                pqxx::connection** ppMasterDbi, pqxx::connection** ppSlaveDbi)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    char szFactor[32] = {0};
    int32 iDataType = 0;
    int32 iSectionFactorType = 0;
    m_oDbConf["table"][oQuery.db_operate().table_name()].Get("data_type", iDataType);
    m_oDbConf["table"][oQuery.db_operate().table_name()].Get("section_factor", iSectionFactorType);
    snprintf(szFactor, 32, "%d:%d", iDataType, iSectionFactorType);
    LOG4_TRACE("oQuery szFactor(%s),section_factor(%u),mod_factor(%llu)",szFactor,oQuery.section_factor(),
                    oQuery.db_operate().mod_factor());//数据请求类型和因子
    std::map<std::string, std::set<uint32> >::const_iterator c_factor_iter =  m_mapFactorSection.find(szFactor);
    if (c_factor_iter == m_mapFactorSection.end())
    {
        LOG4_ERROR("no db config found for data_type %d section_factor_type %d",
                        iDataType, iSectionFactorType);
        Response(stMsgShell,oInMsgHead,ERR_LACK_CLUSTER_INFO, "no db config found for oMemOperate.cluster_info()!");
        return(false);
    }
    else
    {
        std::set<uint32>::const_iterator c_section_iter = c_factor_iter->second.lower_bound(oQuery.section_factor());
        if (c_section_iter == c_factor_iter->second.end())
        {
            LOG4_ERROR("no factor_section config found for data_type %u section_factor_type %u section_factor %u",
                            iDataType, iSectionFactorType, oQuery.section_factor());
            Response(stMsgShell,oInMsgHead,ERR_LACK_CLUSTER_INFO, "no db config for the cluster info!");
            return(false);
        }
        else
        {
            snprintf(szFactor, 32, "%u:%u:%u", iDataType, iSectionFactorType, *c_section_iter);
            m_uiSectionTo = *c_section_iter;
            --c_section_iter;
            if (c_section_iter == c_factor_iter->second.end())
            {
                m_uiSectionFrom = 0;
            }
            else
            {
                m_uiSectionFrom = *c_section_iter + 1;
            }
            //数据库实例配置2:4:1000000 : {"db_userinfo":"robot_db_instance_1","im_relation":"robot_db_instance_1","db_singlechat":"robot_db_instance_1","im_group":"robot_db_instance_1","im_custom":"custom_db"}
            std::map<std::string, util::CJsonObject*>::iterator conf_iter = m_mapDbInstanceInfo.find(szFactor);
            if (conf_iter == m_mapDbInstanceInfo.end())
            {
                LOG4_ERROR("no db config found for %s which consist of data_type %u section_factor_type %u section_factor %u",
                                szFactor, iDataType, iSectionFactorType, oQuery.section_factor());
                Response(stMsgShell,oInMsgHead,ERR_LACK_CLUSTER_INFO, "no db config for the cluster info!");
                return(false);
            }
            //数据库实例对应的数据类型、因子和分段
            LOG4_TRACE("db instance szFactor(%s),uiSectionTo(%u),uiSectionFrom(%u)",szFactor,m_uiSectionTo,m_uiSectionFrom);
            std::string strDbName = m_oDbConf["table"][oQuery.db_operate().table_name()]("db_name");
            LOG4_TRACE("strDbName(%s)",strDbName.c_str());
            std::string strInstance;
            if (!conf_iter->second->Get(strDbName, strInstance))
            {
                Response(stMsgShell,oInMsgHead,ERR_LACK_CLUSTER_INFO, "no db instance config for db name!");
                LOG4_ERROR("no db instance config for strDbName \"%s\"!", strDbName.c_str());
                return(false);
            }
            LOG4_DEBUG("strDbName(%s),strInstance(%s)",strDbName.c_str(),strInstance.c_str());
            util::CJsonObject& dbGroupConf = m_oDbConf["db_group"];
            if (oQuery.db_operate().query_type() > atoi(dbGroupConf[strInstance]("query_permit").c_str()))
            {
                Response(stMsgShell,oInMsgHead,ERR_NO_RIGHT, "no right to excute oMemOperate.db_operate().query_type()!");
                LOG4_ERROR("no right to excute oMemOperate.db_operate().query_type() %d!",
                                oQuery.db_operate().query_type());
                return(false);
            }
            util::CJsonObject& oInstanceConf = dbGroupConf[strInstance];
            std::string strUseGroup = oInstanceConf("use_group");
            util::CJsonObject& oGroupHostConf = oInstanceConf[strUseGroup];
            {//主从
                std::string strMasterIdentify = oGroupHostConf("master_host");
                std::string strSlaveIdentify = oGroupHostConf("slave_host");
                LOG4_TRACE("strMasterIdentify(%s),strSlaveIdentify(%s)",strMasterIdentify.c_str(),strSlaveIdentify.c_str());
                bool bEstablishConnection = FetchOrEstablishConnection(stMsgShell,oInMsgHead,
                                oQuery.db_operate().query_type(),strMasterIdentify, strSlaveIdentify,strDbName,
                                oInstanceConf, ppMasterDbi, ppSlaveDbi);
                return(bEstablishConnection);
            }
        }
    }
}

bool CmdPgOper::FetchOrEstablishConnection(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
				DataMem::MemOperate::DbOperate::E_QUERY_TYPE eQueryType,
                const std::string& strMasterIdentify, const std::string& strSlaveIdentify,const std::string &strDbName,
                const util::CJsonObject& oInstanceConf, pqxx::connection** ppMasterDbi, pqxx::connection** ppSlaveDbi)
{
    LOG4_TRACE("%s(%s, %s,%s, %s)", __FUNCTION__, strMasterIdentify.c_str(), strSlaveIdentify.c_str(),strDbName.c_str(), oInstanceConf.ToString().c_str());
    *ppMasterDbi = NULL;
    *ppSlaveDbi = NULL;
    int iResult(0);
    std::string strMasterDbIdentify = strMasterIdentify +":"+strDbName;//ip:port:db
    std::string strSlaveDbIdentify = strSlaveIdentify +":"+strDbName;//ip:port:db
    std::map<std::string, tagConnection*>::iterator dbi_iter = m_mapDbiPool.find(strMasterDbIdentify);
    if (dbi_iter == m_mapDbiPool.end())
    {
        tagConnection* pConnection = new tagConnection();
        if (NULL == pConnection)
        {
            Response(stMsgShell,oInMsgHead,ERR_NEW, "malloc space for db connection failed!");
            return(false);
        }
        pqxx::connection* pPgConn(NULL);
        iResult = ConnectDb(oInstanceConf, pPgConn, strMasterDbIdentify);
        if (0 == iResult)
        {
        	pConnection->pPgConn = pPgConn;
            LOG4_TRACE("succeed in connecting strMasterIdentify(%s)", strMasterDbIdentify.c_str());
            *ppMasterDbi = pPgConn;
            pConnection->iQueryPermit = atoi(oInstanceConf("query_permit").c_str());
            pConnection->iTimeout = atoi(oInstanceConf("timeout").c_str());
            pConnection->ullBeatTime = time(NULL);
            m_mapDbiPool.insert(std::pair<std::string, tagConnection*>(strMasterDbIdentify, pConnection));
        }
        else
        {
            if (DataMem::MemOperate::DbOperate::SELECT != eQueryType)
            {
                Response(stMsgShell,oInMsgHead,iResult, "");
            }
            delete pConnection;
            pConnection = NULL;
        }
    }
    else
    {
        *ppMasterDbi = dbi_iter->second->pPgConn;
    }

    LOG4_TRACE("find slave %s.", strSlaveDbIdentify.c_str());
    dbi_iter = m_mapDbiPool.find(strSlaveDbIdentify);
    if (dbi_iter == m_mapDbiPool.end())
    {
        tagConnection* pConnection = new tagConnection();
        if (NULL == pConnection)
        {
            Response(stMsgShell,oInMsgHead,ERR_NEW, "malloc space for db connection failed!");
            return(false);
        }
        pqxx::connection* pPgConn(NULL);
        iResult = ConnectDb(oInstanceConf, pPgConn, strSlaveDbIdentify);
        if (0 == iResult)
        {
        	pConnection->pPgConn = pPgConn;
            LOG4_TRACE("succeed in connecting strSlaveIdentify(%s)", strSlaveDbIdentify.c_str());
            *ppSlaveDbi = pPgConn;
            pConnection->iQueryPermit = atoi(oInstanceConf("query_permit").c_str());
            pConnection->iTimeout = atoi(oInstanceConf("timeout").c_str());
            pConnection->ullBeatTime = time(NULL);
            m_mapDbiPool.insert(std::pair<std::string, tagConnection*>(strSlaveDbIdentify, pConnection));
        }
        else
        {
            delete pConnection;
            pConnection = NULL;
        }
    }
    else
    {
        *ppSlaveDbi = dbi_iter->second->pPgConn;
    }

    LOG4_TRACE("pMasterDbi = 0x%x, pSlaveDbi = 0x%x.", *ppMasterDbi, *ppSlaveDbi);
    if (*ppMasterDbi || *ppSlaveDbi)
    {
        return(true);
    }
    else
    {
        return(false);
    }
}

int CmdPgOper::ConnectDb(const util::CJsonObject& oInstanceConf, pqxx::connection* &pPgConn,const std::string& strDbIdentify)
{
    LOG4_DEBUG("%s()", __FUNCTION__);
    util::tagDbConfDetail stDbConfDetail;
    std::string strDbName;
    std::string::size_type nIndex = strDbIdentify.find(":");//ip:port:db
	if (nIndex != std::string::npos)
	{
		std::string strDbHost = strDbIdentify.substr(0,nIndex);
		strncpy(stDbConfDetail.m_stDbConnInfo.m_szDbHost,strDbHost.c_str(),sizeof(stDbConfDetail.m_stDbConnInfo.m_szDbHost));
		std::string strRight = strDbIdentify.substr(nIndex + 1);
		nIndex = strRight.find(":");
		if (nIndex != std::string::npos)
		{
			std::string strDbPort = strRight.substr(0,nIndex);
			stDbConfDetail.m_stDbConnInfo.m_uiDbPort = atoi(strRight.c_str());

			strDbName = strRight.substr(nIndex + 1);
		}
		else
		{
			LOG4_ERROR("error strDbIdentify:%s", strDbIdentify.c_str());
			return -1;
		}
	}
	else
	{
		LOG4_ERROR("error strDbIdentify:%s", strDbIdentify.c_str());
		return -1;
	}
	strncpy(stDbConfDetail.m_stDbConnInfo.m_szDbUser,oInstanceConf("user").c_str(),sizeof(stDbConfDetail.m_stDbConnInfo.m_szDbUser));
    strncpy(stDbConfDetail.m_stDbConnInfo.m_szDbPwd,oInstanceConf("password").c_str(),sizeof(stDbConfDetail.m_stDbConnInfo.m_szDbPwd));
    strncpy(stDbConfDetail.m_stDbConnInfo.m_szDbName, strDbName.c_str(),sizeof(stDbConfDetail.m_stDbConnInfo.m_szDbName));
    strncpy(stDbConfDetail.m_stDbConnInfo.m_szDbCharSet,oInstanceConf("charset").c_str(),sizeof(stDbConfDetail.m_stDbConnInfo.m_szDbCharSet));
    stDbConfDetail.m_ucDbType = util::POSTGRESQL_DB;
    stDbConfDetail.m_ucAccess = 1; //直连
    stDbConfDetail.m_stDbConnInfo.uiTimeOut = atoi(oInstanceConf("timeout").c_str());

    LOG4_DEBUG("InitDbConn(%s, %s, %s, %s, %u, %s)", stDbConfDetail.m_stDbConnInfo.m_szDbHost,
                    stDbConfDetail.m_stDbConnInfo.m_szDbUser, stDbConfDetail.m_stDbConnInfo.m_szDbPwd,
                    stDbConfDetail.m_stDbConnInfo.m_szDbName, stDbConfDetail.m_stDbConnInfo.m_uiDbPort,
                    stDbConfDetail.m_stDbConnInfo.m_szDbCharSet);
    char connect[128];
    snprintf(connect,sizeof(connect),"dbname=%s hostaddr=%s user=%s password=%s port=%d",
    		stDbConfDetail.m_stDbConnInfo.m_szDbName,
    		stDbConfDetail.m_stDbConnInfo.m_szDbHost,
			stDbConfDetail.m_stDbConnInfo.m_szDbUser,
			stDbConfDetail.m_stDbConnInfo.m_szDbPwd,
			stDbConfDetail.m_stDbConnInfo.m_uiDbPort);
    //http://pqxx.org/devprojects/libpqxx/doc/4.0/html/Tutorial/
    //http://pqxx.org/devprojects/libpqxx/doc/4.0/html/Reference/modules.html
    try
    {
    	pPgConn = new pqxx::connection(connect);
		if(!pPgConn->is_open())
		{
			LOG4_ERROR("Connection failed!options:%s",pPgConn->options().c_str());
			delete pPgConn;
			pPgConn = NULL;
			return 1;
		}
    }
    catch (const pqxx::sql_error &e)
	{
		LOG4_ERROR("SQL error: %s.Query was:%s",e.what(),e.query().c_str());
		return 2;
	}
	catch (const std::exception &e)
	{
		LOG4_ERROR("SQL error: %s.Query was:%s",e.what());
		return 1;
	}
	LOG4_TRACE("Connection succesful!options:%s",pPgConn->options().c_str());
	return 0;
}

int CmdPgOper::ConnectDb(const util::tagDbConfDetail &stDbConfDetail, pqxx::connection* &pPgConn)
{
    LOG4_DEBUG("%s()", __FUNCTION__);
    LOG4_DEBUG("InitDbConn(%s, %s, %s, %s, %u, %s)", stDbConfDetail.m_stDbConnInfo.m_szDbHost,
                    stDbConfDetail.m_stDbConnInfo.m_szDbUser, stDbConfDetail.m_stDbConnInfo.m_szDbPwd,
                    stDbConfDetail.m_stDbConnInfo.m_szDbName, stDbConfDetail.m_stDbConnInfo.m_uiDbPort,
                    stDbConfDetail.m_stDbConnInfo.m_szDbCharSet);
    char connect[128];
	snprintf(connect,sizeof(connect),"dbname=%s hostaddr=%s user=%s password=%s port=%d",
			stDbConfDetail.m_stDbConnInfo.m_szDbName,
			stDbConfDetail.m_stDbConnInfo.m_szDbHost,
			stDbConfDetail.m_stDbConnInfo.m_szDbUser,
			stDbConfDetail.m_stDbConnInfo.m_szDbPwd,
			stDbConfDetail.m_stDbConnInfo.m_uiDbPort);
	try
	{
		pPgConn = new pqxx::connection(connect);
		if(!pPgConn->is_open())
		{
			LOG4_ERROR("Connection failed!options:%s",pPgConn->options().c_str());
			delete pPgConn;
			pPgConn = NULL;
			return 1;
		}
	}
	catch (const pqxx::sql_error &e)
	{
		LOG4_ERROR("SQL error: %s.Query was:%s",e.what(),e.query().c_str());
		return 2;
	}
	catch (const std::exception &e)
	{
		LOG4_ERROR("SQL error: %s.Query was:%s",e.what());
		return 1;
	}
	LOG4_TRACE("Connection succesful!options:%s",pPgConn->options().c_str());
	return 0;
}

int CmdPgOper::SyncQuery(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead,const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (NULL == pPgConn)
    {
        LOG4_ERROR("pPgConn is null!");
        Response(stMsgShell,oInMsgHead,ERR_QUERY, "pPgConn is null");
        return(ERR_QUERY);
    }
    std::string strSql;
    if (!CreateSql(oQuery, pPgConn, strSql))
    {
        LOG4_ERROR("Scrabble up sql error!");
        Response(stMsgShell,oInMsgHead,ERR_QUERY, strSql);
        return(ERR_QUERY);
    }
    LOG4_DEBUG("%s", strSql.c_str());
    int iResult = 0;
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	DataMem::MemRsp oRsp;
	oRsp.set_from(DataMem::MemRsp::FROM_DB);
	pqxx::result result;
	std::string strErr;
	{
		try
		{
			/* Create a transactional object. */
			pqxx::work W(*pPgConn);

			/* Execute SQL query */
			result = W.exec( strSql );
			W.commit();
		}
		catch (const pqxx::sql_error &e)
		{
			char sError[256];snprintf(sError,sizeof(sError)-1,"SQL error: %s.Query was:%s",e.what(),e.query().c_str());
			LOG4_ERROR(sError);
			strErr = sError;
			iResult = 2;
		}
		catch (const std::exception &e)
		{
			char sError[256];snprintf(sError,sizeof(sError)-1,"SQL error: %s.",e.what());
			LOG4_ERROR(sError);
			strErr = sError;
			iResult = 1;
		}
	}
	if (iResult > 0)
	{
		// 由于连接方面原因数据写失败，将失败数据节点返回给数据代理，等服务从故障中恢复后再由数据代理自动重试
		Response(stMsgShell,oInMsgHead,iResult, strErr);
		return iResult;
	}

	if (DataMem::MemOperate::DbOperate::SELECT == oQuery.db_operate().query_type())
	{
		DataMem::Record* pRecord = NULL;
		DataMem::Field* pField = NULL;
		// Results can be accessed and iterated again.  Even after the connection
		// has been closed.
//			for (auto row: result)
//			{
//			  std::cout << "Row: ";
//			  // Iterate over fields in a row.
//			  for (auto field: row) std::cout << field.c_str() << " ";
//			  std::cout << std::endl;
//			}
		uint32 uiDataLen = 0;
		int32 iRecordNum = 0;
		//字段值进行赋值
		oRsp.set_err_no(ERR_OK);
		oRsp.set_err_msg("success");
		for (auto row: result)
		{
			++iRecordNum;
			pRecord = oRsp.add_record_data();

			unsigned int uiFieldNum = row.size();
			for(unsigned int i = 0; i < uiFieldNum; ++i)
			{
				pField = pRecord->add_field_info();
				pField->set_col_value(row[i].c_str(),row[i].size());//row[1].as<int>()
				uiDataLen += row[i].size();
			}

			if (uiDataLen > 64000000)//64M
			{
				oRsp.set_curcount(iRecordNum);
				oRsp.set_totalcount(iRecordNum + 1);    // 表示未完
				if (Response(stMsgShell,oInMsgHead,oRsp))
				{
					oRsp.clear_record_data();
					uiDataLen = 0;
				}
				else
				{
					Response(stMsgShell,oInMsgHead,oRsp);
					return iResult;//(ERR_RESULTSET_EXCEED);
				}
			}

		}
		oRsp.set_curcount(iRecordNum);
		oRsp.set_totalcount(iRecordNum);

		Response(stMsgShell,oInMsgHead,oRsp);
		return(iResult);
	}
	else
	{
		Response(stMsgShell,oInMsgHead,oRsp);
		return(iResult);
	}
    return(iResult);
}

void CmdPgOper::CheckConnection()
{
    LOG4_TRACE("%s()",__FUNCTION__);
//    time_t lNowTime = time(NULL);
    //长连接。检查mapdbi中的连接实例有无超时的，超时的连接删除
//    std::map<std::string,tagConnection*>::iterator conn_iter;
//    for (conn_iter = m_mapDbiPool.begin(); conn_iter != m_mapDbiPool.end(); )
//    {
//        int iTimeOut = m_iConnectionTimeout;
//        if (conn_iter->second->iTimeout > 0)
//        {
//            iTimeOut = conn_iter->second->iTimeout;
//        }
//
//        int iDiffTime = lNowTime - conn_iter->second->ullBeatTime;  //求时差
//        if (iDiffTime > iTimeOut)
//        {
//            if (NULL != conn_iter->second)
//            {
//                delete conn_iter->second;
//                conn_iter->second = NULL;
//            }
//            m_mapDbiPool.erase(conn_iter);
//        }
//        else
//        {
//            ++conn_iter;
//        }
//    }
}

void CmdPgOper::Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,int iErrno, const std::string& strErrMsg)
{
    LOG4_TRACE("error %d: %s", iErrno, strErrMsg.c_str());
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    DataMem::MemRsp oRsp;
    oRsp.set_from(DataMem::MemRsp::FROM_DB);
    oRsp.set_err_no(iErrno);
    oRsp.set_err_msg(strErrMsg);
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
    }
}

bool CmdPgOper::Response(const net::tagMsgShell &stMsgShell,const MsgHead &oInMsgHead,const DataMem::MemRsp& oRsp)
{
    LOG4_TRACE("error %d: %s", oRsp.err_no(), oRsp.err_msg().c_str());
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgBody.set_body(oRsp.SerializeAsString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to tagMsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
        return(false);
    }
    return(true);
}

std::string CmdPgOper::GetFullTableName(const std::string& strTableName, uint64 uiFactor)
{
    char szFullTableName[128] = {0};
    std::string strDbName = m_oDbConf["table"][strTableName]("db_name");
    if (strDbName.size() > 0)
    {
    	int iTableNum = atoi(m_oDbConf["table"][strTableName]("table_num").c_str());
		if (1 == iTableNum)
		{
			//snprintf(szFullTableName, sizeof(szFullTableName), "%s.%s", strDbName.c_str(), strTableName.c_str());
			snprintf(szFullTableName, sizeof(szFullTableName), "%s",strTableName.c_str());
		}
		else
		{
			uint32 uiTableIndex = uiFactor % iTableNum;
			//snprintf(szFullTableName, sizeof(szFullTableName), "%s.%s_%02d", strDbName.c_str(), strTableName.c_str(), uiTableIndex);
			snprintf(szFullTableName, sizeof(szFullTableName), "%s_%02d", strTableName.c_str(), uiTableIndex);
			m_uiHash = uiTableIndex;
			m_uiDivisor = iTableNum;
		}
    }
    return(szFullTableName);
}

bool CmdPgOper::CreateSql(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql)
{
    LOG4_TRACE("%s()",__FUNCTION__);
    strSql.clear();
    if (oQuery.db_operate().query_type() == DataMem::MemOperate::DbOperate::CUSTOM)
	{
		if (oQuery.db_operate().fields_size() <= 0)
		{
			LOG4_ERROR("invalid fields_size(%d) for CUSTOM",oQuery.db_operate().fields_size());
			return false;
		}
		//自定义sql则不执行检查（只适用于开发者自己知道存储分表结构）
		if (oQuery.db_operate().fields(0).col_name().size() > 0)
		{
			strSql = oQuery.db_operate().fields(0).col_name();
		}
		else if (oQuery.db_operate().fields(0).col_value().size() > 0)
		{
			strSql = oQuery.db_operate().fields(0).col_value();
		}
		else
		{
			LOG4_ERROR("invalid oQuery.db_operate().fields(0) empty for CUSTOM");
			return false;
		}
		return true;
	}
    if (oQuery.db_operate().table_name().size() == 0)
    {
    	LOG4_ERROR("invalid oQuery.db_operate().table_name().size() == 0");
		return false;
    }
    bool bResult = false;
    switch (oQuery.db_operate().query_type())
    {
        case DataMem::MemOperate::DbOperate::SELECT:
        {
        	if (oQuery.db_operate().fields_size() <= 0)
			{
				LOG4_ERROR("invalid fields_size(%d) for SELECT",oQuery.db_operate().fields_size());
				return false;
			}
            bResult = CreateSelect(oQuery, strSql);
            break;
        }
        case DataMem::MemOperate::DbOperate::INSERT:
        case DataMem::MemOperate::DbOperate::INSERT_IGNORE:
        case DataMem::MemOperate::DbOperate::REPLACE:
        {
        	if (oQuery.db_operate().fields_size() <= 0)
			{
				LOG4_ERROR("invalid fields_size(%d) for UPDATE",oQuery.db_operate().fields_size());
				return false;
			}
            bResult = CreateInsert(oQuery, pPgConn, strSql);
            break;
        }
        case DataMem::MemOperate::DbOperate::UPDATE:
        {
        	if (oQuery.db_operate().fields_size() <= 0)
			{
				LOG4_ERROR("invalid fields_size(%d) for UPDATE",oQuery.db_operate().fields_size());
				return false;
			}
            bResult = CreateUpdate(oQuery, pPgConn, strSql);
            break;
        }
        case DataMem::MemOperate::DbOperate::DELETE:
        {
            bResult = CreateDelete(oQuery, strSql);
            break;
        }
        case DataMem::MemOperate::DbOperate::E_QUERY_TYPE_UNKNONW:
        {
            LOG4_ERROR("invalid query_type(%d)",oQuery.db_operate().query_type());
            return(false);
        }
        default:
        {
            LOG4_ERROR("invalid query_type(%d)",oQuery.db_operate().query_type());
            return(false);
        }
    }

    if (oQuery.db_operate().conditions_size() > 0)
    {
        std::string strCondition;
        bResult = CreateConditionGroup(oQuery, pPgConn, strCondition);
        if (bResult)
        {
            strSql += std::string(" WHERE ") + strCondition;
        }
    }

    if (oQuery.db_operate().groupby_col_size() > 0)
    {
        std::string strGroupBy;
        bResult = CreateGroupBy(oQuery, strGroupBy);
        if (bResult)
        {
            strSql += std::string(" GROUP BY ") + strGroupBy;
        }
    }

    if (oQuery.db_operate().orderby_col_size() > 0)
    {
        std::string strOrderBy;
        bResult = CreateOrderBy(oQuery, strOrderBy);
        if (bResult)
        {
            strSql += std::string(" ORDER BY ") + strOrderBy;
        }
    }

    if (oQuery.db_operate().limit() > 0)
    {
        std::string strLimit;
        bResult = CreateLimit(oQuery, strLimit);
        if (bResult)
        {
            strSql += std::string(" LIMIT ") + strLimit;
        }
    }

    return(bResult);
}

bool CmdPgOper::CreateSelect(const DataMem::MemOperate& oQuery, std::string& strSql)
{
    strSql = "SELECT ";
    for (int i = 0; i < oQuery.db_operate().fields_size(); ++i)
    {
        if (!CheckColName(oQuery.db_operate().fields(i).col_name()))
        {
            LOG4_ERROR("invalid col_name \"%s\".", oQuery.db_operate().fields(i).col_name().c_str());
            return(false);
        }
        if (i == 0)
        {
            if (oQuery.db_operate().fields(i).col_as().size() > 0)
            {
                strSql += oQuery.db_operate().fields(i).col_name() + std::string(" AS ") + oQuery.db_operate().fields(i).col_as();
            }
            else
            {
                strSql += oQuery.db_operate().fields(i).col_name();
            }
        }
        else
        {
            if (oQuery.db_operate().fields(i).col_as().size() > 0)
            {
                strSql += std::string(",") + oQuery.db_operate().fields(i).col_name() + std::string(" AS ") + oQuery.db_operate().fields(i).col_as();
            }
            else
            {
                strSql += std::string(",") + oQuery.db_operate().fields(i).col_name();
            }
        }
    }

    std::string strTableName = GetFullTableName(oQuery.db_operate().table_name(), oQuery.db_operate().mod_factor());
    if (strTableName.empty())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"dbname_table is NULL");
        return false;
    }

    strSql += std::string(" FROM ") + strTableName;

    return true;
}

bool CmdPgOper::CreateInsert(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    strSql = "";
    if (DataMem::MemOperate::DbOperate::INSERT == oQuery.db_operate().query_type())
    {
        strSql = "INSERT INTO ";
    }
    else if (DataMem::MemOperate::DbOperate::INSERT_IGNORE == oQuery.db_operate().query_type())
    {
        strSql = "INSERT IGNORE INTO ";
    }
    else if (DataMem::MemOperate::DbOperate::REPLACE == oQuery.db_operate().query_type())
    {
        strSql = "REPLACE INTO ";
    }

    std::string strTableName = GetFullTableName(oQuery.db_operate().table_name(), oQuery.db_operate().mod_factor());
    if (strTableName.empty())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"dbname_tbname is null");
        return false;
    }

    strSql += strTableName;

    for (int i = 0; i < oQuery.db_operate().fields_size(); ++i)
    {
        if (!CheckColName(oQuery.db_operate().fields(i).col_name()))
        {
            return(false);
        }
        if (i == 0)
        {
            strSql += std::string("(") + oQuery.db_operate().fields(i).col_name();
        }
        else
        {
            strSql += std::string(",") + oQuery.db_operate().fields(i).col_name();
        }
    }
    strSql += std::string(") ");

    for (int i = 0; i < oQuery.db_operate().fields_size(); ++i)
    {
        if (i == 0)
        {
            if (DataMem::STRING == oQuery.db_operate().fields(i).col_type())
            {
            	m_strColValue = pPgConn->esc(oQuery.db_operate().fields(i).col_value());
                strSql += std::string("VALUES('") + std::string(m_strColValue) + std::string("'");
            }
            else
            {
                for (unsigned int j = 0; j < oQuery.db_operate().fields(i).col_value().size(); ++j)
                {
                    if (oQuery.db_operate().fields(i).col_value()[j] >= '0' || oQuery.db_operate().fields(i).col_value()[j] <= '9'
                        || oQuery.db_operate().fields(i).col_value()[j] == '.')
                    {
                        ;
                    }
                    else
                    {
                        return(false);
                    }
                }
                strSql += std::string("VALUES(") + oQuery.db_operate().fields(i).col_value();
            }
        }
        else
        {
            if (DataMem::STRING == oQuery.db_operate().fields(i).col_type())
            {
            	m_strColValue = pPgConn->esc(oQuery.db_operate().fields(i).col_value());
                strSql += std::string(",'") + std::string(m_strColValue) + std::string("'");
            }
            else
            {
                for (unsigned int j = 0; j < oQuery.db_operate().fields(i).col_value().size(); ++j)
                {
                    if (oQuery.db_operate().fields(i).col_value()[j] >= '0' || oQuery.db_operate().fields(i).col_value()[j] <= '9'
                        || oQuery.db_operate().fields(i).col_value()[j] == '.')
                    {
                        ;
                    }
                    else
                    {
                        return(false);
                    }
                }
                strSql += std::string(",") + oQuery.db_operate().fields(i).col_value();
            }
        }
    }
    strSql += std::string(")");

    return true;
}

bool CmdPgOper::CreateUpdate(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strSql)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    strSql = "UPDATE ";
    std::string strTableName = GetFullTableName(oQuery.db_operate().table_name(), oQuery.db_operate().mod_factor());
    if (strTableName.empty())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"dbname_tbname is null");
        return false;
    }

    strSql += strTableName;

    for (int i = 0; i < oQuery.db_operate().fields_size(); ++i)
    {
        if (!CheckColName(oQuery.db_operate().fields(i).col_name()))
        {
            return(false);
        }
        if (i == 0)
        {
            if (DataMem::STRING == oQuery.db_operate().fields(i).col_type())
            {
            	m_strColValue = pPgConn->esc(oQuery.db_operate().fields(i).col_value());
                strSql += std::string(" SET ") + oQuery.db_operate().fields(i).col_name() + std::string("=");
                strSql += std::string("'") + std::string(m_strColValue) + std::string("'");
            }
            else
            {
                for (unsigned int j = 0; j < oQuery.db_operate().fields(i).col_value().size(); ++j)
                {
                    if (oQuery.db_operate().fields(i).col_value()[j] >= '0' || oQuery.db_operate().fields(i).col_value()[j] <= '9'
                        || oQuery.db_operate().fields(i).col_value()[j] == '.')
                    {
                        ;
                    }
                    else
                    {
                        return(false);
                    }
                }
                strSql += std::string(" SET ") + oQuery.db_operate().fields(i).col_name()
                    + std::string("=") + oQuery.db_operate().fields(i).col_value();
            }
        }
        else
        {
            if (DataMem::STRING == oQuery.db_operate().fields(i).col_type())
            {
            	m_strColValue = pPgConn->esc(oQuery.db_operate().fields(i).col_value());
                strSql += std::string(", ") + oQuery.db_operate().fields(i).col_name() + std::string("=");
                strSql += std::string("'") + std::string(m_strColValue) + std::string("'");
            }
            else
            {
                for (unsigned int j = 0; j < oQuery.db_operate().fields(i).col_value().size(); ++j)
                {
                    if (oQuery.db_operate().fields(i).col_value()[j] >= '0' || oQuery.db_operate().fields(i).col_value()[j] <= '9'
                        || oQuery.db_operate().fields(i).col_value()[j] == '.')
                    {
                        ;
                    }
                    else
                    {
                        return(false);
                    }
                }
                strSql += std::string(", ") + oQuery.db_operate().fields(i).col_name()
                    + std::string("=") + oQuery.db_operate().fields(i).col_value();
            }
        }
    }

    return true;
}

bool CmdPgOper::CreateDelete(const DataMem::MemOperate& oQuery, std::string& strSql)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    strSql = "DELETE FROM ";
    std::string strTableName = GetFullTableName(oQuery.db_operate().table_name(), oQuery.db_operate().mod_factor());
    if (strTableName.empty())
    {
        LOG4CPLUS_ERROR_FMT(GetLogger(),"dbname_tbname is null");
        return false;
    }

    strSql += strTableName;

    return true;
}

bool CmdPgOper::CreateCondition(const DataMem::MemOperate::DbOperate::Condition& oCondition, pqxx::connection* pPgConn, std::string& strCondition)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    if (!CheckColName(oCondition.col_name()))
    {
        return(false);
    }
    strCondition = oCondition.col_name();
    switch (oCondition.relation())
    {
    case DataMem::MemOperate::DbOperate::Condition::EQ:
        strCondition += std::string("=");
        break;
    case DataMem::MemOperate::DbOperate::Condition::NE:
        strCondition += std::string("<>");
        break;
    case DataMem::MemOperate::DbOperate::Condition::GT:
        strCondition += std::string(">");
        break;
    case DataMem::MemOperate::DbOperate::Condition::LT:
        strCondition += std::string("<");
        break;
    case DataMem::MemOperate::DbOperate::Condition::GE:
        strCondition += std::string(">=");
        break;
    case DataMem::MemOperate::DbOperate::Condition::LE:
        strCondition += std::string("<=");
        break;
    case DataMem::MemOperate::DbOperate::Condition::LIKE:
        strCondition += std::string(" LIKE ");
        break;
    case DataMem::MemOperate::DbOperate::Condition::IN:
        strCondition += std::string(" IN (");
        break;
    default:
        break;
    }

    if (oCondition.col_name_right().size() > 0)
    {
        if (!CheckColName(oCondition.col_name_right()))
        {
            return(false);
        }
        strCondition += oCondition.col_name_right();
    }
    else
    {
        for (int i = 0; i < oCondition.col_values_size(); ++i)
        {
            if (i > 0)
            {
                strCondition += std::string(",");
            }

            if (DataMem::STRING == oCondition.col_type())
            {
            	m_strColValue = pPgConn->esc(oCondition.col_values(i));
                strCondition += std::string("'") + std::string(m_strColValue) + std::string("'");
            }
            else
            {
                for (unsigned int j = 0; j < oCondition.col_values(i).size(); ++j)
                {
                    if (oCondition.col_values(i)[j] >= '0' || oCondition.col_values(i)[j] <= '9'
                        || oCondition.col_values(i)[j] == '.')
                    {
                        ;
                    }
                    else
                    {
                        return(false);
                    }
                }
                strCondition += oCondition.col_values(i);
            }
        }

        if (DataMem::MemOperate::DbOperate::Condition::IN == oCondition.relation())
        {
            strCondition += std::string(")");
        }
    }

    return true;
}

bool CmdPgOper::CreateConditionGroup(const DataMem::MemOperate& oQuery, pqxx::connection* pPgConn, std::string& strCondition)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    bool bResult = false;
    for (int i = 0; i < oQuery.db_operate().conditions_size(); ++i)
    {
        if (i > 0)
        {
            if (oQuery.db_operate().group_relation() > 0)
            {
                if (DataMem::MemOperate::DbOperate::ConditionGroup::OR == oQuery.db_operate().group_relation())
                {
                    strCondition += std::string(" OR ");
                }
                else
                {
                    strCondition += std::string(" AND ");
                }
            }
            else
            {
                strCondition += std::string(" AND ");
            }
        }
        if (oQuery.db_operate().conditions_size() > 1)
        {
            strCondition += std::string("(");
        }
        for (int j = 0; j < oQuery.db_operate().conditions(i).condition_size(); ++j)
        {
            std::string strRelation;
            std::string strOneCondition;
            if (DataMem::MemOperate::DbOperate::ConditionGroup::OR == oQuery.db_operate().conditions(i).relation())
            {
                strRelation = " OR ";
            }
            else
            {
                strRelation = " AND ";
            }
            bResult = CreateCondition(oQuery.db_operate().conditions(i).condition(j), pPgConn, strOneCondition);
            if (bResult)
            {
                if (j > 0)
                {
                    strCondition += strRelation;
                }
                strCondition += strOneCondition;
            }
            else
            {
                return(bResult);
            }
        }
        if (oQuery.db_operate().conditions_size() > 1)
        {
            strCondition += std::string(")");
        }
    }

    return true;
}

bool CmdPgOper::CreateGroupBy(const DataMem::MemOperate& oQuery, std::string& strGroupBy)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    strGroupBy = "";
    for (int i = 0; i < oQuery.db_operate().groupby_col_size(); ++i)
    {
        if (!CheckColName(oQuery.db_operate().groupby_col(i)))
        {
            return(false);
        }
        if (i == 0)
        {
            strGroupBy += oQuery.db_operate().groupby_col(i);
        }
        else
        {
            strGroupBy += std::string(",") + oQuery.db_operate().groupby_col(i);
        }
    }

    return true;
}

bool CmdPgOper::CreateOrderBy(const DataMem::MemOperate& oQuery, std::string& strOrderBy)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    strOrderBy = "";
    for (int i = 0; i < oQuery.db_operate().orderby_col_size(); ++i)
    {
        if (!CheckColName(oQuery.db_operate().orderby_col(i).col_name()))
        {
            return(false);
        }
        if (i == 0)
        {
            if (DataMem::MemOperate::DbOperate::OrderBy::DESC == oQuery.db_operate().orderby_col(i).relation())
            {
                strOrderBy += oQuery.db_operate().orderby_col(i).col_name() + std::string(" DESC");
            }
            else
            {
                strOrderBy += oQuery.db_operate().orderby_col(i).col_name() + std::string(" ASC");
            }
        }
        else
        {
            if (DataMem::MemOperate::DbOperate::OrderBy::DESC == oQuery.db_operate().orderby_col(i).relation())
            {
                strOrderBy += std::string(",") + oQuery.db_operate().orderby_col(i).col_name() + std::string(" DESC");
            }
            else
            {
                strOrderBy += std::string(",") + oQuery.db_operate().orderby_col(i).col_name() + std::string(" ASC");
            }
        }
    }

    return true;
}

bool CmdPgOper::CreateLimit(const DataMem::MemOperate& oQuery, std::string& strLimit)
{
    LOG4_TRACE("%s()",__FUNCTION__);

    char szLimit[16] = {0};
    if (oQuery.db_operate().limit_from() > 0 && oQuery.db_operate().limit() > 0)
    {
        snprintf(szLimit, sizeof(szLimit), "%u,%u", oQuery.db_operate().limit_from(), oQuery.db_operate().limit());
        strLimit = szLimit;
    }
    else
    {
        snprintf(szLimit, sizeof(szLimit), "%u", oQuery.db_operate().limit());
        strLimit = szLimit;
    }
    return true;
}

bool CmdPgOper::CheckColName(const std::string& strColName)
{
    std::string strUpperColName;
    for (unsigned int i = 0; i < strColName.size(); ++i)
    {
//        if (strColName[i] == '\'' || strColName[i] == '"' || strColName[i] == '\\'
//            || strColName[i] == ';' || strColName[i] == '*' || strColName[i] == ' ')
//        {
//            return(false);
//        }
        if (strColName[i] >= 'a' && strColName[i] <= 'z')
        {
            strUpperColName[i] = strColName[i] - 32;
        }
        else
        {
            strUpperColName[i] = strColName[i];
        }
    }
    if (strUpperColName == "AND" || strUpperColName == "OR"
        || strUpperColName == "UPDATE" || strUpperColName == "DELETE"
        || strUpperColName == "UNION" || strUpperColName == "AS"
        || strUpperColName == "CHANGE" || strUpperColName == "SET"
        || strUpperColName == "TRUNCATE" || strUpperColName == "DESC")
    {
        return(false);
    }
    return true;
}


} /* namespace net */
