/*******************************************************************************
 * Project:  CenterServer
 * @file     CmdServerData.cpp
 * @brief 
 * @author   cjy
 * @date:    2015年8月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "CmdServerData.hpp"

using namespace std;

#ifdef __cplusplus
extern "C"
{
#endif

net::Cmd* create()
{
    net::Cmd* pCmd = new core::CmdServerData();
    return (pCmd);
}

#ifdef __cplusplus
}
#endif

namespace core
{

CmdServerData::CmdServerData()
                : pSess(NULL),boInit(false)
{
}

CmdServerData::~CmdServerData()
{
}

bool CmdServerData::Init()
{
    if (boInit)return true;
    LOG4_DEBUG("CmdServerData::Init");
    pSess = GetNodeSession(GetLabor(),GetConfigPath(),true);
    if(!pSess)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    boInit = true;
    return true;
}

bool CmdServerData::AnyMessage(const net::tagMsgShell& stMsgShell,const MsgHead& oInMsgHead, const MsgBody& oInMsgBody)
{
	if (!pSess->IsMaster())
	{
		LOG4_DEBUG("it is not master,don't need to write server data load");
		return Response(stMsgShell,oInMsgHead,ERR_OK);
	}
	std::string nodetype;
	int innerport;
	std::string innerip;
	int outerport;
	std::string outerip;
	std::string status;
	util::CJsonObject jParseObj;
	if (!jParseObj.Parse(oInMsgBody.body()))
	{
		LOG4_WARN("error jsonParse error! body[%s]",
						oInMsgBody.body().c_str());
		return Response(stMsgShell,oInMsgHead,ERR_BODY_JSON);
	}
	LOG4_DEBUG("server report json data(%s)",jParseObj.ToString().c_str());
	if (!jParseObj.Get("nodetype", nodetype))
	{
		LOG4_WARN("CmdServerData::AnyMessage miss nodetype");
		return Response(stMsgShell,oInMsgHead,ERR_REQ_MISS_PARAM);
	}
	if (!jParseObj.Get("innerport", innerport))
	{
		LOG4_WARN("CmdServerData::AnyMessage miss innerport");
		return Response(stMsgShell,oInMsgHead,ERR_REQ_MISS_PARAM);
	}
	if (!jParseObj.Get("innerip", innerip))
	{
		LOG4_WARN("CmdServerData::AnyMessage miss innerip");
		return Response(stMsgShell,oInMsgHead,ERR_REQ_MISS_PARAM);
	}
	jParseObj.Get("outerport", outerport);
	jParseObj.Get("outerip", outerip);
	if (!jParseObj.Get("status", status))
	{
		LOG4_WARN("CmdServerData::AnyMessage miss status");
		return Response(stMsgShell,oInMsgHead,ERR_REQ_MISS_PARAM);
	}
    if (!pSess->WriteServerDataToDB(nodetype.c_str(), innerport,
                            innerip.c_str(), outerport, outerip.c_str(),
                            status.c_str()))
	{
		LOG4_ERROR("WriteServerDataToDB error,nodetype(%s),innerport(%d),innerip(%s),outerport(%d),outerip(%s),status(%s)",
						nodetype.c_str(), innerport, innerip.c_str(),
						outerport, outerip.c_str(), status.c_str());
		return Response(stMsgShell,oInMsgHead,ERR_SERVERINFO_RECORD);
	}
    return Response(stMsgShell,oInMsgHead,ERR_OK);
}

bool CmdServerData::Response(const net::tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,int iRet)
{
    MsgHead oOutMsgHead;
    MsgBody oOutMsgBody;
    oOutMsgHead.set_cmd(oInMsgHead.cmd() + 1);
    oOutMsgHead.set_seq(oInMsgHead.seq());
    util::CJsonObject jObjReturn;
    jObjReturn.Add("errcode", iRet);
    oOutMsgBody.set_body(jObjReturn.ToString());
    oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
    GetLabor()->SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
    return (iRet ? false : true);
}

} /* namespace starshiplib */
