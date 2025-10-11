/*
 * HelloSession.h
 *
 *  Created on: 2015年10月21日
 *      author   Tommy
 */
#ifndef CODE_HELLOSERVER_SRC_HELLOSESSION_H_
#define CODE_HELLOSERVER_SRC_HELLOSESSION_H_
#include <string>
#include <map>
#include <set>

#include "RobotError.h"
#include "util/json/CJsonObject.hpp"
#include "dbi/MysqlDbi.hpp"
#include "session/Session.hpp"
#include "NetDefine.hpp"
#include "NetError.hpp"
#include "step/Step.hpp"
#include "cmd/Cmd.hpp"


#define HELLO_SESSIN_ID (20000)

namespace im
{

class HelloSession: public net::Session
{
public:
	explicit HelloSession(const std::string& strSessionId, ev_tstamp dSessionTimeout = 5.0,
	    		const std::string& strSessionClass = SessionClass()):
	    	net::Session(strSessionId, dSessionTimeout,strSessionClass)
	{
	}
    virtual ~HelloSession() {}
    static const std::string SessionClass(){return std::string("im::HelloSession");}
    bool Init(const util::CJsonObject& conf);
    void LoadConfig(const util::CJsonObject& conf);
    bool GetConfig(const std::string& strFile,util::CJsonObject& conf);

    net::E_CMD_STATUS Timeout();

    void TestLog();
    bool m_boTestLogInit = false;

    void TestLog2();
    bool m_boTest2LogInit = false;

    void TestLog3();
    bool m_boTest3LogInit = false;

	void TestCat();
	void TestCatFailed();
	void TestRebootProcess();

	bool m_boTestCat = false;

    void SetCurrentTime()
    {
        m_uiCurrentTime = ::time(NULL);
    }
    const util::CJsonObject & GetLocateDataRequest() {return m_objModuleLocateDataRequest;}
    //权限字段
    const std::string&  GetAccessControlAllowOrigin()const {return m_AccessControlAllowOrigin;}
    const std::string&  GetAccessControlAllowHeaders()const {return m_AccessControlAllowHeaders;}
    const std::string&  GetAccessControlAllowMethods()const {return m_AccessControlAllowMethods;}
    uint32 GetValidTimeDelay()const {return m_ValidTimeDelay;}

    void IncrRecv()
    {
        if (m_recvCounter & 0x100000000)//4294967296
        {m_recvCounter = 0;m_succCounter = 0;}
        ++m_recvCounter;
    }
    void IncrSucc(){++m_succCounter;}
    uint64 GetRecv()const{return m_recvCounter;}
    uint64 GetSucc()const{return m_succCounter;}
    uint64 GetFailed()const{return m_recvCounter - m_succCounter;}
private:
    bool boInit = false;
    uint64 m_recvCounter = 0;
    uint64 m_succCounter = 0;
    uint32 m_ValidTimeDelay = 0;
    uint64 m_uiCurrentTime = ::time(NULL); //当前时间
    /*
     *数据库连接配置,如：
       "dbip":"192.168.18.68",
       "dbport":3395,
       "dbuser":"robot",
       "dbpwd":"robot123456",
       "dbname":"db_im3_center",
       "dbcharacterset":"utf8",
     * */
    util::CJsonObject m_objModuleLocateDataRequest;
    std::string m_AccessControlAllowOrigin;
    std::string m_AccessControlAllowHeaders;
    std::string m_AccessControlAllowMethods;
};

inline HelloSession* GetHelloSession() {return net::GetGlobalConfigSession<HelloSession>("HelloDynamic.json",5.0);}

}
;

#endif /* CODE_HELLOSERVER_SRC_HELLOSESSION_H_ */
