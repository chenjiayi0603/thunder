/*******************************************************************************
 * Project:  Access
 * @file     StepRedisMysqlTest.cpp
 * @brief    访问多次mysql或者redis存储
 * @author   Tommy
 * @date:    2020年1月9日
 * @note
 * Modify history:
 ******************************************************************************/
#include "RobotRedisProto.h"
#include "StepRedisMysqlTest.hpp"
#include "user.pb.h"
#include "common.pb.h"
#include "util/CBuffer.hpp"

namespace im
{

StepRedisMysqlTest::StepRedisMysqlTest(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg)
:net::HttpStep(stMsgShell,oInHttpMsg)
{
}

net::E_CMD_STATUS StepRedisMysqlTest::Emit(int iErrno, const std::string& strErrMsg,const std::string& strErrShow)
{
	LOG4_TRACE("%s()", __FUNCTION__);
	if(eStage_load_user_from_mysql_redis == m_istage)
	{
		return Emit_load_user_from_mysql_redis();
	}
	else if(eStage_load_user_from_mysql == m_istage)
	{
		return Emit_load_user_from_mysql();
	}
	else if(eStage_load_user_from_redis == m_istage)
	{
		return Emit_load_user_from_redis();
	}
	else if (eStage_redis_lua == m_istage)
	{
		return Emit_lua2();
	}
	else if (eStage_end == m_istage)
	{
		util::CJsonObject oJsonObj;
		oJsonObj.Add("code", robot::ERR_OK);
		oJsonObj.Add("msg", "ok");
		oJsonObj.Add("from_mysql_redis", m_strResponseData_from_mysql_redis);
		oJsonObj.Add("from_mysql", m_strResponseData_from_mysql);
		oJsonObj.Add("from_redis", m_strResponseData_from_redis);
		SendToClient(oJsonObj.ToString());
		LOG4_TRACE("%s() done m_istage(%d)", __FUNCTION__,m_istage);
		return(net::STATUS_CMD_COMPLETED);
	}
	LOG4_TRACE("%s() error m_istage(%d)", __FUNCTION__,m_istage);
	return(net::STATUS_CMD_COMPLETED);
}

// "if redis.call('get', KEYS[1]) == false then redis.call('set', KEYS[1], ARGV[1]) redis.call('expire', KEYS[1], ARGV[2]) return 0 else return 1 end";

//char cmd[] = "eval \"local a=redis.call('get',KEYS[1]);if a~='1' then redis.call('set',KEYS[1],'1');return 1;else return 0;end\" 1 UserLock:%s";
//redisReply *reply = (redisReply*)redisCommand(c, cmd, "zhangsan");

//char cmd[] = "local a=redis.call('get',KEYS[1]);if a~='1' then redis.call('set',KEYS[1],'1');return 1;else return 0;end";
//reply = (redisReply*)redisCommand(c, "eval %s 1 UserLock:%s", cmd, "zhangsan");

//char cmd[] = "if (redis.call('exists', KEYS[1]) == 1) then redis.call('incr', KEYS[1]);return 1;else return 0;end";
//reply = (redisReply*)redisCommand(c, "eval %s 1 %s", cmd, "zhangsan");

//http://doc.redisfans.com/script/eval.html
// EVAL "if (redis.call('exists', KEYS[1]) == 1) then return redis.call('incr', KEYS[1]);else return 0;end" 1 testkeyincr11

//EVAL "if (redis.call('exists', KEYS[1]) == 0) then return {-1,0}; end;local tmpval = redis.call('get', KEYS[1]); if (tmpval % 10 == 0) then return {-2,tmpval};else return {0,redis.call('incr', KEYS[1])};end" 1 testkeyincr12
net::E_CMD_STATUS StepRedisMysqlTest::Emit_lua()
{
	LOG4_TRACE("%s()", __FUNCTION__);
//	std::string cmdstr = "if (redis.call('exists', KEYS[1]) == 1) then redis.call('incr', KEYS[1]);return 1;else return 0;end";
//	std::string cmdstr = "if (redis.call('exists', KEYS[1]) == 1) then return redis.call('incr', KEYS[1]);else return 0;end";
	std::string cmdstr =
	"if (redis.call('exists', KEYS[1]) == 0) then return {-1,0}; end;local tmpval = redis.call('get', KEYS[1]); if (tmpval % 10 == 0) then return {-2,tmpval};else return {0,redis.call('incr', KEYS[1])};end;";
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	net::RedisOperator oMemOper(0,cmdstr, "eval");//redis 指令
	oMemOper.AddRedisField("1");
	oMemOper.AddRedisField("testkeyincr12");
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}
//err_msg: "OK"
//totalcount: 1
//curcount: 1
//record_data {
//  field_info {
//    col_value: "-2"
//  }
//  field_info {
//    col_value: "10"
//  }
//}
//from: 1

// EVAL "redis.call('setnx', KEYS[1],ARGV[1]);return redis.call('incr', KEYS[1]);" 1 testkeyincr11 1000
net::E_CMD_STATUS StepRedisMysqlTest::Emit_lua2()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	std::string cmdstr = "redis.call('setnx',KEYS[1],ARGV[1]);return redis.call('incr', KEYS[1]);";
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	net::RedisOperator oMemOper(0,cmdstr, "eval");//redis 指令
	oMemOper.AddRedisField("1");
	oMemOper.AddRedisField("testkeyincr11");
	oMemOper.AddRedisField("10000");
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}


