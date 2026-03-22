/*******************************************************************************
 * Project:  Net
 * @file     Interface.hpp
 * @brief    Node工作成员
 * @author   cjy
 * @date:    2017年9月6日
 * Modify history:
 ******************************************************************************/
#ifndef SRC_LABOR_INTERFACE_HPP_
#define SRC_LABOR_INTERFACE_HPP_
#include <memory>
#include "NetDefine.hpp"
#include "storage/dataproxy.pb.h"
namespace net
{

#define USE_CONHASH

//如果是监控版本则定义_CAT_MONITOR，否则就注释掉
#ifndef _CAT_MONITOR
#define _CAT_MONITOR
#endif

class Cmd;
class Module;
class Step;
class RedisStep;
class MysqlStep;
class HttpStep;
class Session;
class Labor;
class StepParam;

/**
 * @brief 获取配置目录
 * @return 配置目录
 */
std::string GetConfigPath();
/**
 * @brief 获取配置数据
 * @param oConf 返回配置数据
 * @param strConfFile 配置文件路径
 * @return 成功
 */
bool GetConfig(util::CJsonObject& oConf,const std::string &strConfFile);
/**
 * @brief 获取配置数据
 * @param strFileData 返回配置数据
 * @param strConfFile 配置文件路径
 * @return 成功
 */
bool GetFileData(std::string& strFileData,const std::string &strConfFile);
/*
 * @brief 执行步骤（含注册）
 * @note `MysqlStep` 走 `Register` 时会 `Init(超时次数, 重试)`；StepCo20 等协程 Step 继承 HttpStep，通常用 Launch，超时请 SetTimeoutParams
 * @param uiTimeOutMax 超时次数
 * @param uiToRetry 是否超时重发 1：是 0 否
 * @param dTimeout 超时时间（单位秒，默认使用配置时间）
 * @return 是否成功
 * */
bool Launch(std::unique_ptr<Step> step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);
bool Launch(Step *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);
/*
 * @brief 注册 MysqlStep（未执行 Emit）
 * @param uiTimeOutMax 超时次数
 * @param uiToRetry 是否超时重发 1：是 0 否
 * @param dTimeout 超时时间（单位秒，默认使用配置时间）
 * @return 是否成功
 * */
bool Register(MysqlStep *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);

bool Json2Pb(const std::string &strJson,google::protobuf::Message &message);

bool Pb2Json(const google::protobuf::Message &message, std::string &strJson);

bool Pb2Json(const google::protobuf::Message &message, util::CJsonObject& oJson);

bool MsgBody2MemRsp(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,DataMem::MemRsp& oRsp);

}

#endif
