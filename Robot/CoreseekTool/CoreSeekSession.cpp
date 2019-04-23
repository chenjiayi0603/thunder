/*
 * CoreSeekSession.cpp
 *
 *  Created on: 2015年11月24日
 *      Author: chen
 */
#include "CoreSeekSession.h"

namespace robot
{
bool CoreSeekSession::Init(const std::string &configPath,
                const std::string &strWorkPath,const log4cplus::Logger& oLogger)
{
    if (boInit)
    {
        return true;
    }
    SetLogger(oLogger);
    m_strConfigPath = configPath;
    m_strWorkPath = strWorkPath;
    //配置文件路径查找
    std::string strConfFile = m_strConfigPath + std::string("/CoreseekTool.json");
    LOG4CPLUS_DEBUG_FMT(m_oLogger, "CONF FILE = %s.", strConfFile.c_str());

    std::ifstream fin(strConfFile.c_str());
    //配置信息输入流
    if (fin.good())
    {
        //解析配置信息 JSON格式
        std::stringstream ssContent;
        ssContent << fin.rdbuf();
        if (!m_oCoreSeekCmdConf.Parse(ssContent.str()))
        {
            //配置文件解析失败
            LOG4CPLUS_ERROR_FMT(m_oLogger,
                            "Read conf (%s) error,it's maybe not a json file!",
                            strConfFile.c_str());
            ssContent.str("");
            fin.close();
            return false;
        }
    }
    else
    {
        //配置信息流读取失败
        LOG4CPLUS_ERROR_FMT(m_oLogger, "Open conf (%s) error!",
                        strConfFile.c_str());
        return false;
    }
    if (!m_oCoreSeekCmdConf.Get("read_file_path", m_readWordsFilePath))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "failed to load CoreSeekSession conf read_file_path");
        return false;
    }
    if (!m_oCoreSeekCmdConf.Get("separate", m_separate))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "failed to load CoreSeekSession conf separate");
        return false;
    }
    if (!m_oCoreSeekCmdConf.Get("delete_old", m_deleteOldWordsFile))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "failed to load CoreSeekSession conf delete_old");
        return false;
    }
    if (!m_oCoreSeekCmdConf.Get("convert_code", m_convertCode))
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,
                        "failed to load CoreSeekSession conf convert_code");
        return false;
    }
    boInit = true;
    return true;
}

void CoreSeekSession::Routine()
{
    LOG4CPLUS_DEBUG_FMT(m_oLogger,"CoreSeekSession::Routine");
    if(!CheckDataFiles())
    {
        LOG4CPLUS_DEBUG_FMT(m_oLogger,"failed to CheckDataFiles");
        return;
    }
    if(!TransferFiles())
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger,"failed to TransferFiles");
        return;
    }
}

bool CoreSeekSession::CheckDataFiles()
{
    LOG4CPLUS_DEBUG_FMT(m_oLogger,"CoreSeekSession::CheckDataFiles");
    m_filesNameVec.clear();
    if (!IsDirectory(m_readWordsFilePath.c_str()))
    {
        LOG4CPLUS_INFO_FMT(m_oLogger, "readfile_path(%s) don't exist", m_readWordsFilePath.c_str());
        if (!DeepCreateDirectory(m_readWordsFilePath.c_str()))
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger, "failed to create readfile_path(%s)", m_readWordsFilePath.c_str());
            return false;
        }
    }
    //获取该目录下.in拓展名的所有文件(.in为数据文件拓展名)
    const char* sFileExt = ".in";
    //替换字符 find . -name "*.txt" |xargs sed -i 's/\r/\n/g'
    //其他格式的文件需要先转换为 .in结尾的文件  find . -type f -name "*.txt" |xargs -i mv "{}" "{}.in"
    if (GetDirCommonFilesByExt(m_readWordsFilePath.c_str(), sFileExt, 3,
                    m_filesNameVec) == -1)
    {
        LOG4CPLUS_ERROR_FMT(m_oLogger, "can't get readfile_path(%s) file for input files,errno(%d),strerror(%s)",
                        m_readWordsFilePath.c_str(), errno, strerror(errno));
        return (false);
    }
    if (m_filesNameVec.empty())//没有文件
    {
        LOG4CPLUS_DEBUG_FMT(m_oLogger, "readfile_path(%s) don't has files to read",
                        m_readWordsFilePath.c_str());
        return (false);
    }

    if(m_deleteOldWordsFile)
    {
        m_deletefilesNameVec.clear();
        //获取该目录下.out拓展名的所有文件(.out为数据文件拓展名)
        const char* sFileExt = ".out";
        //替换字符 find . -name "*.txt" |xargs sed -i 's/\r/\n/g'
        //其他格式的文件需要先转换为 .in结尾的文件  find . -type f -name "*.txt" |xargs -i mv "{}" "{}.in"
        if (GetDirCommonFilesByExt(m_readWordsFilePath.c_str(), sFileExt, 3,
                        m_deletefilesNameVec) == -1)
        {
            LOG4CPLUS_ERROR_FMT(m_oLogger, "can't get readfile_path(%s) file for deletefiles,errno(%d),strerror(%s)",
                            m_readWordsFilePath.c_str(), errno, strerror(errno));
            return false;
        }
        //删除旧的生成文件
        std::vector<std::string>::iterator it = m_deletefilesNameVec.begin();
        std::vector<std::string>::iterator itEnd = m_deletefilesNameVec.end();
        for(;it != itEnd;++it)
        {
            if(IsArchive(it->c_str()))
            {
                if(RemoveFile(it->c_str()) == 0)
                {
                    LOG4CPLUS_DEBUG_FMT(m_oLogger,"remove file(%s) ok",it->c_str());
                }
                else
                {
                    LOG4CPLUS_ERROR_FMT(m_oLogger,"failed to remove file(%s)",it->c_str());
                }
            }
        }
    }
    return true;
}

