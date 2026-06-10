/*******************************************************************************
 * Project:  Hello
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

#include "RobotError.h"
#include "google/protobuf/util/json_util.h"
#include "google/protobuf/map.h"
#include "google/protobuf/any.pb.h"
#include "util/encrypt/base64.h"
#include "cmd/Module.hpp"
#include "cmd/Cmd.hpp"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "HelloSession.h"
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
    
    //crytopp
    void TestRSA();

    // C++20 coroutine
    bool TestHttpRequestCo(const net::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    bool TestHelloPoolCpu(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    bool TestHelloPoolBlock(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);
    /// JSON option TestHelloCoRedis / TestHelloCoMysql（需本机或 compose 中 redis、mysql）
    bool TestHelloCoRedis(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg,
                          const util::CJsonObject& oJson);
    bool TestHelloCoMysql(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg,
                          const util::CJsonObject& oJson);

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
};

} /* namespace core */

#endif /* SRC_ModuleHello_ModuleHello_HPP_ */
