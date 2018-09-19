/*******************************************************************************
 * Project:  LogQueue
 * @file    Comm.hpp
 * @brief    
 * @author   cjy
 * @date:    2015年9月16日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_LOGQUEUE_COMM_HPP_
#define SRC_LOGQUEUE_COMM_HPP_
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory.h>
#include <time.h>
#include <sys/time.h>
#include "behaviour.pb.h"
#include "util/json/CJsonObject.hpp"
#include "log4cplus/logger.h"
#include "log4cplus/fileappender.h"
#include "log4cplus/loggingmacros.h"
#include "util/http/HttpParamCodec.h"
#include "algorithm/Levenshtein.hpp"

namespace analysis
{

typedef wchar_t WCHAR;
typedef WCHAR *PWCHAR,*LPWCH,*PWCH,*NWPSTR,*LPWSTR,*PWSTR;
typedef const WCHAR *LPCWCH,*PCWCH,*LPCWSTR,*PCWSTR;
typedef char *PCHAR,*LPCH,*PCH,*NPSTR,*LPSTR,*PSTR;
typedef const char *LPCCH,*PCSTR,*LPCSTR;

typedef char CHAR;
typedef short SHORT;
typedef long LONG;
typedef char CCHAR, *PCCHAR;
typedef unsigned char UCHAR,*PUCHAR;
typedef unsigned short USHORT,*PUSHORT;
typedef unsigned long ULONG,*PULONG;
typedef char *PSZ;
//typedef CHAR TCHAR;
typedef CHAR _TCHAR;
typedef CHAR TBYTE,*PTCH,*PTBYTE;
typedef CHAR *LPTCH,*PTSTR,*LPTSTR,*LP,*PTCHAR;

typedef void *PVOID,*LPVOID;

typedef long long LONGLONG;
typedef unsigned long DWORD;

typedef int HANDLE;

#define INVALID_HANDLE_VALUE (HANDLE)(-1)

typedef char int8;
typedef unsigned char uint8;
typedef short int16;
typedef unsigned short uint16;
typedef int int32;
typedef unsigned int uint32;
typedef long long int int64;
typedef unsigned long long int uint64;

#define MAKEFOURCC(ch0, ch1, ch2, ch3) ((uint32)(uint8)(ch0) | ((uint32)(uint8)(ch1) << 8) | ((uint32)(uint8)(ch2) << 16) | ((uint32)(uint8)(ch3) << 24 ))

typedef std::map<std::string, std::string> HttpHead;
typedef HttpHead::value_type HttpHeadValue;

//函数运行时间计算类
class CustomClock
{
public:
    CustomClock()
    {
        m_desc = NULL;
        boStart = false;
    }
    CustomClock(const char* desc,const log4cplus::Logger &logger)
    {
        Start(desc,logger);
    }
    void Start(const char* desc,const log4cplus::Logger &logger)
    {
        if(!boStart)
        {
            m_desc = desc;
            m_logger = logger;
            StartClock();
            boStart = true;
        }
    }
    ~CustomClock()
    {
        EndClock();
    }
    void StartClock()
    {
        gettimeofday(&m_cBeginClock,NULL);
    }
    void EndClock()
    {
        if (boStart)
        {
            gettimeofday(&m_cEndClock,NULL);
            float useTime=1000000*(m_cEndClock.tv_sec-m_cBeginClock.tv_sec)+
                            m_cEndClock.tv_usec-m_cBeginClock.tv_usec;
            useTime/=1000;
            LOG4CPLUS_INFO_FMT(m_logger,"%s() CustomClock %s use time(%lf) ms",__FUNCTION__,m_desc,useTime);
            boStart = false;
        }
    }
    bool boStart;
    timeval m_cBeginClock;
    timeval m_cEndClock;
    const char* m_desc;
    log4cplus::Logger m_logger;
};


/*
数据记录头内容(一共2+2+2+2=8字节)
uint16 nCmdType;//指令类型
uint8 nDataVersion;//数据版本号(服务器)
uint8 nEncript;/// 标识压缩类型、或编码类型(其中高4位表示压缩算法（0表示不压缩），低4位表示加密算法（0表示不加密）)
uint16 nCrc;//冗余校验字段
uint16 nBodySize;//数据记录体大小
 * */
struct LogDataHeader
{
    LogDataHeader()
    {
        memset(this, 0, sizeof(LogDataHeader));
    }
    LogDataHeader(const LogDataHeader& header)
    {
        nLogCmd = header.nLogCmd;
        nDataVersion = header.nDataVersion;
        nEncript = header.nEncript;
        nCrc = header.nCrc;
        nBodySize = header.nBodySize;
    }
    LogDataHeader &operator =(const LogDataHeader &header)
    {
        nLogCmd = header.nLogCmd;
        nDataVersion = header.nDataVersion;
        nEncript = header.nEncript;
        nCrc = header.nCrc;
        nBodySize = header.nBodySize;
        return *this;
    }
    uint16 nLogCmd;//日志指令
    uint8  nDataVersion;//数据版本号
    uint8  nEncript;//标识压缩类型、或编码类型(其中高4位表示压缩算法（0表示不压缩），低4位表示加密算法（0表示不加密）)
    uint16 nCrc;//冗余校验字段
    uint16 nBodySize;//数据记录体大小
    static const uint8 VERSION = 1;
};

} /* namespace analysis */

#endif /* SRC_LOG_COMM_HPP_ */
