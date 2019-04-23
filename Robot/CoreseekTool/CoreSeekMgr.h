/*
 * CoreSeekMgr.h
 *
 *  Created on: 2015年11月24日
 *      Author: chen
 */
#ifndef CODE_CORESEEKMGR_H_
#define CODE_CORESEEKMGR_H_
#include <stdlib.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <map>
#include <set>
#include "log4cplus/logger.h"
#include "log4cplus/fileappender.h"
#include "log4cplus/loggingmacros.h"
#include "util/json/CJsonObject.hpp"
#include "unix_util/proctitle_helper.h"
#include "unix_util/process_helper.h"
#include "Comm.hpp"
#include "CoreSeekSession.h"

namespace robot
{
//从日志文件加载到db文件管理类

class CoreSeekMgr
{
public:
    CoreSeekMgr(const std::string& strConfFile);
    bool Init();
    bool loadServerConf();
    bool SetProcessName(const loss::CJsonObject& oJsonConf);
    bool InitLogger(const loss::CJsonObject& oJsonConf);
    void Run();
    void SetLogger(const log4cplus::Logger& oLogger);
    const log4cplus::Logger& GetLogger();
    void SetConfigPath();
private:
    bool m_bInitLogger;
    std::string m_strConfFile;              ///< 配置文件(启动时参数传入)
    std::string m_strWorkPath;              //工作目录
    std::string m_strConfigPath;            //配置目录
    loss::CJsonObject m_oLastConf;          ///< 上次加载的配置
    loss::CJsonObject m_oCurrentConf;       //< 当次加载的配置
    int m_iLogLevel;
    std::string m_strServerName;
    CoreSeekSession  m_CoreseekToolSession;       //会话
    log4cplus::Logger m_oLogger;
};

}//namespace robot


#endif /* CODE_CORESEEKMGR_H_ */
