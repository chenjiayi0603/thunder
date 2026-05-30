/*******************************************************************************
 * Project:  Net
 * @file     Interface.hpp
 * @brief    Node工作成员
 * @author   cjy
 * @date:    2017年9月6日
 * Modify history:
 ******************************************************************************/
#include "Interface.hpp"
#include <utility>
#include "absl/status/status.h"
#include "google/protobuf/util/json_util.h"
#include "coro/StepCo20Func.hpp"
#include "labor/Labor.hpp"
#include "step/MysqlStep.hpp"

namespace net
{

std::string GetConfigPath(){return(GetLabor()->GetWorkPath() + std::string("/conf/"));}

bool Launch(std::unique_ptr<Step> pStep,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	if (!pStep)
	{
		LOG4_ERROR("%s() null step",__FUNCTION__);
		return(false);
	}
	return GetLabor()->ExecStep(std::move(pStep),dTimeout);
}

bool LaunchCo(const tagMsgShell& stMsgShell,
              const HttpMsg& oHttpMsg,
              StepCo20Func::CoroFn fn)
{
	return Launch(std::make_unique<StepCo20Func>(stMsgShell, oHttpMsg, std::move(fn)));
}

bool LaunchCo(const tagMsgShell& stMsgShell,
              const MsgHead& oMsgHead,
              StepCo20Func::CoroFn fn)
{
	return Launch(std::make_unique<StepCo20Func>(stMsgShell, oMsgHead, std::move(fn)));
}

bool LaunchCo(std::unique_ptr<StepCo20> pStep)
{
	return Launch(std::move(pStep));
}

bool Register(std::unique_ptr<MysqlStep> pStep,uint32 uiTimeOutMax,uint8 uiToRetry,double dTimeout)
{
	MysqlStep* pRaw = pStep.get();
	if (!GetLabor()->RegisterCallback(std::unique_ptr<Step>(pStep.release()),dTimeout))
	{
		LOG4_ERROR("%s() RegisterCallback error",__FUNCTION__);
		return(false);
	}
	pRaw->Init(uiTimeOutMax,uiToRetry);
	return true;
}

bool GetConfig(util::CJsonObject& oConf,const std::string &strConfFile)
{
	std::ifstream fin(strConfFile.c_str());
	//配置信息输入流
	if (fin.good())
	{
		//解析配置信息 JSON格式
		std::stringstream ssContent;
		ssContent << fin.rdbuf();
		if (!oConf.Parse(ssContent.str()))
		{
			//配置文件解析失败
			if (GetLabor()->IsInitLogger())
			{
				LOG4_ERROR("Read conf (%s) error,it's maybe not a json file!",strConfFile.c_str());
			}
			else
			{
				std::cerr << "Open conf to read error,it's maybe not a json file!" << strConfFile << std::endl;
			}
			ssContent.str("");
			fin.close();
			return false;
		}
		ssContent.str("");
		fin.close();
		return true;
	}
	else
	{
		//配置信息流读取失败
		if (GetLabor()->IsInitLogger())
		{
			LOG4_ERROR( "Open conf (%s) error!",strConfFile.c_str());
		}
		else
		{
			std::cerr << "Open conf to read error!" << strConfFile << std::endl;
		}
		return false;
	}
}

bool GetFileData(std::string& strFileData,const std::string &strConfFile)
{
	std::ifstream fin(strConfFile.c_str());
	//配置信息输入流
	if (fin.good())
	{
		//解析配置信息 JSON格式
		std::stringstream ssContent;
		ssContent << fin.rdbuf();
		strFileData = ssContent.str();
		ssContent.str("");
		fin.close();
		return true;
	}
	else
	{
		//配置信息流读取失败
		LOG4_ERROR( "Open conf (%s) error!",strConfFile.c_str());
		return false;
	}
}

bool Json2Pb(const std::string &strJson,google::protobuf::Message &message)
{
	if (strJson.size() > 0)//json
    {
        google::protobuf::util::JsonParseOptions oOption;
        absl::Status oStatus = google::protobuf::util::JsonStringToMessage(strJson,&message, oOption);
        if(!oStatus.ok())
        {
            LOG4_ERROR("%s() json sbody to MsgBody error(%s)!strJson(%s) message.descriptor(%s)",
                                __FUNCTION__,oStatus.ToString().c_str(),strJson.c_str(),
                                message.GetDescriptor()->DebugString().c_str());
            return (false);
        }
        return true;
    }
    LOG4_TRACE("%s() empty msg body",__FUNCTION__);//如果请求消息体为空，则请求内容为空
    return (false);
}

bool Pb2Json(const google::protobuf::Message &message, std::string &strJson)
{
	google::protobuf::util::JsonPrintOptions oOption;
	oOption.always_print_fields_with_no_presence = true;
	absl::Status oStatus = google::protobuf::util::MessageToJsonString(message,&strJson,oOption);
	if(!oStatus.ok())
	{
		LOG4_ERROR("%s() MessageToJsonString failed error(%s)",__FUNCTION__,oStatus.ToString().c_str());
		return false;
	}
	return true;
}

bool Pb2Json(const google::protobuf::Message &message, util::CJsonObject& oJson)
{
	std::string strJson;
	google::protobuf::util::JsonPrintOptions oOption;
	oOption.always_print_fields_with_no_presence = true;
	absl::Status oStatus = google::protobuf::util::MessageToJsonString(message,&strJson,oOption);
	if(!oStatus.ok())
	{
		LOG4_ERROR("%s() MessageToJsonString failed error(%s)",__FUNCTION__,oStatus.ToString().c_str());
		return false;
	}
	if (!oJson.Parse(strJson))
	{
		LOG4_ERROR("%s() oJson.Parse(strJson) failed:%s",__FUNCTION__,strJson.c_str());
		return false;
	}
	return true;
}

bool MsgBody2MemRsp(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,DataMem::MemRsp& oRsp)
{
	if(net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())//系统错误（如没有该指令）
	{
		LOG4_ERROR("%s() system response error",__FUNCTION__);
		return false;
	}
	if(!oRsp.ParseFromString(oInMsgBody.body()))
	{
		LOG4_ERROR("%s() parse protobuf data fault",__FUNCTION__);
		return false;
	}
	//读存储出错
	if(0 != oRsp.err_no())
	{
		if(oRsp.has_err_msg())
		{
			LOG4_ERROR("%s() Callback error %d: %s!",__FUNCTION__,oRsp.err_no(),oRsp.err_msg().c_str());
		}
		else
		{
			LOG4_ERROR("%s() Callback error %d!",__FUNCTION__, oRsp.err_no());
		}
		return false;
	}
	return true;
}




}

