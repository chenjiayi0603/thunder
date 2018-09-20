#ifndef __LOGQUEUE_LOCALFILEMGR__
#define __LOGQUEUE_LOCALFILEMGR__
#include <time.h>
#include "google/protobuf/util/json_util.h"
#include "behaviour.pb.h"
#include "util/CommonUtils.hpp"
#include "util/UnixTime.hpp"
#include "util/FileUtil.h"
#include "algorithm/CalcCrc.hpp"
#include "Comm.hpp"
#include "LogFile.h"
#include "labor/Labor.hpp"
#include "step/StepNode.hpp"

//日志队列长度限制
#define LOG_MSG_MAX (100)

namespace analysis
{

//日志记录列表
typedef std::vector<behaviour::behaviour> LogMsgVec;
enum elog_cmd
{
	elog_cmd_trace = 1,
	elog_cmd_device = 2,
	elog_cmd_user = 3,
};
/*
vecComsumeTables消费文件格式
{
	"log_cmd":1,
	"log_type":"startTrace",
	"fields": [
		 "event_id",
		 "session_id",
		 "user_id",
		 "device_id"
	],
	"comsume_head":{"index":{"_index":"db_start_trace","_type":"tb_start_trace"}}
}
 * */
//本地文件管理类
class FileMgr
{
public:
    FileMgr();
    ~FileMgr();
    /*配置*/
    void Init(net::Labor* pOssLabor,const log4cplus::Logger& logger,const std::string &strWorkerIdentify)
    {m_pOssLabor = pOssLabor;m_oLogger = logger;m_logFile.SetLogger(logger);SetCurrentTime();m_strWorkerIdentify = strWorkerIdentify;}
    uint32 SetCurrentTime(){m_uiCurrentTime = ::time(NULL);return m_uiCurrentTime;}
    std::vector<util::CJsonObject> m_vecComsumeTables;//消费文件格式
    /*日志接口*/
    bool OpenLog();//打开日志文件
    bool AddLog(const behaviour::behaviour &logMsg);//追加日志记录到日志队列追加列表
    void CloseLog();//关闭当前打开的文件库
    bool CheckSync();//检查同步
    bool RoutineWrite(bool boForceNewLog = false);//文件写入操作历程(定时写入日志文件、或者日志队列数量达到指定数量时写入日志文件)
    bool IsAllWriteComplete()const{return (m_logMsgVec.size() == 0);}//是否所有的更新操作均已完成
    uint32 GetAllWriteSize()const{return m_logMsgVec.size();};//所有可写的日志数
    bool IsLogFileOpened()const {return m_logFile.IsOpened();}
    bool NeedNewLogCycle(uint32 uiCurrentTime)const
    {return (m_uiCreateLogLastTime + m_dCreateLogInterval <= uiCurrentTime) && (m_logFile.GetFileSize() > 0);}//旧文件存在且大小大于0 时 才创建新文件
    bool NeedWriteLogCycle(uint32 uiCurrentTime)const
    {return (m_uiWriteLogLastTime + m_dWriteLogInterval < uiCurrentTime) || (m_logMsgVec.size() >= m_uiLogQueueNum);}

    double m_dWriteLogInterval; //定时写日志间隔
	double m_dCreateLogInterval; //创建日志文件间隔
	uint32 m_uiSyncLog;
	uint32 m_uiLogQueueNum;//单次写盘消息数
	uint32 m_uiLogFormat;

	std::string m_strConfigPath; //配置路径
	std::string m_strDatalogPath; //日志文件目录 路径
	std::string m_strWorkerIdentify;//工作者标识符

	LogFile m_logFile;  //日志文件
    uint32 m_uiCreateLogLastTime;
	uint32 m_uiWriteLogLastTime;
private:
    //自定义二进制头存储结构
    bool Write2CustomHeadLogFile(const std::string & logBody,uint32 nLogCmd);

    //类ES型存储结构消费文件
    bool Write2ESConsumeFile(const behaviour::behaviour& message);
    bool Write2ESConsumeFileWithLog(const std::string& logBody,const behaviour::behaviour& message);

    //ssdb存储ES型消费文件
    bool Write2SSDBConsumeFile(const behaviour::behaviour& message);
    bool Write2SSDBConsumeFileWithLog(const std::string& logBody,const behaviour::behaviour& message);

	bool m_boNeedSync;

	log4cplus::Logger m_oLogger;
	net::Labor* m_pOssLabor;
	net::BUFF_RW m_buff;

	uint32 m_uiCurrentTime; //当前时间
    uint64 m_uiWritedLogCounter;
    LogMsgVec m_logMsgVec;//日志记录队列
};

} //namespace analysis

#endif