bool CoreSeekSession::TransferFiles()
{
    LOG4CPLUS_DEBUG_FMT(m_oLogger,"CoreSeekSession::TransferFiles");
    std::vector<std::string>::iterator it = m_filesNameVec.begin();
    std::vector<std::string>::iterator itEnd = m_filesNameVec.end();
    std::string strWriteTotalFile;
    std::ofstream writeTotalfout;
    int writeTotalCounter(0);
    if(eSeparateStatus_Total == m_separate ||eSeparateStatus_All == m_separate)//打开总体文件  总体.out
    {
        strWriteTotalFile = m_readWordsFilePath + std::string("总体") + ".out";
        writeTotalfout.open(strWriteTotalFile.c_str(),std::ios::out | std::ios::app);
        if(!writeTotalfout.good())
        {
            //配置信息流读取失败
            LOG4_ERROR("Open file(%s) to write error!",
                            strWriteTotalFile.c_str());
            return false;
        }
        LOG4CPLUS_DEBUG_FMT(m_oLogger,"open strWriteTotalFile file :%s ok",strWriteTotalFile.c_str());
    }
    for(;it != itEnd;++it)
    {
        std::ifstream readfin(it->c_str(), std::ios::in);//读取一个数据文件
        if(!readfin.good())
        {
            //配置信息流读取失败
            LOG4_ERROR("Open conf (%s) error!",it->c_str());
            return false;
        }
        LOG4CPLUS_DEBUG_FMT(m_oLogger,"read file:%s",it->c_str());
        std::string strWriteFile;
        std::ofstream writefout;
        int writeCounter(0);
        if(eSeparateStatus_Separate == m_separate ||eSeparateStatus_All == m_separate)//打开新文件 *.out
        {
            //写入文件
            char sFileNameNoExt[256];
            DropFileExt(it->c_str(),sFileNameNoExt,sizeof(sFileNameNoExt));
            strWriteFile = std::string(sFileNameNoExt) + ".out";
            writefout.open(strWriteFile.c_str(),std::ios::out | std::ios::app);
            if(!writefout.good())
            {
                //配置信息流读取失败
                LOG4_ERROR("Open conf (%s) to write error!",
                                strWriteFile.c_str());
                readfin.close();
                return false;
            }
            LOG4CPLUS_DEBUG_FMT(m_oLogger,"open write file :%s ok",strWriteFile.c_str());
        }
        {
            char word[256];
            std::string line;
            int n;
            while(std::getline(readfin,line))
            {
                if(!line.empty())
                {
                    if(m_convertCode)
                    {
                        std::string convertedCode;
                        int ret = gbk2utf8(convertedCode,line.c_str());
                        if (ret < 0)
                        {
                            LOG4CPLUS_WARN_FMT(m_oLogger,"failed to gbk2utf8,code:%d",ret);
                            readfin.close();
                            if(writefout.is_open())
                            {
                                writefout.close();
                            }
                            if(writeTotalfout.is_open())
                            {
                                writeTotalfout.close();
                            }
                            return false;
                        }
                        line.assign(convertedCode);
                    }
                    LOG4CPLUS_DEBUG_FMT(m_oLogger,"line size(%u):%s",line.size(),line.c_str());
                    //写入格式为  单词\t1\r\nx:1\r\n
                    n = snprintf(word,sizeof(word),"%s\t1\r\nx:1\r\n",line.c_str());
                    if (n > 0)
                    {
                        if(writefout.is_open())//写到新的分别文件  *.out
                        {
                            writefout.write(word,n);
                            ++writeCounter;
                        }
                        if(writeTotalfout.is_open())//写到总体文件  总体.out
                        {
                            writeTotalfout.write(word,n);
                            ++writeTotalCounter;
                        }
                    }
                }
                else
                {
                    LOG4_DEBUG("empty line");
                }
            }
            readfin.close();
        }
        if(writefout.is_open())
        {
            writefout.flush();
            writefout.close();
            LOG4_DEBUG("update strWriteFile(%s) ok,writeCounter(%d)",strWriteFile.c_str(),writeCounter);
        }
    }
    if(writeTotalfout.is_open())
    {
        writeTotalfout.flush();
        writeTotalfout.close();
        LOG4_DEBUG("update strWriteTotalFile(%s) ok,writeTotalCounter(%d)",strWriteTotalFile.c_str(),writeTotalCounter);
    }
    return true;
}


}    //namespace robot
