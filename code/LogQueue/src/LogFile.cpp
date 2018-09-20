#include "LogFile.h"

namespace analysis
{

const char LogFile::DataFileExt[] = ".dat";

class LogOpenHelper
{
public:
    int m_dataFd;

    LogOpenHelper()
    {
        m_dataFd = -1;
    }
    ~LogOpenHelper()
    {
        if (0 < m_dataFd)
            close(m_dataFd);
    }
};

LogFile::LogFile(): m_dataFd(-1)
{
}

LogFile::LogFile(const LogFile& logFile)
{
	m_strLogFileName = logFile.m_strLogFileName;
	m_strLogFileFullName = logFile.m_strLogFileFullName;
	m_dataFd = logFile.m_dataFd;
	m_oLogger = logFile.m_oLogger;
}
LogFile& LogFile::operator =( const LogFile &logFile )
{
	m_strLogFileName = logFile.m_strLogFileName;
	m_strLogFileFullName = logFile.m_strLogFileFullName;
	m_dataFd = logFile.m_dataFd;
	m_oLogger = logFile.m_oLogger;
	return *this;
}

LogFile::~LogFile()
{
}

int LogFile::GetFileSize()const
{
    if (m_dataFd <= 0)
    {
        LOG4CPLUS_WARN_FMT(m_oLogger,"invalid m_dataFd(%d)",m_dataFd);
        return -1;
    }
    struct stat fileStat;
    if (-1 == ::fstat(m_dataFd, &fileStat))
    {
        LOG4CPLUS_WARN_FMT(m_oLogger,"Can not query data file size,file no(%d) errno(%d),strerror(%s) m_strLogFileFullName(%s)",
                        m_dataFd,errno, strerror(errno),m_strLogFileFullName.c_str());
        return -1;
    }
    return fileStat.st_size;//在文件末尾处写
}

bool LogFile::ClearFile()
{
    //设置文件结束位置
    if (-1 == ::ftruncate(m_dataFd, 0))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,"Can not clear data file,file no(%d),errno(%d),strerror(%s)",m_dataFd, errno, strerror(errno));
        close(); //清除失败也需要关闭
        return false;
    }
    close();
    LOG4CPLUS_DEBUG_FMT(m_oLogger, "clear db file(%s) ok",m_strLogFileFullName.c_str());
    return true;
}

bool LogFile::open(const char* sLogName)
{
    if (IsOpened() && m_strLogFileName == sLogName)
    {
        LOG4CPLUS_INFO_FMT(m_oLogger, "Already opened:\"%s\"",m_strLogFileFullName.c_str());
        return true;
    }
    //关闭之前打开文件
    close();
    m_strLogFileName = sLogName;
    LogOpenHelper openHelper;
    //以读写方式打开数据文件
    m_strLogFileFullName = m_strLogFileName + DataFileExt;
    openHelper.m_dataFd = ::open(m_strLogFileFullName.c_str(), O_RDWR, 0);
    if (-1 == openHelper.m_dataFd)
    {
        LOG4CPLUS_TRACE_FMT(m_oLogger, "Can not open DataFile \"%s\"",m_strLogFileFullName.c_str());
        return false;
    }
    //将文件句柄以及文件头保存到类中
    m_dataFd = openHelper.m_dataFd;
    LOG4CPLUS_TRACE_FMT(m_oLogger, "succ to open DataFile \"%s\"",m_strLogFileFullName.c_str());
    return true;
}

void LogFile::close()
{
    if (m_dataFd > 0)
    {
        ::fsync(m_dataFd);
        ::close(m_dataFd);
        m_dataFd = -1;
    }
}