net::E_CMD_STATUS StepRedisMysqlTest::Emit_load_user_from_mysql_redis()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	char szRedisKey[64] = {0};
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	snprintf(szRedisKey, sizeof(szRedisKey), "%d:%d:%d", REDIS_T_HASH, IM_DATA_TEST, m_uiID);
	net::MemOperator oMemOper(
			m_uiID,
		TEST_TABLE,
		DataMem::MemOperate::DbOperate::SELECT,
		szRedisKey, "hmset", "hmget");//redis 查询和回写指令
	oMemOper.AddField("id");
	oMemOper.AddField("name");//所有字段需要根据配置补全
	oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ, "id", std::to_string(m_uiID), DataMem::INT);//mysql 查询条件
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepRedisMysqlTest::Emit_load_user_from_redis()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	char szRedisKey[64] = {0};
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	snprintf(szRedisKey, sizeof(szRedisKey), "%d:%d:%d", REDIS_T_HASH, IM_DATA_TEST, m_uiID);
	net::RedisOperator oMemOper(
			m_uiID,
		szRedisKey, "", "hmget");//redis 查询指令
	oMemOper.AddRedisField("id");
	oMemOper.AddRedisField("name");//所有字段需要根据配置补全
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}


net::E_CMD_STATUS StepRedisMysqlTest::Emit_load_user_from_mysql()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	net::DbOperator oMemOper(
			m_uiID,
		TEST_TABLE,
		DataMem::MemOperate::DbOperate::SELECT);
	oMemOper.AddDbField("id");//所有字段需要根据配置补全
	oMemOper.AddDbField("name");
	oMemOper.AddCondition(DataMem::MemOperate::DbOperate::Condition::EQ, "id", std::to_string(m_uiID), DataMem::INT);//mysql 查询条件
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepRedisMysqlTest::Emit_set_user_from_redis()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	char szRedisKey[64] = {0};
	MsgHead oMsgHead;
	MsgBody oMsgBody;
	snprintf(szRedisKey, sizeof(szRedisKey), "%d:%d:%d", REDIS_T_HASH, IM_DATA_TEST, m_uiID);
	net::RedisOperator oMemOper(
			m_uiID,
		szRedisKey, "hmset");//redis 写指令
	oMemOper.AddRedisField("id",1);
	oMemOper.AddRedisField("name","ken");//所有字段需要根据配置补全
	oMsgBody.set_body(oMemOper.MakeMemOperate()->SerializeAsString());
	oMsgHead.set_cmd(net::CMD_REQ_STORATE);
	oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
	oMsgHead.set_seq(GetSequence());
	if (!GetLabor()->SendToNext("REDISAGENT", oMsgHead, oMsgBody))
	{
		LOG4_ERROR("send to REDISAGENT error!");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");//发送失败,返回错误
		return(net::STATUS_CMD_FAULT);//状态错误，放弃执行
	}
	return(net::STATUS_CMD_RUNNING);
}

