/*******************************************************************************
 * Project:  LoginServer
 * @file     ModuleHello.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_ModuleHello_ModuleHello_HPP_
#define SRC_ModuleHello_ModuleHello_HPP_
#include <map>

#include "StarshipError.h"
#include "StarshipErrorMapping.h"
#include "google/protobuf/util/json_util.h"
#include "google/protobuf/map.h"
#include "google/protobuf/any.pb.h"
#include "test_proto3.pb.h"
#include "util/encrypt/base64.h"
#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "../HelloSession.h"

#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace starshiplib
{

class ModuleHello: public oss::Module
{
public:
    ModuleHello();
    virtual ~ModuleHello();
    virtual bool Init();
    virtual bool AnyMessage(
                    const oss::tagMsgShell& stMsgShell,
                    const HttpMsg& oInHttpMsg);
private:
    void Response(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,int iCode);

    void QueryFromPostgres(const oss::tagMsgShell& stMsgShell,
            const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType);

    //redis basic
    void SetValueFromRedis(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType="PROXY");
    void OnlySetValueFromRedis(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType="PROXY");
    void OnlyGetValueFromRedis(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &nodeType="PROXY");

    //redisearch http://redisearch.io/Commands/
    void RedisearchAdd(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sDoc,const std::string &sValue);
    void RedisearchSearch(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);

    //redis geo
    void RedisGEOADD(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);
    void RedisGEORADIUSBYMEMBER(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue);

    //redis bitmap
#define  SETBIT_KEY "4:4:SETBIT"
#define PROXY "PROXYSSDB"
    void RedisbitmapSETBIT(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=PROXY);
    void RedisbitmapGETBIT(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=PROXY);
    void RedisbitmapBITPOS(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=PROXY);
    void RedisbitmapGET(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=SETBIT_KEY,const std::string &sNode=PROXY);
    void RedisbitmapGET_GET(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey1=SETBIT_KEY,const std::string &sKey2=SETBIT_KEY,const std::string &sNode=PROXY);

    static void String2UserData(const std::string & col_value,std::vector<uint32>& usersData,log4cplus::Logger logger);
    static void OPUserData(const std::vector<uint32>& usersData1,const std::vector<uint32>& usersData2,log4cplus::Logger logger);

#define  MSG_KEY "1:11:MSG"
    void SsdbMsgHset(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sValue,const std::string &sKey=MSG_KEY,const std::string &sNode=PROXY);
    void SsdbMsgHsetall(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &sKey=MSG_KEY,const std::string &sNode=PROXY);
    void SsdbMsgHscan(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &key_start,const std::string &sKey=MSG_KEY,const std::string &sNode=PROXY);

    //db
    void TestDBSELECT(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);

    //crytopp
    void TestRSA();

    //Coroutinue
    void TestCoroutinue();
    void TestStepCoFuncDataProxy(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &str);

    //stage machine
    bool TestHttpRequestState(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
	bool TestHttpRequestStateFunc(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
	bool TestHttpRequestStateFuncDataProxy(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg,const std::string &str);

    //pb
    void Base64Encode(const char* data,unsigned int datalen,std::string &strEncode);
    void Base64Decode(const char* data,unsigned int datalen,std::string &strDecode);
    void PrintBin(const char* data,unsigned int datalen);
    bool TestJson2pb(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    void TestProto3Type();
    void TestJson2pbRepeatedFields();

    CustomClock m_CustomClock;
};

} /* namespace starshiplib */

#endif /* SRC_ModuleHello_ModuleHello_HPP_ */