bool LogFile::create(const char* pLogName)
{
    LogOpenHelper helper;
    m_strLogFileName = pLogName;
    m_strLogFileFullName = m_strLogFileName + DataFileExt;
    //数据文件已经存在则不能再创建
    if (util::IsArchive(m_strLogFileFullName.c_str()))
    {
        LOG4CPLUS_WARN_FMT(m_oLogger,
                        "data file \"%s\" already exists",
                        m_strLogFileFullName.c_str());
        if (0 != util::RemoveFile(m_strLogFileFullName.c_str()))
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,"data file \"%s\" already exists,failed to RemoveFile",m_strLogFileFullName.c_str());
            return false;
        }
    }
    //逐层创建文件库目录，例如指定文件库./FD/db1/file1，则需要创建目录./FD以及./FD/db1。
    char sPathDir[128];
    util::ExtractFileDirectory(pLogName, sPathDir, sizeof(sPathDir));
    LOG4CPLUS_DEBUG_FMT(m_oLogger, "create dir %s", sPathDir);
    if (!util::DeepCreateDirectory(sPathDir))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger, "Can not create dir \"%s\"", sPathDir);
        return false;
    }
    //创建数据文件
    LOG4CPLUS_DEBUG_FMT(m_oLogger, "create dataFile %s", m_strLogFileFullName.c_str());
    //不存在则创建.参数3用于指定文件的访问权限位
    //创建的文件文件拥有者可读写权限
    helper.m_dataFd = ::open(m_strLogFileFullName.c_str(), O_RDWR | O_CREAT |O_APPEND, S_IRUSR|S_IWUSR|S_IRGRP | S_IWGRP);
    if (-1 == helper.m_dataFd)
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,"Can not create new data file \"%s\",errno(%d),strerror(%s)",m_strLogFileFullName.c_str(), errno, strerror(errno));
        return false;
    }
    //关闭之前打开的文件
    close();
    //保存当前的文件库文件句柄为新打开的句柄
    m_dataFd = helper.m_dataFd;
    helper.m_dataFd = -1;
    return true;
}

bool LogFile::WriteLog(const char* lpDataBuffer,int nLogSize)
{
    struct stat fileStat;
    if (-1 == ::fstat(m_dataFd, &fileStat))
    {
        LOG4CPLUS_WARN_FMT(m_oLogger,
                        "Can not query data file size,file no(%d) errno(%d),strerror(%s) m_strLogFileFullName(%s)",
                        m_dataFd,errno, strerror(errno),m_strLogFileFullName.c_str());
        return false;
    }
    uint64 nOffset = fileStat.st_size;//在文件末尾处写
    if (!WriteDataFile(nOffset, lpDataBuffer, nLogSize))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "failed to Write data body to data file,position(%llu),file no(%d),errno(%d),strerror(%s) m_strLogFileFullName(%s)",
                        nOffset, m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
        return false;
    }
    LOG4CPLUS_TRACE_FMT(m_oLogger,"%s() write log nOffset(%d) nLogSize(%d) m_strLogFileFullName(%s)",
                    __FUNCTION__,nOffset,nLogSize,m_strLogFileFullName.c_str());
    nOffset += nLogSize;
    if (nOffset)
    {
        //调整文件指针到数据块大小单元处（这里是文件名末尾处）
        uint64 lNewSize = lseek(m_dataFd, nOffset, SEEK_SET);
        if (0 >= lNewSize)
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,
                            "Can not query data file size,position(%llu),file no(%d),errno(%d),strerror(%s) m_strLogFileFullName(%s)",
                            nOffset, m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
            return false;
        }
        //设置文件结束位置（linux下文件没有文件结束符）
        if (-1 == ::ftruncate(m_dataFd, nOffset))
        {
            LOG4CPLUS_WARN_FMT(m_oLogger,
                            "Can not set end of data file,position(%llu),file no(%d),errno(%d),strerror(%s) m_strLogFileFullName(%s)",
                            nOffset, m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
            return false;
        }
    }
    return true;
}

