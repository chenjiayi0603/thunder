/*******************************************************************************
 * Project:  Center
 * @file     ModuleAdmin.cpp
 * @brief 
 * @author   cjy
 * @date:    2018年12月8日
 * @note
 * Modify history:
 ******************************************************************************/
#include "ModuleAdmin.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include "util/json/CJsonObject.hpp"

MUDULE_CREATE(coor::ModuleAdmin);

namespace coor
{
namespace
{
constexpr const char kPathAdmin[] = "/admin";
constexpr const char kPathServices[] = "/admin/api/services";
constexpr const char kPathDetail[] = "/admin/api/service/detail";
static const char kAdminPageRelPath[] = "/conf/admin/AdminPage.html";

static void ToLowerAscii(std::string& s)
{
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

static bool NameMatchesFilter(const std::string& nodeType, const std::string& filterLower)
{
    if (filterLower.empty())
    {
        return true;
    }
    std::string t = nodeType;
    ToLowerAscii(t);
    return t.find(filterLower) != std::string::npos;
}

static bool ReadWholeFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
} // namespace

bool ModuleAdmin::Init()
{
	if (nullptr == m_pSessionOnlineNodes)
	{
		m_pSessionOnlineNodes = GetSessionOnlineNodes();
		if (nullptr == m_pSessionOnlineNodes)
		{
			LOG4_ERROR("no session node found!");
			return false;
		}
	}
    return(true);
}

void ModuleAdmin::SendJsonResponse(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg,
        int status, const std::string& jsonBody)
{
    HttpMsg oHttpMsg;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_status_code(status);
    oHttpMsg.set_http_major(oInHttpMsg.http_major());
    oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>(
            "Content-Type", "application/json; charset=utf-8"));
    oHttpMsg.set_body(jsonBody);
    GetLabor()->SendTo(stMsgShell, oHttpMsg);
}

void ModuleAdmin::SendUnifiedAdminPage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    const std::string path = GetLabor()->GetWorkPath() + kAdminPageRelPath;
    std::string html;
    if (!ReadWholeFile(path, html))
    {
        LOG4_ERROR("ModuleAdmin: failed to read admin page: %s", path.c_str());
        HttpMsg oHttpMsg;
        oHttpMsg.set_type(HTTP_RESPONSE);
        oHttpMsg.set_status_code(500);
        oHttpMsg.set_http_major(oInHttpMsg.http_major());
        oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
        oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>(
                "Content-Type", "text/plain; charset=utf-8"));
        oHttpMsg.set_body(std::string("Admin page missing. Expected: ") + path);
        GetLabor()->SendTo(stMsgShell, oHttpMsg);
        return;
    }
    HttpMsg oHttpMsg;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_status_code(200);
    oHttpMsg.set_http_major(oInHttpMsg.http_major());
    oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>(
            "Content-Type", "text/html; charset=utf-8"));
    oHttpMsg.set_body(std::move(html));
    GetLabor()->SendTo(stMsgShell, oHttpMsg);
}

void ModuleAdmin::HandleGetServices(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    std::string nameFilter;
    auto pit = oInHttpMsg.params().find("name");
    if (pit != oInHttpMsg.params().end())
    {
        nameFilter = pit->second;
    }
    ToLowerAscii(nameFilter);

    util::CJsonObject oOnline;
    m_pSessionOnlineNodes->GetOnlineNode(oOnline);

    util::CJsonObject oData;
    oData.AddEmptySubArray("data");

    const int n = oOnline.GetArraySize();
    for (int i = 0; i < n; ++i)
    {
        const std::string nodeType = oOnline[i]("node_type");
        if (!NameMatchesFilter(nodeType, nameFilter))
        {
            continue;
        }
        const int inst = oOnline[i]["node"].GetArraySize();
        util::CJsonObject row("{}");
        row.Add("serviceName", nodeType);
        row.Add("clusterCount", 1);
        row.Add("instanceCount", inst);
        row.Add("healthyInstanceCount", inst);
        oData["data"].Add(row);
    }

    oData.Add("code", ERR_OK);
    oData.Add("msg", std::string("success."));
    SendJsonResponse(stMsgShell, oInHttpMsg, 200, oData.ToFormattedString());
}

void ModuleAdmin::HandleGetServiceDetail(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    auto tit = oInHttpMsg.params().find("type");
    if (tit == oInHttpMsg.params().end() || tit->second.empty())
    {
        util::CJsonObject err("{}");
        err.Add("code", ERR_INVALID_ARGV);
        err.Add("msg", "missing query parameter type");
        SendJsonResponse(stMsgShell, oInHttpMsg, 400, err.ToFormattedString());
        return;
    }
    const std::string& nodeType = tit->second;
    util::CJsonObject oReport;
    if (!m_pSessionOnlineNodes->GetNodeReport(nodeType, oReport))
    {
        util::CJsonObject err("{}");
        err.Add("code", ERR_NODE_TYPE);
        err.Add("msg", std::string("unknown node_type: ") + nodeType);
        SendJsonResponse(stMsgShell, oInHttpMsg, 404, err.ToFormattedString());
        return;
    }

    util::CJsonObject ok("{}");
    ok.Add("code", ERR_OK);
    ok.Add("msg", std::string("success."));
    ok.Add("data", oReport);
    SendJsonResponse(stMsgShell, oInHttpMsg, 200, ok.ToFormattedString());
}

