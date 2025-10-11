/*******************************************************************************
 * Project:  Hello
 * @file     ModuleHello.hpp
 * @brief 
 * @author   Tommy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_ModuleHello_ModuleHello_HPP_
#define SRC_ModuleHello_ModuleHello_HPP_
#include <map>

#include "RobotError.h"
#include "google/protobuf/util/json_util.h"
#include "google/protobuf/map.h"
#include "google/protobuf/any.pb.h"
#include "test_proto3.pb.h"
#include "util/encrypt/base64.h"
#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "HelloSession.h"
#include "StepMongoTest.hpp"
#include "StepRedisMysqlTest.hpp"
#include "util/CommonUtils.hpp"

namespace core
{
#define GET_TOKEN_GEN (10001)

class ModuleHello: public net::Module
{
public:
    ModuleHello() = default;
    virtual ~ModuleHello();
    virtual bool Init();
    void Tests();
    void TestLoadConfigFromCustom();
    void TestLog();
    bool boTestLog = true;

    bool TestMsg(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
private:
    void Response(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,int iCode);
    void TestBson();

    //DbAgent
	#define PG_TB_TEST "tb_test"
    void DbAgent_SELECT(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    void DbAgent_Insert(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType);
    void DbAgent_SetGet(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType);
    void DbAgent_Get(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType);
    void DbAgent_Set(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType);
    void DbAgent_AddUp(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,uint32 id,const std::string &sName,uint32 sum,const std::string &nodeType);

	#define TEST_REDIS_KEY "1:2:testkey"
    //redis basic
    void Redis_SetGet(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType=REDISAGENT_NODE);
    void Redis_Set(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType=REDISAGENT_NODE);
    void Redis_Get(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &nodeType=REDISAGENT_NODE);
    void Redis_GetPineline(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &nodeType=REDISAGENT_NODE);
    void RedisMysql_Step(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);

    //redisearch http://redisearch.io/Commands/
    void Redis_SearchAdd(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sDoc,const std::string &sValue);
    void Redis_SearchSearch(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);

    //redis geo
    void Redis_GEOADD(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);
    void Redis_GEORADIUSBYMEMBER(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);

    //redis bitmap
	#define  SETBIT_KEY "4:4:SETBIT"
    void Redis_bitmapSETBIT(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_bitmapGETBIT(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_bitmapBITPOS(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_bitmapGET(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_bitmapGET_GET(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey1=SETBIT_KEY,const std::string &sKey2=SETBIT_KEY,const std::string &sNode=REDISAGENT_NODE);

    static void String2UserData(const std::string & col_value,std::vector<uint32>& usersData);
    static void OPUserData(const std::vector<uint32>& usersData1,const std::vector<uint32>& usersData2);

    //redis hash
	#define  MSG_KEY "1:11:MSG"
    void Redis_Hset(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=MSG_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_Hsetall(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sKey=MSG_KEY,const std::string &sNode=REDISAGENT_NODE);
    void Redis_Hscan(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &key_start,const std::string &sKey=MSG_KEY,const std::string &sNode=REDISAGENT_NODE);

	#define tb_coordinate "imtest"// "tb_coordinate"
    void Mongo_Insert(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &nodeType=MONGOAGENT_NODE);
    void Mongo_Upsert(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &nodeType=MONGOAGENT_NODE);
    void Mongo_Step(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);

    //hiredis_vip
    void hiredis_vip_test_format_commands();

    //crytopp
    void TestRSA();

    //Coroutinue
    void TestCoroutinue();
    void TestCoroutinueAuto();
    void TestStepCoFuncDataRedisAgent(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &str);

    //stage machine
    bool TestHttpRequestState(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
	bool TestHttpRequestStateFunc(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
	bool TestHttpRequestStateFuncDataRedisAgent(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &str);

    //pb
    void Base64Encode(const char* data,unsigned int datalen,std::string &strEncode);
    void Base64Decode(const char* data,unsigned int datalen,std::string &strDecode);
    void PrintBin(const char* data,unsigned int datalen);
    bool TestJson2pb(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    void TestProto3Type();
    void TestJson2pbRepeatedFields();

    void GenKey(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
	void VerifyKey(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);

    bool boTests = false;
    net::RunClock m_RunClock;
};

} /* namespace core */

#endif /* SRC_ModuleHello_ModuleHello_HPP_ */