bool LogFile::WriteDataFile(uint64 nOffset, const char* lpBuffer, uint64 dwSize)
{
    static const uint32 OnceWriteBytes = 0x10000; //每次写文件的最大字节数65536,
    if (-1 == ::lseek(m_dataFd, (long) nOffset, SEEK_SET))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "Fatal error can not set data file pointer,position(%llu),file no(%d),errno(%d),strerror(%s) logDataName(%s)",
                        nOffset, m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
        return false;
    }
    size_t dwBytesToWrite, dwBytesWriten;
    const char* ptr = (const char*) lpBuffer;
    while (dwSize > 0)
    {
        if (dwSize > OnceWriteBytes)
            dwBytesToWrite = OnceWriteBytes;
        else
            dwBytesToWrite = (uint64) dwSize;
        //open文件未设置O_SYNC同步写时，write操作只是把数据写入内存缓存区就返回了，数据会在以后的某个时间将队列中的数据写入磁盘
        //此时，write系统调用和写入磁盘的操作是异步执行
        dwBytesWriten = ::write(m_dataFd, ptr, dwBytesToWrite);
        if (0 >= dwBytesWriten)
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,
                            "Fatal error can not write data file,errno(%d),strerror(%s) logDataName(%s)",
                            errno, strerror(errno),m_strLogFileFullName.c_str());
            return false;
        }
        ptr += dwBytesWriten;
        dwSize -= dwBytesWriten;
    }
//    fsync(m_dataFd);
    return true;
}


bool LogFile::AppendDataFile(const char* lpBuffer, uint64 dwSize)
{
    static const uint32 OnceWriteBytes = 0x10000; //每次写文件的最大字节数65536,
    size_t dwBytesToWrite, dwBytesWriten;
    const char* ptr = (const char*) lpBuffer;
    while (dwSize > 0)
    {
        if (dwSize > OnceWriteBytes)
            dwBytesToWrite = OnceWriteBytes;
        else
            dwBytesToWrite = (uint64) dwSize;
        //open文件未设置O_SYNC同步写时，write操作只是把数据写入内存缓存区就返回了，数据会在以后的某个时间将队列中的数据写入磁盘
        //此时，write系统调用和写入磁盘的操作是异步执行
        dwBytesWriten = ::write(m_dataFd, ptr, dwBytesToWrite);
        if (0 >= dwBytesWriten)
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,
                            "Fatal error can not write data file,errno(%d),strerror(%s) logDataName(%s)",
                            errno, strerror(errno),m_strLogFileFullName.c_str());
            return false;
        }
        LOG4CPLUS_TRACE_FMT(m_oLogger,"%s() write log dwBytesWriten(%d) dwSize(%llu) logDataName(%s)",
                            __FUNCTION__,dwBytesWriten,dwSize,m_strLogFileFullName.c_str());
        ptr += dwBytesWriten;
        dwSize -= dwBytesWriten;
    }
    return true;
}

bool LogFile::ReadDataFile(uint64 nOffset, char* lpBuffer, uint64 dwSize) const
{
    static const uint32 OnceReadBytes = 0x10000;    //每次读文件的字节数
    if (-1 == ::lseek(m_dataFd, nOffset, SEEK_SET))
    {
        LOG4CPLUS_WARN_FMT(m_oLogger,
                        "ReadDataFile:Fatal error can not set data file pointer,position(%llu),file no(%d),errno(%d),strerror(%s) logDataName(%s)",
                        nOffset, m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
        return false;
    }
    uint32 dwBytesToRead = 0;
    int dwBytesReaded = 0;
    uint64 dwTotalRead = 0;
    char* ptr = lpBuffer;
    while (dwSize > dwTotalRead)
    {
        if (dwSize > OnceReadBytes)
            dwBytesToRead = OnceReadBytes;
        else
            dwBytesToRead = (uint64) dwSize;
        dwBytesReaded = ::read(m_dataFd, ptr, dwBytesToRead);
        if (dwBytesReaded <= 0)
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger,
                            "ReadDataFile:Fatal error can not read data file,file no(%d),errno(%d),strerror(%s) logDataName(%s)",
                            m_dataFd, errno, strerror(errno),m_strLogFileFullName.c_str());
            return false;
        }
        ptr += dwBytesReaded;
        dwTotalRead += dwBytesReaded;
    }
    return true;
}


}
//namespace analysis