net::E_CMD_STATUS StepRedisMysqlTest::Callback(
                    const net::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data)
{
    LOG4_TRACE("seq[%llu] StepRedisMysqlTest::Callback ok!", oInMsgHead.seq());
    if(net::CMD_RSP_SYS_ERROR == oInMsgHead.cmd())//系统错误（如没有该指令）
	{
    	LOG4_ERROR("system response error");
    	Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");
		return net::STATUS_CMD_FAULT;
	}
	DataMem::MemRsp oRsp;
	if(!oRsp.ParseFromString(oInMsgBody.body()))
	{
		LOG4_ERROR("parse protobuf data fault");
		Response(robot::ERR_SERVERINFO,"ERR_SERVERINFO");
		return net::STATUS_CMD_FAULT;
	}
	//读存储出错
	if(0 != oRsp.err_no())
	{
		if(oRsp.has_err_msg())
		{
			LOG4_ERROR("Callback error %d: %s!",oRsp.err_no(),oRsp.err_msg().c_str());
		}
		else
		{
			LOG4_ERROR("Callback error %d!", oRsp.err_no());
		}
		Response(oRsp.err_no(),oRsp.err_msg());
		return net::STATUS_CMD_FAULT;
	}
    if (eStage_load_user_from_mysql_redis == m_istage)
    {
    	LOG4_TRACE("eStage_load_user_from_mysql_redis ok %s",oRsp.DebugString().c_str());
    	util::CJsonObject objJson;
		if (oRsp.record_data_size())
		{
			for(int i = 0; i < oRsp.record_data_size();++i)
			{
				for(int j = 0;j < oRsp.record_data(i).field_info_size() ;++j)
				{
					objJson.Add(oRsp.record_data(i).field_info(j).col_value());
				}
			}
		}
		m_strResponseData_from_mysql_redis = objJson.ToString();
		LOG4_TRACE("m_strResponseData_from_mysql_redis %s",m_strResponseData_from_mysql_redis.c_str());
    	//切换下一个状态
		m_istage = eStage_load_user_from_mysql;
    }
    else if (eStage_load_user_from_mysql == m_istage)
    {
    	LOG4_TRACE("eStage_load_user_from_mysql ok %s",oRsp.DebugString().c_str());
    	util::CJsonObject objJson;
		if (oRsp.record_data_size())
		{
			for(int i = 0; i < oRsp.record_data_size();++i)
			{
				for(int j = 0;j < oRsp.record_data(i).field_info_size() ;++j)
				{
					objJson.Add(oRsp.record_data(i).field_info(j).col_value());
				}
			}
		}
    	m_strResponseData_from_mysql = objJson.ToString();
		LOG4_TRACE("eStage_load_user_from_mysql %s",m_strResponseData_from_mysql.c_str());
    	//切换下一个状态
		m_istage = eStage_load_user_from_redis;
    }
    else if (eStage_load_user_from_redis == m_istage)
    {
    	LOG4_TRACE("eStage_load_user_from_redis ok %s",oRsp.DebugString().c_str());
    	util::CJsonObject objJson;
		if (oRsp.record_data_size())
		{
			for(int i = 0; i < oRsp.record_data_size();++i)
			{
				for(int j = 0;j < oRsp.record_data(i).field_info_size() ;++j)
				{
					objJson.Add(oRsp.record_data(i).field_info(j).col_value());
				}
			}
		}
		m_strResponseData_from_redis = objJson.ToString();
		LOG4_TRACE("m_strResponseData_from_redis %s",m_strResponseData_from_redis.c_str());
		//切换下一个状态
		m_istage = eStage_end;
    }
    else if (eStage_redis_lua == m_istage)
    {
    	LOG4_TRACE("eStage_redis_lua ok %s",oRsp.DebugString().c_str());
		util::CJsonObject objJson;
		if (oRsp.record_data_size())
		{
			for(int i = 0; i < oRsp.record_data_size();++i)
			{
				for(int j = 0;j < oRsp.record_data(i).field_info_size() ;++j)
				{
					objJson.Add(oRsp.record_data(i).field_info(j).col_value());
				}
			}
		}
		m_strResponseData_from_redis = objJson.ToString();
		LOG4_TRACE("m_strResponseData_from_redis %s",m_strResponseData_from_redis.c_str());
    	//切换下一个状态
		m_istage = eStage_end;
    }
	LOG4_TRACE("%s()", __FUNCTION__);
	return(Emit(robot::ERR_OK));
}


void StepRedisMysqlTest::Response(int err_no,const std::string& msg)
{
	util::CJsonObject oJsonObj;
	oJsonObj.Add("code", err_no);
	oJsonObj.Add("msg", msg);
	SendToClient(oJsonObj.ToString());
}

net::E_CMD_STATUS StepRedisMysqlTest::Timeout()
{
	LOG4_WARN("%s()", __FUNCTION__);
	if (m_iTimeoutNum++ > 3)
	{
		LOG4_TRACE("%s()", __FUNCTION__);//超时3次放弃
		return(net::STATUS_CMD_FAULT);
	}
	return(Emit(robot::ERR_OK));//超时重试
}

} /* namespace im */
