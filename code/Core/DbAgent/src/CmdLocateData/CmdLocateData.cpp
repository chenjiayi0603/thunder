/*******************************************************************************
 * Project:  DbAgent
 * @file     CmdLocateData.cpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月18日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdLocateData.hpp"

#ifdef __cplusplus
extern "C" {
#endif
    net::Cmd* create()
    {
        net::Cmd* pCmd = new net::CmdLocateData();
        return(pCmd);
    }
#ifdef __cplusplus
}
#endif

namespace net
{

CmdLocateData::CmdLocateData()
{
}

CmdLocateData::~CmdLocateData()
{
}

bool CmdLocateData::Init()
{
    //配置文件路径查找
    std::string strConfFile = GetConfigPath() + std::string("Agent/CmdDbOper.json");
    LOG4_DEBUG("CONF FILE = %s.", strConfFile.c_str());

    std::ifstream fin(strConfFile.c_str());
    if (fin.good())
    {
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        fin.close();
        if (m_oDbConf.Parse(ssContent.str()))
        {
            LOG4_DEBUG("m_oDbConf pasre OK");
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
                                        util::CJsonObject* pInstanceGroup
                                            = new util::CJsonObject(m_oDbConf["cluster"][m_oDbConf["data_type"](i)][m_oDbConf["section_factor"](j)][szFactorSectionKey]);
                                        LOG4_DEBUG("%s : %s", szInstanceGroup, pInstanceGroup->ToString().c_str());
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
                                LOG4_DEBUG("%s factor size %u", szInstanceGroup, setFactorSection.size());
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

    return(true);
}

bool CmdLocateData::AnyMessage(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
    DataMem::MemOperate oQuery;
    if (!oQuery.ParseFromString(oInMsgBody.body()))
    {
        LOG4_ERROR("DataMem::MemOperate ParseFromString(oInMsgBody.body()) error!");
        Response(stMsgShell, oInMsgHead, ERR_PARASE_PROTOBUF, "DataMem::MemOperate ParseFromString(oInMsgBody.body()) error!");
        return(false);
    }

    char szFactor[32] = {0};
    int32 iDataType = 0;
    int32 iSectionFactorType = 0;
    m_oDbConf["table"][oQuery.db_operate().table_name()].Get("data_type", iDataType);
    m_oDbConf["table"][oQuery.db_operate().table_name()].Get("section_factor", iSectionFactorType);
    snprintf(szFactor, 32, "%d:%d", iDataType, iSectionFactorType);
    std::map<std::string, std::set<uint32> >::const_iterator c_factor_iter =  m_mapFactorSection.find(szFactor);
    if (c_factor_iter == m_mapFactorSection.end())
    {
        LOG4_ERROR("no db config found for data_type %d section_factor_type %d", iDataType, iSectionFactorType);
        Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, "no db config found for oMemOperate.cluster_info()!");
        return(false);
    }
    else
    {
        std::set<uint32>::const_iterator c_section_iter = c_factor_iter->second.lower_bound(oQuery.section_factor());
        if (c_section_iter == c_factor_iter->second.end())
        {
            LOG4_ERROR("no factor_section config found for data_type %u section_factor_type %u section_factor %u",
                            iDataType, iSectionFactorType, oQuery.section_factor());
            Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, "no db config for the cluster info!");
            return(false);
        }
        else
        {
            snprintf(szFactor, 32, "%u:%u:%u", iDataType, iSectionFactorType, *c_section_iter);
            std::map<std::string, util::CJsonObject*>::iterator conf_iter = m_mapDbInstanceInfo.find(szFactor);
            if (conf_iter == m_mapDbInstanceInfo.end())
            {
                LOG4_ERROR("no db config found for %s which consist of data_type %u section_factor_type %u section_factor %u",
                                szFactor, iDataType, iSectionFactorType, oQuery.section_factor());
                Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, "no db config for the cluster info!");
                return(false);
            }
            std::string strDbName = m_oDbConf["table"][oQuery.db_operate().table_name()]("db_name");
            std::string strInstance;
            if (!conf_iter->second->Get(strDbName, strInstance))
            {
                Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, "no db instance config for db name!");
                LOG4_ERROR("no db instance config for strDbName \"%s\"!", strDbName.c_str());
                return(false);
            }


            util::CJsonObject& dbGroupConf = m_oDbConf["db_group"];
            util::CJsonObject& dbInstanceConf = dbGroupConf[strInstance];
            std::string strUseGroup = dbInstanceConf("use_group");
			util::CJsonObject& oGroupHostConf = dbInstanceConf[strUseGroup];
			int nArraySize = oGroupHostConf.GetArraySize();
			std::string strMasterIdentify;
			std::string strSlaveIdentify;
			if (nArraySize > 0)//cluster集群
			{
				strMasterIdentify = oGroupHostConf[0].ToString();//只返回第一个节点
				strSlaveIdentify = oGroupHostConf[0].ToString();
			}
			else//主从
			{
				strMasterIdentify = dbInstanceConf("master_host");
				strSlaveIdentify = dbInstanceConf("slave_host");
			}

            std::string strTableName = GetFullTableName(oQuery.db_operate().table_name(), oQuery.db_operate().mod_factor());
            if (strTableName.empty())
            {
                LOG4CPLUS_ERROR_FMT(GetLogger(),"dbname_table is NULL");
                return false;
            }

            MsgHead oOutMsgHead = oInMsgHead;
            MsgBody oOutMsgBody;
            util::CJsonObject oRspJson;
            oRspJson.Add("code", ERR_OK);
            oRspJson.Add("msg", "successfully");
            oRspJson.Add("db_node", util::CJsonObject("{}"));
            oRspJson["db_node"].Add("master", strMasterIdentify);
            oRspJson["db_node"].Add("slave", strSlaveIdentify);
            oRspJson["db_node"].Add("table", strTableName);
            oOutMsgBody.set_body(oRspJson.ToFormattedString());
            oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
            oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
            if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
            {
                LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
            }
            return(true);
        }
    }
}

std::string CmdLocateData::GetFullTableName(const std::string& strTableName, uint32 uiFactor)
{
    char szFullTableName[128] = {0};
    std::string strDbName = m_oDbConf["table"][strTableName]("db_name");
    int iTableNum = atoi(m_oDbConf["table"][strTableName]("table_num").c_str());
    if (1 == iTableNum)
    {
        snprintf(szFullTableName, sizeof(szFullTableName), "%s.%s", strDbName.c_str(), strTableName.c_str());
    }
    else
    {
        uint32 uiTableIndex = uiFactor % iTableNum;
        snprintf(szFullTableName, sizeof(szFullTableName), "%s.%s_%02d", strDbName.c_str(), strTableName.c_str(), uiTableIndex);
    }
    return(szFullTableName);
}

void CmdLocateData::Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                int iErrno, const std::string& strErrMsg)
{
    LOG4_DEBUG("%d: %s", iErrno, strErrMsg.c_str());
    MsgHead oOutMsgHead = oInMsgHead;
    MsgBody oOutMsgBody;
    util::CJsonObject oRspJson;
    oRspJson.Add("code", iErrno);
    oRspJson.Add("msg", strErrMsg);
    oOutMsgBody.set_body(oRspJson.ToFormattedString());
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    if (!GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody))
    {
        LOG4_ERROR("send to MsgShell(fd %d, seq %u) error!", stMsgShell.iFd, stMsgShell.ulSeq);
    }
}

} /* namespace net */
