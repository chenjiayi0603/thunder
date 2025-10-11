/*******************************************************************************
 * Project:  AsyncServer
 * @file     StepRedisMysqlTest.hpp
 * @brief    访问多次mysql或者redis存储
 * @author   Tommy
 * @date:    2020年1月9日
 * Modify history:
 ******************************************************************************/
#ifndef SRC_STEP_StepRedisMysqlTest_HPP_
#define SRC_STEP_StepRedisMysqlTest_HPP_
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "user.pb.h"
#include "common.pb.h"
#include "RobotError.h"

namespace im
{

//本step作为演示多步骤访问存储处理
class StepRedisMysqlTest : public net::HttpStep
{
	enum
	{
		eStage_load_user_from_mysql_redis = 0,
		eStage_load_user_from_mysql = 1,
		eStage_load_user_from_redis = 2,
		eStage_redis_lua = 3,
		eStage_end = 4,
	};
public:
    StepRedisMysqlTest(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    virtual ~StepRedisMysqlTest() = default;

    virtual net::E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");
    virtual net::E_CMD_STATUS Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);
    virtual net::E_CMD_STATUS Timeout();
    void Response(int err_no,const std::string& msg);
#define TEST_TABLE "tb_test"
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
    //查询redis、mysql，同时查询redis 和mysql 的示范性接口
    net::E_CMD_STATUS Emit_lua();
    net::E_CMD_STATUS Emit_lua2();
    net::E_CMD_STATUS Emit_load_user_from_mysql_redis();
	net::E_CMD_STATUS Emit_load_user_from_mysql();
	net::E_CMD_STATUS Emit_load_user_from_redis();

	std::string m_strResponseData_from_mysql_redis;
	std::string m_strResponseData_from_mysql;
	std::string m_strResponseData_from_redis;

	net::E_CMD_STATUS Emit_set_user_from_redis();
private:
	uint32 m_uiID = 1;
	int m_istage = eStage_redis_lua;          ///< 状态标识
    int m_iTimeoutNum = 0;          ///< 超时次数
};

} /* namespace im */

#endif 
