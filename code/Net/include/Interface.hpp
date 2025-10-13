/*******************************************************************************
 * Project:  Net
 * @file     Interface.hpp
 * @brief    Node工作成员
 * @author   Tommy
 * @date:    2017年9月6日
 * Modify history:
 ******************************************************************************/
#ifndef SRC_LABOR_INTERFACE_HPP_
#define SRC_LABOR_INTERFACE_HPP_
#include "NetDefine.hpp"
#include "dispatcher/Coroutine.h"
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
class StepState;
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
 * @brief 执行状态步骤（含注册）
 * @param uiTimeOutMax 超时次数
 * @param uiToRetry 是否超时重发 1：是 0 否
 * @param dTimeout 超时时间（单位秒，默认使用配置时间）
 * @return 是否成功
 * */
bool Launch(StepState *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);
/*
 * @brief 注册状态步骤（未执行）
 * @param uiTimeOutMax 超时次数
 * @param uiToRetry 是否超时重发 1：是 0 否
 * @param dTimeout 超时时间（单位秒，默认使用配置时间）
 * @return 是否成功
 * */
bool Register(StepState *step,uint32 uiTimeOutMax=3,uint8 uiToRetry = 1,double dTimeout = 0.0);

bool Json2Pb(const std::string &strJson,google::protobuf::Message &message);

bool Pb2Json(const google::protobuf::Message &message, std::string &strJson);

bool Pb2Json(const google::protobuf::Message &message, util::CJsonObject& oJson);

bool MsgBody2MemRsp(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,DataMem::MemRsp& oRsp);

}

#endif
