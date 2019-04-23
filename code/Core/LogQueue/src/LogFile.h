#ifndef __LOG_FILE__
#define __LOG_FILE__
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <errno.h>
#include <memory.h>
#include <string.h>
#include "util/CommonUtils.hpp"
#include "util/UnixTime.hpp"
#include "util/FileUtil.h"
#include "Comm.hpp"
#include "labor/Labor.hpp"

namespace core
{

class LogFile
{
    static const char DataFileExt[]; //数据文件后缀
public:
    LogFile();
    LogFile(const LogFile& logFile);
    LogFile &operator =( const LogFile &logFile );
    ~LogFile();
    bool IsOpened()const{return (m_dataFd > 0);}
    int GetFileSize()const;
    bool ClearFile();
    bool open(const char* sLogName);//打开文件库
    void close();//关闭文件库
    bool create(const char* sLogName);//创建文件库

    bool ArchiveExist()const{return util::IsArchive(m_logDataName.c_str());}
    bool WriteLog(const char* lpDataBuffer,int nLogSize);//写一个日志记录到日志文件
    bool AppendDataFile(const char* lpBuffer, uint64 dwSize);
    int SyncDataLog(){return ::fsync(m_dataFd);}
    int GetFileFD()const {return m_dataFd;}
    std::string             m_logName;//日志文件（不带后缀名,含路径）
    std::string             m_logDataName;//日志数据文件(含后缀名,含路径)
    int                     m_dataFd;
    log4cplus::Logger       m_oLogger;
private:
    //向数据文件中写数据（任意数据）。未同步到磁盘，可后续手动调用sync函数
    bool WriteDataFile(uint64 nOffset, const char* lpBuffer, uint64 dwSize);
    //从数据文件中读数据
    bool ReadDataFile(uint64 nOffset, char* lpBuffer, uint64 dwSize) const;
};

}

#endif
