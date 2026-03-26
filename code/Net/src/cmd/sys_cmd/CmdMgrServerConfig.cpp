/*******************************************************************************
 * Project:  Net
 * @file     CmdMgrServerConfig.cpp
 * @brief    Center 下发服务器配置：写入共享内存并递增版本，各 Worker/Loader 通过 CheckShareMem 等拉取
 ******************************************************************************/
#include <fstream>
#include "CmdMgrServerConfig.hpp"
#include "labor/Manager.hpp"
#include "Interface.hpp"

namespace net
{

bool CmdMgrServerConfig::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    auto sendRsp = [&](uint32 errNo, const char* errMsg) -> bool {
        OrdinaryResponse oRes;
        oRes.set_err_no(errNo);
        oRes.set_err_msg(errMsg ? errMsg : "");
        GetLabor()->SendTo(stMsgShell, oInMsgHead.cmd() + 1, oInMsgHead.seq(),
                          oRes.SerializeAsString());
        return errNo == 0;
    };

    const std::string& oInMsgBodyStr = oInMsgBody.body();
    util::CJsonObject reqConfigObj;
    if (!reqConfigObj.Parse(oInMsgBodyStr))
    {
        LOG4_WARN("%s body is not json: %s", __FUNCTION__, oInMsgBodyStr.c_str());
        return sendRsp(1, "invalid json");
    }

    std::string m_ReqConfigFileName;
    if (!reqConfigObj.Get("config_file", m_ReqConfigFileName))
    {
        LOG4_WARN("%s config_file missing", __FUNCTION__);
        return sendRsp(2, "config_file missing");
    }

    util::CJsonObject m_ReqConfigContent;
    if (!reqConfigObj.Get("config_content", m_ReqConfigContent))
    {
        LOG4_WARN("%s config_content missing", __FUNCTION__);
        return sendRsp(3, "config_content missing");
    }

    int m_ReqConfigType = 0;
    if (!reqConfigObj.Get("config_type", m_ReqConfigType))
    {
        LOG4_WARN("%s config_type missing", __FUNCTION__);
        return sendRsp(4, "config_type missing");
    }

    std::string strConfFile = GetConfigPath() + m_ReqConfigFileName;
    util::CJsonObject oLocalConfJson;
    if (!net::GetConfig(oLocalConfJson, strConfFile))
    {
        LOG4_ERROR("%s open conf (%s) to read error", __FUNCTION__, strConfFile.c_str());
        return sendRsp(5, "read local conf failed");
    }

    std::string oConfJsonStr;
    if (0 == m_ReqConfigType)
    {
        util::CJsonObject configUpdateContentSo;
        util::CJsonObject configLocalContentSo;
        if (m_ReqConfigContent.Get("so", configUpdateContentSo))
        {
            if (oLocalConfJson.Get("so", configLocalContentSo)
                && (configLocalContentSo.ToString() != configUpdateContentSo.ToString()))
            {
                oLocalConfJson.Replace("so", configUpdateContentSo);
            }
        }
        int iUpdateLogLevel(0);
        if (m_ReqConfigContent.Get("log_level", iUpdateLogLevel))
        {
            int iLocalLogLevel(0);
            if (oLocalConfJson.Get("log_level", iLocalLogLevel) && (iLocalLogLevel != iUpdateLogLevel))
            {
                oLocalConfJson.Replace("log_level", iUpdateLogLevel);
            }
        }
        util::CJsonObject configUpdateContentModule;
        util::CJsonObject configLocalContentModule;
        if (m_ReqConfigContent.Get("module", configUpdateContentModule))
        {
            if (oLocalConfJson.Get("module", configLocalContentModule)
                && (configLocalContentModule.ToString() != configUpdateContentModule.ToString()))
            {
                oLocalConfJson.Replace("module", configUpdateContentModule);
            }
        }
        oConfJsonStr = oLocalConfJson.ToFormattedString();
    }
    else
    {
        oConfJsonStr = m_ReqConfigContent.ToFormattedString();
    }

    if (oConfJsonStr.empty())
    {
        LOG4_ERROR("%s empty config after merge", __FUNCTION__);
        return sendRsp(6, "empty config");
    }

    Manager* pMgr = static_cast<Manager*>(GetLabor());
    LoaderConfigVersionData& verData = pMgr->GetLoaderConfigVersionData();
    verData.SetServerConfigFile(m_ReqConfigFileName, oConfJsonStr);
    const uint64 ver = verData.IncLoaderConfigVersion();
    LOG4_INFO("%s published to shm version=%llu file=%s", __FUNCTION__,
              static_cast<unsigned long long>(ver), m_ReqConfigFileName.c_str());

    std::ofstream fout(strConfFile.c_str(), std::ios::out | std::ios::trunc);
    if (fout.good())
    {
        fout.write(oConfJsonStr.c_str(), static_cast<std::streamsize>(oConfJsonStr.length()));
        fout.flush();
        fout.close();
    }
    else
    {
        LOG4_ERROR("%s open conf (%s) to write error", __FUNCTION__, strConfFile.c_str());
    }

    return sendRsp(0, "OK");
}

} /* namespace net */