bool ModuleAdmin::AnyMessage(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
{
	LOG4_TRACE("%s() oInHttpMsg:%s", __FUNCTION__,oInHttpMsg.DebugString().c_str());
	if (HTTP_OPTIONS == oInHttpMsg.method())
    {
        LOG4_TRACE("receive an OPTIONS");
        ResponseOptions(stMsgShell, oInHttpMsg);
        return(true);
    }

    const std::string& path = oInHttpMsg.path();
    if (path == kPathServices && HTTP_GET == oInHttpMsg.method())
    {
        HandleGetServices(stMsgShell, oInHttpMsg);
        return true;
    }
    if (path == kPathDetail && HTTP_GET == oInHttpMsg.method())
    {
        HandleGetServiceDetail(stMsgShell, oInHttpMsg);
        return true;
    }
    if (path == kPathAdmin && HTTP_GET == oInHttpMsg.method())
    {
        SendUnifiedAdminPage(stMsgShell, oInHttpMsg);
        return true;
    }
    if (path == kPathAdmin && HTTP_POST == oInHttpMsg.method())
    {
        HttpMsg oHttpMsg;
        oHttpMsg.set_type(HTTP_RESPONSE);
        oHttpMsg.set_status_code(200);
        oHttpMsg.set_http_major(oInHttpMsg.http_major());
        oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
        util::CJsonObject oResponseData;
        util::CJsonObject oCmdJson;
        if (!oCmdJson.Parse(oInHttpMsg.body()))
        {
            oResponseData.Add("code", net::ERR_BODY_JSON);
            oResponseData.Add("msg", "error json format!");
            oHttpMsg.set_body(oResponseData.ToFormattedString());
            GetLabor()->SendTo(stMsgShell, oHttpMsg);
            return(false);
        }

        if (std::string("show") == oCmdJson("cmd") || std::string("SHOW") == oCmdJson("cmd"))
        {
            Show(oCmdJson, oResponseData);
        }
        else if (std::string("get") == oCmdJson("cmd") || std::string("GET") == oCmdJson("cmd"))
        {
            Get(stMsgShell, oInHttpMsg.http_major(), oInHttpMsg.http_minor(), oCmdJson, oResponseData);
        }
        else if (std::string("set") == oCmdJson("cmd") || std::string("SET") == oCmdJson("cmd"))
        {
            Set(stMsgShell, oInHttpMsg.http_major(), oInHttpMsg.http_minor(), oCmdJson, oResponseData);
        }
        else
        {
            oResponseData.Add("code", ERR_INVALID_CMD);
            oResponseData.Add("msg", std::string("invalid cmd \"") + oCmdJson("cmd") + std::string("\" !"));
        }

        if (oResponseData.IsEmpty())
        {
            return(true);
        }

        oHttpMsg.set_body(oResponseData.ToFormattedString());
        GetLabor()->SendTo(stMsgShell, oHttpMsg);
        return(true);
    }

    util::CJsonObject err("{}");
    err.Add("code", ERR_INVALID_CMD);
    err.Add("msg", std::string("method or path not supported: ") + path);
    SendJsonResponse(stMsgShell, oInHttpMsg, 405, err.ToFormattedString());
    return true;
}

void ModuleAdmin::ResponseOptions(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    HttpMsg oHttpMsg;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_status_code(200);
    oHttpMsg.set_http_major(oInHttpMsg.http_major());
    oHttpMsg.set_http_minor(oInHttpMsg.http_minor());
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Connection", "keep-alive"));
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Origin", "*"));
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Headers", "Origin, Content-Type, Cookie, Accept"));
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Methods", "GET, POST"));
    oHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Credentials", "true"));
    GetLabor()->SendTo(stMsgShell, oHttpMsg);
}

void ModuleAdmin::Show(util::CJsonObject& oCmdJson, util::CJsonObject& oResult) const
{
	LOG4_TRACE("%s() args size:%d", __FUNCTION__,oCmdJson["args"].GetArraySize());
    switch (oCmdJson["args"].GetArraySize())
    {
        case 0:
            oResult.Add("code", ERR_INVALID_ARGC);
            oResult.Add("msg", std::string("invalid arguments num for \"")
                    + oCmdJson("cmd") + std::string("\"!"));
            break;
        case 1:
            if (std::string("ip_white") == oCmdJson["args"](0))
            {
                oResult.Add("code", ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetIpWhite(oResult["data"]);
            }
            else if (std::string("subscription") == oCmdJson["args"](0))
            {
                oResult.Add("code", ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetSubscription(oResult["data"]);
                LOG4_TRACE("%s() GetSubscription:%s", __FUNCTION__,oResult["data"].ToString().c_str());
            }
            else if (std::string("nodes") == oCmdJson["args"](0))
            {
                oResult.Add("code", ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetOnlineNode(oResult["data"]);
            }
            else if (std::string("center") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetCenter(oResult["data"]);
            }
            else
            {
                oResult.Add("code", ERR_INVALID_ARGV);
                oResult.Add("msg", std::string("invalid arguments \"")
                        + oCmdJson["args"](0) + std::string("\" !"));
            }
            break;
        case 2:
            if (std::string("subscription") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetSubscription(oCmdJson["args"](1), oResult["data"]);
            }
            else if (std::string("nodes") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetOnlineNode(oCmdJson["args"](1), oResult["data"]);
            }
            else if (std::string("node_report") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetNodeReport(oCmdJson["args"](1), oResult["data"]);
            }
            else if (std::string("node_detail") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetNodeReport(oCmdJson["args"](1), oResult["data"]);
            }
            else
            {
                oResult.Add("code", ERR_INVALID_ARGV);
                oResult.Add("msg", std::string("invalid arguments \"")
                        + oCmdJson["args"](0) + std::string("\" !"));
            }
            break;
        case 3:
            if (std::string("node_report") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetNodeReport(oCmdJson["args"](1), oCmdJson["args"](2), oResult["data"]);
            }
            else if (std::string("node_detail") == oCmdJson["args"](0))
            {
                oResult.Add("code", (int32)net::ERR_OK);
                oResult.Add("msg", std::string("success."));
                oResult.AddEmptySubArray("data");
                m_pSessionOnlineNodes->GetNodeReport(oCmdJson["args"](1), oCmdJson["args"](2), oResult["data"]);
            }
            else
            {
                oResult.Add("code", ERR_INVALID_ARGV);
                oResult.Add("msg", "invalid arguments num or invalid arguments!");
            }
            break;
        default:
            oResult.Add("code", ERR_INVALID_ARGC);
            oResult.Add("msg", std::string("invalid arguments num for \"")
                    + oCmdJson("cmd") + " " + oCmdJson["args"](0) + std::string("\" !"));
    }
}

void ModuleAdmin::Get(const net::tagMsgShell& stMsgShell,
        int32 iHttpMajor, int32 iHttpMinor,
        util::CJsonObject& oCmdJson, util::CJsonObject& oResult)
{
    if (std::string("node_config") == oCmdJson["args"](0))
    {
        if (oCmdJson["args"].GetArraySize() == 2)
        {
        	if (!GetLabor()->ExecStep(std::make_unique<StepGetConfig>(
        	                        stMsgShell, iHttpMajor, iHttpMinor,
        	                        (int32)net::CMD_REQ_GET_NODE_CONFIG,
        	                        oCmdJson["args"](1), std::string(""), std::string(""))))
        	{
        		oResult.Add("code", (int32)net::ERR_NEW);
				oResult.Add("msg", "server internal error!");
        	}
        }
        else
        {
            oResult.Add("code", ERR_INVALID_ARGV);
            oResult.Add("msg", "invalid arguments num or invalid arguments!");
        }
    }
    else if (std::string("node_custom_config") == oCmdJson["args"](0))
    {
        if (oCmdJson["args"].GetArraySize() == 2)
        {
        	if (!GetLabor()->ExecStep(std::make_unique<coor::StepGetConfig>(stMsgShell, iHttpMajor, iHttpMinor,
					(int32)net::CMD_REQ_GET_NODE_CUSTOM_CONFIG,
					oCmdJson["args"](1), std::string(""), std::string(""))))
        	{
        		oResult.Add("code", (int32)net::ERR_NEW);
				oResult.Add("msg", "server internal error!");
        	}
        }
        else
        {
            oResult.Add("code", ERR_INVALID_ARGV);
            oResult.Add("msg", "invalid arguments num or invalid arguments!");
        }
    }
    else if (std::string("custom_config") == oCmdJson["args"](0))
    {
        if (oCmdJson["args"].GetArraySize() == 4)
        {
			if (!GetLabor()->ExecStep(std::make_unique<coor::StepGetConfig>(stMsgShell, iHttpMajor, iHttpMinor,
					(int32)net::CMD_REQ_GET_CUSTOM_CONFIG,
					oCmdJson["args"](1), oCmdJson["args"](2), oCmdJson["args"](3))))
			{
				oResult.Add("code", (int32)net::ERR_NEW);
				oResult.Add("msg", "server internal error!");
			}
        }
        else
        {
            oResult.Add("code", ERR_INVALID_ARGV);
            oResult.Add("msg", "invalid arguments num or invalid arguments!");
        }
    }
    else
    {
        oResult.Add("code", ERR_INVALID_ARGV);
        oResult.Add("msg", "invalid arguments num or invalid arguments!");
    }
}

void ModuleAdmin::Set(const net::tagMsgShell& stMsgShell,int32 iHttpMajor, int32 iHttpMinor,
        util::CJsonObject& oCmdJson, util::CJsonObject& oResult)
{
    if (std::string("node_config") == oCmdJson["args"](0)
            || std::string("node_config_from_file") == oCmdJson["args"](0)
            || std::string("node_custom_config") == oCmdJson["args"](0)
            || std::string("node_custom_config_from_file") == oCmdJson["args"](0))
    {
        int32 iCmd = net::CMD_REQ_SET_NODE_CONFIG;
        if (std::string("node_custom_config") == oCmdJson["args"](0)
                || std::string("node_custom_config_from_file") == oCmdJson["args"](0))
        {
            iCmd = net::CMD_REQ_SET_NODE_CUSTOM_CONFIG;
        }
        if (oCmdJson["args"].GetArraySize() == 3)
        {
            if (!GetLabor()->ExecStep(std::make_unique<StepSetConfig>(m_pSessionOnlineNodes,
                    stMsgShell, iHttpMajor, iHttpMinor,
                    iCmd, oCmdJson["args"](1), std::string(""),
                    oCmdJson["args"](2), std::string(""), std::string(""))))
            {
                oResult.Add("code", (int32)net::ERR_NEW);
                oResult.Add("msg", "server internal error!");
            }
        }
        else if (oCmdJson["args"].GetArraySize() == 4)
        {
            if (!GetLabor()->ExecStep(std::make_unique<StepSetConfig>( m_pSessionOnlineNodes,
                    stMsgShell, iHttpMajor, iHttpMinor,
                    iCmd, oCmdJson["args"](1),
                    oCmdJson["args"](2), oCmdJson["args"](3), 
                    std::string(""), std::string(""))))
            {
                oResult.Add("code", (int32)net::ERR_NEW);
                oResult.Add("msg", "server internal error!");
            }
        }
        else
        {
            oResult.Add("code", ERR_INVALID_ARGV);
            oResult.Add("msg", "invalid arguments num or invalid arguments!");
        }
    }
    else if (std::string("custom_config") == oCmdJson["args"](0)
            || std::string("custom_config_from_file") == oCmdJson["args"](0))
    {
        int32 iCmd = net::CMD_REQ_SET_CUSTOM_CONFIG;
        if (oCmdJson["args"].GetArraySize() == 4)
        {
            if (!GetLabor()->ExecStep(std::make_unique<StepSetConfig>(m_pSessionOnlineNodes,
                    stMsgShell, iHttpMajor, iHttpMinor,
                    iCmd, oCmdJson["args"](1), std::string(""),
                    oCmdJson["args"](3), std::string(""), oCmdJson["args"](2))))
            {
                oResult.Add("code", (int32)net::ERR_NEW);
                oResult.Add("msg", "server internal error!");
            }
        }
        else if (oCmdJson["args"].GetArraySize() == 5)
        {
            if (!GetLabor()->ExecStep(std::make_unique<StepSetConfig>(m_pSessionOnlineNodes,
                    stMsgShell, iHttpMajor, iHttpMinor,
                    iCmd, oCmdJson["args"](1), std::string(""), oCmdJson["args"](4),
                    oCmdJson["args"](2), oCmdJson["args"](3))))
            {
                oResult.Add("code", (int32)net::ERR_NEW);
                oResult.Add("msg", "server internal error!");
            }
        }
        else if (oCmdJson["args"].GetArraySize() == 6)
        {
            if (!GetLabor()->ExecStep(std::make_unique<StepSetConfig>(m_pSessionOnlineNodes,
                    stMsgShell, iHttpMajor, iHttpMinor,
                    iCmd, oCmdJson["args"](1), oCmdJson["args"](2), oCmdJson["args"](5),
                    oCmdJson["args"](3), oCmdJson["args"](4))))
            {
                oResult.Add("code", (int32)net::ERR_NEW);
                oResult.Add("msg", "server internal error!");
            }
        }
        else
        {
            oResult.Add("code", ERR_INVALID_ARGV);
            oResult.Add("msg", "invalid arguments num or invalid arguments!");
        }
    }
    else
    {
        oResult.Add("code", ERR_INVALID_ARGV);
        oResult.Add("msg", "invalid arguments num or invalid arguments!");
    }
}

} /* namespace coor */

