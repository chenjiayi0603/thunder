/*******************************************************************************
 * Project:  AsyncServer
 * @file     StepMongoTest.hpp
 * @brief    访问多次mongo存储
 * @author   Tommy
 * @date:    2020年1月9日
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_StepMongoTest_HPP_
#define SRC_STEP_StepMongoTest_HPP_
#include "util/bson/BsonUtil.h"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "user.pb.h"
#include "common.pb.h"
#include "RobotError.h"

namespace im
{

//本step作为演示多步骤访问存储处理
class StepMongoTest : public net::HttpStep
{
	enum
	{
		eStage_insert3_mongo,
		eStage_insert_mongo,
		eStage_insert2_mongo,
		eStage_upsert_mongo,
		eStage_search_mongo,
		eStage_search2_mongo,
		eStage_end,
		eStage_update_mongo,
		eStage_update_end,
		eStage_pineline_mongo,
		eStage_pineline_end,
	};
public:
    StepMongoTest(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    virtual ~StepMongoTest() = default;

    virtual net::E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    virtual net::E_CMD_STATUS Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);
    virtual net::E_CMD_STATUS Timeout();
    void Response(int err_no,const std::string& msg);

    #define tb_coordinate "imtest"// "tb_coordinate"
	#define tb_coordinate2 "imtest2"// "tb_coordinate"
    /*
     * 每次发送返回步骤状态,步骤状态如下：
     enum E_CMD_STATUS
	{
		STATUS_CMD_START            = 0,    ///< 创建命令执行者，但未开始执行
		STATUS_CMD_RUNNING          = 1,    ///< 正在执行命令
		STATUS_CMD_COMPLETED        = 2,    ///< 命令执行完毕
		STATUS_CMD_FAULT            = 3,    ///< 命令执行出错并且不必重试
	};
	STATUS_CMD_START 初始状态
	STATUS_CMD_RUNNING 则step继续执行 （只有STATUS_CMD_RUNNING是step继续执行）
	STATUS_CMD_COMPLETED 则step成功完成
	STATUS_CMD_FAULT 则step发生错误放弃
     * */
    uint32 m_uiGroupId = 1000;
    //请求
    net::E_CMD_STATUS Emit_findAndModify_mongo();
    net::E_CMD_STATUS Emit_insert3_mongo();
    net::E_CMD_STATUS Emit_insert_mongo();
    net::E_CMD_STATUS Emit_insert2_mongo();
    net::E_CMD_STATUS Emit_upsert_mongo();
    net::E_CMD_STATUS Emit_search_mongo();
    net::E_CMD_STATUS Emit_search2_mongo();
    net::E_CMD_STATUS Emit_update_mongo();

    net::E_CMD_STATUS Emit_pineline_mongo();

    //响应
    void Callback_insert_mongo(const DataMem::MemRsp &oRsp);
    void Callback_insert2_mongo(const DataMem::MemRsp &oRsp);
    void Callback_upsert_mongo(const DataMem::MemRsp &oRsp);
    void Callback_search_mongo(const DataMem::MemRsp &oRsp);
    void Callback_search2_mongo(const DataMem::MemRsp &oRsp);
    void Callback_update_mongo(const DataMem::MemRsp &oRsp);

    void Callback_pineline_mongo(const DataMem::MemRsp &oRsp);
private:
    util::CJsonObject m_strResponseData;
	util::CJsonObject m_strResponse2Data;
    util::CJsonObject m_jsonObj;
	uint32 m_uiImid = 10001;
	int m_istage = eStage_insert3_mongo;          ///< 状态标识
    int m_iTimeoutNum = 0;          ///< 超时次数
};

} /* namespace im */

#endif 
