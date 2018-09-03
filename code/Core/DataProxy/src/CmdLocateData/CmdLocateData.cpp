/*******************************************************************************
 * Project:  DataProxyServer
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
    : m_pRedisNodeSession(NULL), pStepDbDistribute(NULL)
{
}

CmdLocateData::~CmdLocateData()
{
}

bool CmdLocateData::Init()
{
    if (ReadDataProxyConf() && ReadTableRelation())
    {
        return(true);
    }
    return(false);
}

bool CmdLocateData::ReadDataProxyConf()
{
    util::CJsonObject oConfJson;
    //配置文件路径查找
    std::string strConfFile = GetConfigPath() + std::string("Proxy/CmdDataProxy.json");
    LOG4_DEBUG("CONF FILE = %s.", strConfFile.c_str());
    std::ifstream fin(strConfFile.c_str());
    if (fin.good())
    {
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        fin.close();
        if (oConfJson.Parse(ssContent.str()))
        {
            LOG4_DEBUG("oConfJson pasre OK");
            char szSessionId[64] = {0};
            char szFactorSectionKey[32] = {0};
            int32 iVirtualNodeNum = 200;
            int32 iHashAlgorithm = 0;
            uint32 uiDataType = 0;
            uint32 uiFactor = 0;
            uint32 uiFactorSection = 0;
            std::string strRedisNode;
            std::string strMaster;
            std::string strSlave;
            oConfJson.Get("hash_algorithm", iHashAlgorithm);
            oConfJson.Get("virtual_node_num", iVirtualNodeNum);
            if (oConfJson["cluster"].IsEmpty())
            {
                LOG4_ERROR("oConfJson[\"cluster\"] is empty!");
                return(false);
            }
            std::string strGroupUse;
			if (!oConfJson.Get("group_use",strGroupUse))
			{
				strGroupUse = "redis_group";
			}
			LOG4_INFO("server start to use data group:%s!",strGroupUse.c_str());
            if (oConfJson[strGroupUse].IsEmpty())
            {
                LOG4_ERROR("oConfJson[\"redis_group\"] is empty!");
                return(false);
            }
            for (int i = 0; i < oConfJson["data_type"].GetArraySize(); ++i)
            {
                if (oConfJson["data_type_enum"].Get(oConfJson["data_type"](i), uiDataType))
                {
                    for (int j = 0; j < oConfJson["section_factor"].GetArraySize(); ++j)
                    {
                        if (oConfJson["section_factor_enum"].Get(oConfJson["section_factor"](j), uiFactor))
                        {
                            if (oConfJson["factor_section"][oConfJson["section_factor"](j)].IsArray())
                            {
                                std::set<uint32> setFactorSection;
                                for (int k = 0; k < oConfJson["factor_section"][oConfJson["section_factor"](j)].GetArraySize(); ++k)
                                {
                                    if (oConfJson["factor_section"][oConfJson["section_factor"](j)].Get(k, uiFactorSection))
                                    {
                                        snprintf(szSessionId, sizeof(szSessionId), "%u:%u:%u", uiDataType, uiFactor, uiFactorSection);
                                        snprintf(szFactorSectionKey, sizeof(szFactorSectionKey), "LE_%u", uiFactorSection);
                                        setFactorSection.insert(uiFactorSection);
                                        if (oConfJson["cluster"][oConfJson["data_type"](i)][oConfJson["section_factor"](j)][szFactorSectionKey].IsArray())
                                        {
                                            for (int l = 0; l < oConfJson["cluster"][oConfJson["data_type"](i)][oConfJson["section_factor"](j)][szFactorSectionKey].GetArraySize(); ++l)
                                            {
                                                SessionRedisNode* pNodeSession = (SessionRedisNode*)GetSession(szSessionId, "net::SessionRedisNode");
                                                if (pNodeSession == NULL)
                                                {
                                                    pNodeSession = new SessionRedisNode(szSessionId, iHashAlgorithm, iVirtualNodeNum,strGroupUse);
//                                                    LOG4_DEBUG("register node session %s", szSessionId);
                                                    RegisterCallback(pNodeSession);
                                                }
                                                strRedisNode = oConfJson["cluster"][oConfJson["data_type"](i)][oConfJson["section_factor"](j)][szFactorSectionKey](l);
                                                if (oConfJson[strGroupUse][strRedisNode].Get("master", strMaster)
                                                                && oConfJson[strGroupUse][strRedisNode].Get("slave", strSlave))
                                                {
//                                                    LOG4_DEBUG("Add node %s[%s, %s] to %s!",
//                                                                    strRedisNode.c_str(), strMaster.c_str(), strSlave.c_str(), szSessionId);
                                                    pNodeSession->AddRedisNode(strRedisNode, strMaster, strSlave);
                                                }
                                            }
                                        }
                                        else
                                        {
                                            LOG4_ERROR("oConfJson[\"cluster\"][\"%s\"][\"%s\"][\"%s\"] is not a json array!",
                                                            oConfJson["data_type"](i).c_str(), oConfJson["section_factor"](j).c_str(), szFactorSectionKey);
                                            continue;
                                        }
                                    }
                                    else
                                    {
                                        LOG4_ERROR("oConfJson[\"factor_section\"][\"%s\"](%d) is not exist!",
                                                        oConfJson["section_factor"](j).c_str(), k);
                                        continue;
                                    }
                                }
                                snprintf(szSessionId, sizeof(szSessionId), "%u:%u", uiDataType, uiFactor);
//                                LOG4_DEBUG("add data_type and factor [%s] to m_mapFactorSection", szSessionId);
                                m_mapFactorSection.insert(std::pair<std::string, std::set<uint32> >(szSessionId, setFactorSection));
                            }
                            else
                            {
                                LOG4_ERROR("oConfJson[\"factor_section\"][\"%s\"] is not a json array!",
                                                oConfJson["section_factor"](j).c_str());
                                continue;
                            }
                        }
                        else
                        {
                            LOG4_ERROR("missing %s in oConfJson[\"section_factor_enum\"]", oConfJson["section_factor"](j).c_str());
                            continue;
                        }
                    }
                }
                else
                {
                    LOG4_ERROR("missing %s in oConfJson[\"data_type_enum\"]", oConfJson["data_type"](i).c_str());
                    continue;
                }
            }
        }
        else
        {
            LOG4_ERROR("oConfJson pasre error");
            return false;
        }
    }
    else
    {
        //配置信息流读取失败
        LOG4_ERROR("Open conf (%s) error!",strConfFile.c_str());
        return false;
    }

    return true;
}

bool CmdLocateData::ReadTableRelation()
{
    //配置文件路径查找
    std::string strConfFile = GetConfigPath() + std::string("Proxy/CmdDataProxyTableRelative.json");
    LOG4_DEBUG("CONF FILE = %s.", strConfFile.c_str());
    std::ifstream fin(strConfFile.c_str());
    if (fin.good())
    {
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        fin.close();
        if (m_oJsonTableRelative.Parse(ssContent.str()))
        {
            return(true);
        }
        else
        {
            LOG4_ERROR("m_oJsonTableRelative parse error!");
        }
    }
    LOG4_ERROR("failed to open %s!", strConfFile.c_str());
    return(false);
}

bool CmdLocateData::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    DataMem::MemOperate oMemOperate;
    if (oMemOperate.ParseFromString(oInMsgBody.body()))
    {
        if (!oMemOperate.has_db_operate())
        {
            return(RedisOnly(stMsgShell, oInMsgHead, oMemOperate));
        }
        if (!oMemOperate.has_redis_operate())
        {
            return(DbOnly(stMsgShell, oInMsgHead, oInMsgBody));
        }
        if (oMemOperate.has_db_operate() && oMemOperate.has_redis_operate())
        {
            return(RedisAndDb(stMsgShell, oInMsgHead, oInMsgBody));
        }
        LOG4_ERROR("%d: %s", ERR_INCOMPLET_DATAPROXY_DATA, "neighter redis_operate nor db_operate was exist!");
        Response(stMsgShell, oInMsgHead, ERR_INCOMPLET_DATAPROXY_DATA, "neighter redis_operate nor db_operate was exist!");
        return(false);
    }
    else
    {
        LOG4_ERROR("%d: %s", ERR_PARASE_PROTOBUF, "failed to parse DataMem::MemOperate from oInMsgBody.body()!");
        Response(stMsgShell, oInMsgHead, ERR_PARASE_PROTOBUF, "failed to parse DataMem::MemOperate from oInMsgBody.body()!");
        return(false);
    }
}

bool CmdLocateData::RedisOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate)
{
    char szRedisDataPurpose[16] = {0};
    char szFactor[32] = {0};
    char szErrMsg[128] = {0};
    int32 iDataType = 0;
    int32 iSectionFactorType = 0;
    snprintf(szRedisDataPurpose, 16, "%d", oMemOperate.redis_operate().data_purpose());
    m_oJsonTableRelative["redis_struct"][szRedisDataPurpose].Get("data_type", iDataType);
    m_oJsonTableRelative["redis_struct"][szRedisDataPurpose].Get("section_factor", iSectionFactorType);
    snprintf(szFactor, 32, "%d:%d", iDataType, iSectionFactorType);
    std::map<std::string, std::set<uint32> >::const_iterator c_factor_iter =  m_mapFactorSection.find(szFactor);
    if (c_factor_iter == m_mapFactorSection.end())
    {
        snprintf(szErrMsg, sizeof(szErrMsg), "no redis node session for data_type %u and section_factor_type %u!",
                        iDataType, iSectionFactorType);
        LOG4_ERROR("%d: %s", ERR_LACK_CLUSTER_INFO, szErrMsg);
        Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, szErrMsg);
        return(false);
    }
    else
    {
        std::set<uint32>::const_iterator c_section_iter = c_factor_iter->second.lower_bound(oMemOperate.section_factor());
        if (c_section_iter == c_factor_iter->second.end())
        {
            snprintf(szErrMsg, sizeof(szErrMsg), "no redis node found for data_type %u and section_factor_type %u and factor %u!",
                            iDataType, iSectionFactorType, oMemOperate.section_factor());
            LOG4_ERROR("%d: %s oMemOperate(%s)", ERR_LACK_CLUSTER_INFO, szErrMsg,oMemOperate.DebugString().c_str());
            Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, szErrMsg);
            return(false);
        }
        else
        {
            std::string strMasterIdentify;
            std::string strSlaveIdentify;
            snprintf(szFactor, 32, "%u:%u:%u", iDataType, iSectionFactorType, *c_section_iter);
            m_pRedisNodeSession = (SessionRedisNode*)GetSession(std::string(szFactor), "net::SessionRedisNode");
            if (oMemOperate.redis_operate().hash_key().size() > 0)
            {
                m_pRedisNodeSession->GetRedisNode(oMemOperate.redis_operate().hash_key(), strMasterIdentify, strSlaveIdentify);
            }
            else
            {
                m_pRedisNodeSession->GetRedisNode(oMemOperate.redis_operate().key_name(), strMasterIdentify, strSlaveIdentify);
            }
            MsgHead oOutMsgHead = oInMsgHead;
            MsgBody oOutMsgBody;
            util::CJsonObject oRspJson;
            oRspJson.Add("code", ERR_OK);
            oRspJson.Add("msg", "successfully");
            oRspJson.Add("redis_node", util::CJsonObject("{}"));
            oRspJson["redis_node"].Add("master", strMasterIdentify);
            oRspJson["redis_node"].Add("slave", strSlaveIdentify);
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

bool CmdLocateData::DbOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    pStepDbDistribute = new StepDbDistribute(stMsgShell, oInMsgHead, oInMsgBody);
    if (NULL == pStepDbDistribute)
    {
        Response(stMsgShell, oInMsgHead, ERR_NEW,
                        "malloc space for pStepDbDistribute error!");
        return(false);
    }

    if (RegisterCallback(pStepDbDistribute))
    {
        if (STATUS_CMD_RUNNING == pStepDbDistribute->Emit(ERR_OK))
        {
            return(true);
        }
        else
        {
            DeleteCallback(pStepDbDistribute);
            return(false);
        }
    }
    return(false);
}

bool CmdLocateData::RedisAndDb(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody)
{
    DataMem::MemOperate oMemOperate;
    oMemOperate.ParseFromString(oInMsgBody.body());
    char szRedisDataPurpose[16] = {0};
    char szFactor[32] = {0};
    char szErrMsg[128] = {0};
    int32 iDataType = 0;
    int32 iSectionFactorType = 0;
    snprintf(szRedisDataPurpose, 16, "%d", oMemOperate.redis_operate().data_purpose());
    m_oJsonTableRelative["redis_struct"][szRedisDataPurpose].Get("data_type", iDataType);
    m_oJsonTableRelative["redis_struct"][szRedisDataPurpose].Get("section_factor", iSectionFactorType);
    snprintf(szFactor, 32, "%d:%d", iDataType, iSectionFactorType);
    std::map<std::string, std::set<uint32> >::const_iterator c_factor_iter =  m_mapFactorSection.find(szFactor);
    if (c_factor_iter == m_mapFactorSection.end())
    {
        snprintf(szErrMsg, sizeof(szErrMsg), "no redis node session for data_type %u and section_factor_type %u!",
                        iDataType, iSectionFactorType);
        LOG4_ERROR("%d: %s oMemOperate(%s)", ERR_LACK_CLUSTER_INFO, szErrMsg,oMemOperate.DebugString().c_str());
        Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, szErrMsg);
        return(false);
    }
    else
    {
        std::set<uint32>::const_iterator c_section_iter = c_factor_iter->second.lower_bound(oMemOperate.section_factor());
        if (c_section_iter == c_factor_iter->second.end())
        {
            snprintf(szErrMsg, sizeof(szErrMsg), "no redis node found for data_type %u and section_factor_type %u and factor %u!",
                            iDataType, iSectionFactorType, oMemOperate.section_factor());
            LOG4_ERROR("%d: %s oMemOperate(%s)", ERR_LACK_CLUSTER_INFO, szErrMsg,oMemOperate.DebugString().c_str());
            Response(stMsgShell, oInMsgHead, ERR_LACK_CLUSTER_INFO, szErrMsg);
            return(false);
        }
        else
        {
            std::string strMasterIdentify;
            std::string strSlaveIdentify;
            snprintf(szFactor, 32, "%u:%u:%u", iDataType, iSectionFactorType, *c_section_iter);
            m_pRedisNodeSession = (SessionRedisNode*)GetSession(std::string(szFactor), "net::SessionRedisNode");
            if (oMemOperate.redis_operate().hash_key().size() > 0)
            {
                m_pRedisNodeSession->GetRedisNode(oMemOperate.redis_operate().hash_key(), strMasterIdentify, strSlaveIdentify);
            }
            else
            {
                m_pRedisNodeSession->GetRedisNode(oMemOperate.redis_operate().key_name(), strMasterIdentify, strSlaveIdentify);
            }
            util::CJsonObject oRedisNodeJson;
            oRedisNodeJson.Add("master", strMasterIdentify);
            oRedisNodeJson.Add("slave", strSlaveIdentify);
            pStepDbDistribute = new StepDbDistribute(stMsgShell, oInMsgHead, oInMsgBody, &oRedisNodeJson);
            if (NULL == pStepDbDistribute)
            {
                Response(stMsgShell, oInMsgHead, ERR_NEW,
                                "malloc space for pStepDbDistribute error!");
                return(false);
            }

            if (RegisterCallback(pStepDbDistribute))
            {
                if (STATUS_CMD_RUNNING == pStepDbDistribute->Emit(ERR_OK))
                {
                    return(true);
                }
                else
                {
                    DeleteCallback(pStepDbDistribute);
                    return(false);
                }
            }
            return(false);
        }
    }
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
