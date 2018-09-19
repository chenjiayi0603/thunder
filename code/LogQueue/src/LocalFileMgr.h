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
struct BUFF_RW
{
    BUFF_RW(): m_pbuffer(NULL), size(0),indexW(0),indexR(0)
    {
    }
    ~BUFF_RW()
    {
        if (m_pbuffer)
        {
            ::free(m_pbuffer);
        }
    }
    char* m_pbuffer;
    uint32 size;
    uint32 indexW;
    uint32 indexR;
    inline void Clear()
    {
        indexR = indexW = 0;
    }
    inline void Resize(uint32 buffsize)
    {
        if (buffsize > 0)
        {
            if (size < buffsize)//容量只会扩大
            {
                m_pbuffer = (char*) ::realloc(m_pbuffer, buffsize);
                size = buffsize;
            }
        }
    }
    inline void Write(const char* data,int dataSize)
    {
        if ((indexW + dataSize) > size)
        {
            Resize(indexW + dataSize);
        }
        memcpy(m_pbuffer + indexW,data,dataSize);
        indexW += dataSize;
    }
    inline bool Read(char* data,unsigned int dataSize)
    {
        if ((indexR + dataSize) > indexW)
        {
            return false;
        }
        memcpy(data,m_pbuffer + indexR,dataSize);
        indexR += dataSize;
        return true;
    }
    inline char* Getbuff() const
    {
        return m_pbuffer;
    }
    inline char* GetWriteBuff() const
    {
        return m_pbuffer + indexW;
    }
    inline char* GetReadBuff() const
    {
        return m_pbuffer + indexR;
    }
    inline void AdvanceIndexW(uint32 s)
    {
        if (indexW + s <= size)
        {
            indexW += s;
        }
    }
    void RewindR(uint32 s)
    {
        if (s > 0 && indexR > s)
        {
            indexR -= s;
        }
    }

    uint32 GetIndexW()const {return indexW;}
    uint32 GetIndexR()const {return indexR;}
    uint32 GetSize()const {return size;}
    uint32 ReadDataLen()const
    {
        if (indexW > indexR)
        {
            return indexW - indexR;
        }
        return 0;
    }
};

//日志记录列表
typedef std::vector<behaviour::behaviour> LogMsgVec;

//本地文件管理类
class LocalFileMgr
{
public:
    LocalFileMgr();
    ~LocalFileMgr();
    /*
     * 配置处理
     * */
    void SetConfigPath(const std::string &configpath){m_strConfigPath = configpath;}
    void SetDatalogPath(const std::string & datalogPath){m_datalogPath = datalogPath;}
    void SetWorkerIdentify(const std::string & workerIdentify){m_strWorkerIdentify = workerIdentify;}
    void SetSyncLog(uint32 uiSyncLog){m_uiSyncLog = uiSyncLog;}
    void SetLogQueueNum(uint32 logQueueNum){m_logQueueNum = logQueueNum;}
    void SetLogFormat(uint32 uiLogFormat){m_uiLogFormat = uiLogFormat;}

    void setCurrentTime(){m_currentTime = ::time(NULL);}
    uint64 getCurrentTime(){return m_currentTime;}
    void SetLogger(const log4cplus::Logger& logger)
	{
		m_oLogger = logger;
		m_logFile.SetLogger(logger);
	}
    void SetLabor(net::Labor* pOssLabor)
    {
        m_pOssLabor = pOssLabor;
    }
    std::vector<util::CJsonObject> m_vecLogTables;
    //消费文件格式
    std::vector<util::CJsonObject> m_vecComsumeTables;
private:
    enum elog_cmd
    {
        elog_cmd_trace = 1,
        elog_cmd_device = 2,
        elog_cmd_user = 3,
    };
    //自定义二进制头存储结构
    bool Write2CustomHeadLogFile(const std::string & logBody,uint32 nLogCmd);

    //类ES型存储结构消费文件
    bool Write2ESConsumeFile(const behaviour::behaviour& message);
    bool Write2ESConsumeFileWithLog(const std::string& logBody,const behaviour::behaviour& message);

    //ssdb存储ES型消费文件
    bool Write2SSDBConsumeFile(const behaviour::behaviour& message);
    bool Write2SSDBConsumeFileWithLog(const std::string& logBody,const behaviour::behaviour& message);

    std::string m_strConfigPath; //配置路径
	std::string m_datalogPath; //日志文件路径
	uint32 m_uiSyncLog;
	uint32 m_logQueueNum;
	uint32 m_uiLogFormat;

	std::string m_strWorkerIdentify;//工作者标识符
	bool m_boNeedSync;

	uint64 m_currentTime; //当前时间
	log4cplus::Logger m_oLogger;
	net::Labor* m_pOssLabor;
	BUFF_RW m_buff;
public:
    /*
     * 日志文件处理
     * */
    //打开日志文件
    bool OpenLog(const std::string &configPath, const std::string& sLogFilePath,int logQueueNum);
    bool OpenNewLog()
    {
        RoutineAllWrite();//在创建新日志文件前需要把之前的日志文件全部写入上一个文件
        //创建新的日志文件（如果已经打开旧的日志文件则会关闭旧的日志文件）
        if(!OpenLog(m_strConfigPath,m_datalogPath,m_logQueueNum))
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,"failed to open new log file,datalogPath(%s)",m_datalogPath.c_str());
            return false;
        }
        LOG4CPLUS_DEBUG_FMT(m_oLogger,"open new log file,datalogPath(%s)",m_datalogPath.c_str());
        return true;
    }
    //关闭当前打开的文件库
    void CloseLog();
    //检查同步
    bool CheckSync();
    //文件写入操作历程(定时写入日志文件、或者日志队列数量达到指定数量时写入日志文件)
    bool RoutineWrite(bool boForceNewLog = false);
    //日志文件全部写入操作历程（在创建新日志文件时需要调用）
    void RoutineAllWrite();
    //是否所有的更新操作均已完成
    bool IsAllWriteComplete();
    //所有可写的日志数
    uint32 GetAllWriteSize();
    //追加日志记录到日志队列追加列表
    bool AddLog(const behaviour::behaviour &logMsg,bool boForceWrite = false,bool boForceNewLog = false);
    bool IsLogFileOpened()const {return m_logFile.IsOpened();}
    uint32 GetCreatLogLastTime()const {return m_createLogLastTime;}
    LogFile m_logFile;  //日志服务器写日志
private:
    uint32 m_createLogLastTime;
	LogMsgVec m_logMsgVec;//日志记录队列
public:
	bool TryOpenNewLog();
private:
	uint64 m_uiWritedLogCounter;
};

} //namespace im

#endif
