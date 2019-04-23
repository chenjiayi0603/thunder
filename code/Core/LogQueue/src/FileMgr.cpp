#include "util/bzhash.hpp"
#include "step/StepNode.hpp"
#include "FileMgr.h"

#include "../../Proto/include/NosqlProto.h"

namespace core
{

FileMgr::FileMgr():m_dWriteLogInterval(0),m_dCreateLogInterval(0),m_uiSyncLog(1),m_uiLogQueueNum(0),m_uiLogFormat(0),m_uiCreateLogLastTime(0),m_uiWriteLogLastTime(0)
{
    m_uiCurrentTime = 0;
    m_boNeedSync = false;
    m_uiWritedLogCounter = 0;
}

FileMgr::~FileMgr()
{
    CloseLog();
}

bool FileMgr::OpenLog()
{
    static const char szLogDataFileName[] = "Log_"; //日志文件库名
    //打开或者创建日志文件数据文件
    //如 /data/file/Log_201511241139_192.168.18.68:16004.0.dat
    char sLogName[256];
    char sTimeStamp[64];
    util::GetTimeStampMinuteStr(sTimeStamp, sizeof(sTimeStamp));//用毫秒字符串来作为文件名
    snprintf(sLogName, sizeof(sLogName) - 1, "%s%s%s_%s", m_strDatalogPath.c_str(),
                    szLogDataFileName, sTimeStamp,net::GetWorkerIdentify().c_str());//dir/prefile_time_ident.dat
    if (m_logFile.IsOpened() && m_logFile.m_logName == sLogName)
    {
        LOG4_TRACE( "%s() file already opened m_logName(%s)",__FUNCTION__,m_logFile.m_logName.c_str());
        return (true);
    }
    if (m_logFile.open(sLogName))//当前时间作为新的日志文件，如果已有该文件则使用该文件
    {
    	LOG4_INFO( "%s() succ to open logDataName(%s)",__FUNCTION__,m_logFile.m_logDataName.c_str());

    }
    else if (m_logFile.create(sLogName))
    {
    	LOG4_INFO( "%s() succ to create logDataName(%s)",__FUNCTION__,m_logFile.m_logDataName.c_str());
    }
    else
    {
    	LOG4_ERROR( "%s() failed to open and create sLogName(%s)",__FUNCTION__,sLogName);
		return (false);
    }
    SetCurrentTime();
    m_uiCreateLogLastTime = m_uiCurrentTime;
    m_uiWritedLogCounter = 0;
    LOG4_INFO("OpenLog ok,logFile(%s,%d,%u)",m_logFile.m_logDataName.c_str(),m_logFile.GetFileFD(),m_uiCreateLogLastTime);
    return (true);
}

void FileMgr::CloseLog()
{
    LOG4_INFO("%s() need to CloseLog m_logDataName(%s) file no(%d)",
                        __FUNCTION__,m_logFile.m_logDataName.c_str(),m_logFile.GetFileFD());
    if (!m_logFile.IsOpened())
    {
        LOG4_INFO("%s() CloseLog file not open m_logDataName(%s) file no(%d)",
                        __FUNCTION__,m_logFile.m_logDataName.c_str(),m_logFile.GetFileFD());
        return;
    }
    //如果有数据存储尚未完成是要进行额外的处理，不能直接关闭文件
    if (!IsAllWriteComplete())
    {
    	RoutineWrite();
    }
    LOG4_INFO("%s() CloseLog ok m_logDataName(%s) file no(%d)",
                    __FUNCTION__,m_logFile.m_logDataName.c_str(),m_logFile.GetFileFD());
    m_logFile.close();
}

bool FileMgr::CheckSync()
{
    //在数据都写入系统缓存队列后同步到磁盘(目前使用系统的缓存，则不在用户空间一次写入多个消息)
    if (m_uiSyncLog > 0 && m_boNeedSync)
    {
        if(-1 == m_logFile.SyncDataLog())
        {
            LOG4_FATAL("SyncDataLog failed errno(%d),strerror(%s) GetFileFD(%u) logDataName(%s)",
                            errno, strerror(errno),m_logFile.GetFileFD(),m_logFile.m_logDataName.c_str());
            return false;
        }
        m_boNeedSync = false;
        LOG4_INFO("SyncDataLog succed");
    }
    return true;
}

bool FileMgr::RoutineWrite(bool boForceNewLog)    //写入日志队列的全部写入
{
	m_uiWriteLogLastTime = SetCurrentTime();
    if (boForceNewLog)
    {
        LOG4_INFO("%s() force to open new file",__FUNCTION__);
        if(!OpenLog())
        {
            LOG4_FATAL("OpenLog failed");
            return false;
        }
    }
    else if (!m_logFile.IsOpened() || !m_logFile.ArchiveExist())
    {
        LOG4_INFO("%s() m_logFile(%s) not existd or not open(%d),need to open new file",
                    __FUNCTION__,m_logFile.m_logDataName.c_str(),m_logFile.GetFileFD());
        if(!OpenLog())
        {
            LOG4_FATAL("OpenLog failed");
            return false;
        }
    }
    LOG4_INFO("RoutineWrite logfilename(%s) m_logMsgVec(%u)",m_logFile.m_logDataName.c_str(),m_logMsgVec.size());//写日志队列
    if (m_logMsgVec.size() > 0)
    {
        uint32 counter(0);
        auto cit = m_logMsgVec.begin();
        for (;cit != m_logMsgVec.end();++cit,++counter)
        {
            if (m_uiLogQueueNum  > 0 && counter >= m_uiLogQueueNum)
            {
                break;
            }
            const logqueue::log& message = *cit;
            if (0 == m_uiLogFormat)//写磁盘(es格式)
            {
                if (!Write2ESConsumeFile(message))
                {
                    LOG4_FATAL( "%s() Write2ESConsumeFile failed message(%s)",__FUNCTION__,message.DebugString().c_str());
                    return false;
                }
                m_boNeedSync = true;
            }
            else if (1 == m_uiLogFormat)//写磁盘(自定义格式)
            {
                if (!Write2CustomHeadLogFile(message))
                {
                    LOG4_FATAL( "%s() Write2CustomHeadLogFile failed message(%s)",__FUNCTION__,message.DebugString().c_str());
                    return false;
                }
                m_boNeedSync = true;
            }
            else if (2 == m_uiLogFormat)//写ssdb(es格式)
            {
                if (!Write2SSDBConsumeFile(message))//写ssdb(es格式)
                {
                    LOG4_FATAL( "%s() Write2SSDBConsumeFile failed message(%s)",__FUNCTION__,message.DebugString().c_str());
                    return false;
                }
            }
            else//写ssdb(es格式) 和 写磁盘(es格式)
            {
                if (!Write2ESConsumeFile(message))//写磁盘(es格式)
                {
                    LOG4_FATAL( "%s() Write2ESConsumeFile failed message(%s)",__FUNCTION__,message.DebugString().c_str());
                    return false;
                }
                m_boNeedSync = true;
                if (!Write2SSDBConsumeFile(message))//写ssdb(es格式)
                {
                    LOG4_FATAL( "%s() Write2SSDBConsumeFile failed message(%s)", __FUNCTION__,message.DebugString().c_str());
                }
            }

        }
        if (m_logMsgVec.size() > counter)
        {
        	m_logMsgVec.erase(m_logMsgVec.begin(),cit);//删除已写入的部分
        }
        else
        {
        	m_logMsgVec.clear();
        }
        m_uiWritedLogCounter += counter;
        LOG4_INFO("%s() counter(%d,%llu) logMsgVec(%u) m_uiLogQueueNum(%u)",__FUNCTION__,counter,m_uiWritedLogCounter,m_logMsgVec.size(),m_uiLogQueueNum);
    }
    return true;
}
//自定义二进制头存储结构
bool FileMgr::Write2CustomHeadLogFile(const logqueue::log& message)
{
    /*
    {"account_number":1,"balance":39225,"firstname":"Amber","lastname":"Duke","age":32,"gender":"M","address":"880 Holmes Lane","employer":"Pyrami","email":"amberduke@pyrami.com","city":"Brogan","state":"IL"}
     * */
    m_buff.Clear();
    const std::string& logBody = message.log_info();
    {//数据头
    	int nLogCmd(0);
    	auto iter = m_oTableCmd.find(message.type());
    	if (iter != m_oTableCmd.end())
    	{
    		nLogCmd = iter->second;
    	}
        LogDataHeader header;
        header.nLogCmd = nLogCmd;//指令类型
        header.nDataVersion = LogDataHeader::VERSION;//数据版本号
        header.nCrc = (uint16)util::CalcCrc(logBody);//冗余校验字段
        header.nBodySize = logBody.size();//数据记录体大小
        m_buff.Write((const char *)&header,sizeof(header));
    }
    {//数据体
        m_buff.Write(logBody.c_str(),logBody.size());
    }
    //写到日志文件
    if(!m_logFile.AppendDataFile(m_buff.GetReadBuff(),m_buff.ReadDataLen()))
    {
        LOG4_FATAL("%s() failed to buff(%s)",__FUNCTION__,m_buff.GetReadBuff());
        return false;
    }
    LOG4_TRACE("%s() AppendDataFile ok",__FUNCTION__);
    return true;
}
//类ES型存储结构消费文件
bool FileMgr::Write2ESConsumeFile(const logqueue::log& message)
{
    LOG4_TRACE( "%s() Write2ESConsumeFile message(%s)",__FUNCTION__,message.DebugString().c_str());
    util::CJsonObject msgs;
    if(!msgs.Parse(message.log_info()))
    {
        LOG4_WARN( "%s() not valid message(%s)",__FUNCTION__,message.DebugString().c_str());
        return false;
    }
    int s = msgs.GetArraySize();
    if (s > 0)
    {
        for(int i = 0;i < s;++i)
        {
            msgs[i].Add("date",m_strDate);
            Write2ESConsumeFileWithLog(msgs[i].ToString(),message);
        }
    }
    else
    {
        msgs.Add("date",m_strDate);
        Write2ESConsumeFileWithLog(msgs.ToString(),message);
    }
    LOG4_TRACE("%s() AppendDataFile ok",__FUNCTION__);
    return true;
}

bool FileMgr::Write2ESConsumeFileWithLog(const std::string& logBody,const logqueue::log& message)
{
    m_buff.Clear();
    //数据头
    /*
     {
        "log_cmd":1,
        "table":{"index":{"_index":"db_start_trace","_type":"tb_start_trace"}}
    }
     * */
    auto iter = m_oTableHead.find(message.type());
    if (iter == m_oTableHead.end())
    {
    	LOG4_ERROR( "%s() type(%u) invalid.not found.",__FUNCTION__,message.type().c_str());
		return false;
    }
    util::CJsonObject oHead = iter->second;
	{
		//存储结构可根据id去重
		char strHash[64];
		util::HashStrToString(logBody.c_str(),logBody.size(),strHash,sizeof(strHash));
		oHead["index"].Add("_id",strHash);
	}
	std::string strTableSerial = oHead.ToString();
	if (strTableSerial.size() > 0)
	{
		//{"index":{"_index":"db_trace","_type":"tb_trace","_id":"772575682936466444916803884"},"user":{"app_id":1,"user_id":"028ac627-9c99-4ed8-bdaa-c84a8d7cfcfb","device_id":"","session_id":"1508922829098"}}
		m_buff.Write(strTableSerial.c_str(),strTableSerial.size());
		m_buff.Write("\n",1);
	}
	else
	{
		LOG4_ERROR( "%s() type(%s) invalid. empty oHead.",__FUNCTION__,message.type().c_str());
		return false;
	}

    {//数据体
        /*
        {"account_number":1,"balance":39225,"firstname":"Amber","lastname":"Duke","age":32,"gender":"M","address":"880 Holmes Lane","employer":"Pyrami","email":"amberduke@pyrami.com","city":"Brogan","state":"IL"}
         * */
        m_buff.Write(logBody.c_str(),logBody.size());
        m_buff.Write("\n",1);
    }
    //写到日志文件
    if(!m_logFile.AppendDataFile(m_buff.GetReadBuff(),m_buff.ReadDataLen()))
    {
        LOG4_ERROR("%s() failed to buff(%s)",__FUNCTION__,m_buff.GetReadBuff());
        return false;
    }
    return true;
}

/*
/app/analysis/db/redis/bin/redis-cli -h 192.168.18.78 -p 7000 hgetall 1:4:MSG?20180515
 1) "6555702720609648641"
 2) "{\"index\":{\"_index\":\"db_start_trace\",\"_type\":\"tb_start_trace\",\"_id\":\"13519432753697886603220793589\"},
 \"user\":{\"app_id\":1,\"user_id\":\"chenjiayi\",\"device_id\":\"\",\"session_id\":\"11111111\"}}\n
 {\"event_id\":\"event_id4\",\"user_id\":\"chenjiayi\",\"session_id\":\"20170712171708989\",\"location\":\"111\",\"session_id\":\"11111111\",
 \"channel\":\"\xe8\x85\xbe\xe8\xae\xaf121212\",\"device_id\":\"00-50-56-C0-00-08\",\"ip\":\"192.168.11.232\",\"os\":\"iOS6\",\"os_version\":\"4.0\",
 \"model\":\"iPhone 5s\",\"app_version\":\"1.0\",\"time\":\"2017-08-14 16:15:10\"}\n"
 * */
//ssdb存储类ES结构消费文件
bool FileMgr::Write2SSDBConsumeFile(const logqueue::log& message)
{
    LOG4_TRACE( "%s() Write2SSDBConsumeFile message(%s)",__FUNCTION__,message.DebugString().c_str());
    util::CJsonObject msgs;
    if(!msgs.Parse(message.log_info()))
    {
        LOG4_WARN( "%s() not valid message(%s)",__FUNCTION__,message.DebugString().c_str());
        return false;
    }
    int s = msgs.GetArraySize();
    if (s > 0)
    {
        for(int i = 0;i < s;++i)
        {
            msgs[i].Add("date",m_strDate);
            Write2SSDBConsumeFileWithLog(msgs[i].ToString(),message);
        }
    }
    else
    {
        msgs.Add("date",m_strDate);
        Write2SSDBConsumeFileWithLog(msgs.ToString(),message);
    }

    return true;
}

bool FileMgr::Write2SSDBConsumeFileWithLog(const std::string& logBody,const logqueue::log& message)
{
    m_buff.Clear();
    {//数据头

    	auto iter = m_oTableHead.find(message.type());
        if (iter == m_oTableHead.end())
        {
        	LOG4_ERROR( "%s() type(%s) invalid.not found.",__FUNCTION__,message.type().c_str());
			return false;
        }
        util::CJsonObject oHead = iter->second;
		{
			//存储结构可根据id去重
			char strHash[64];
			util::HashStrToString(logBody.c_str(),logBody.size(),strHash,sizeof(strHash));
			oHead["index"].Add("_id",strHash);
		}
		std::string strTableSerial = oHead.ToString();
		if (strTableSerial.size() > 0)
		{
			//{"index":{"_index":"db_trace","_type":"tb_trace","_id":"772575682936466444916803884"},"user":{"app_id":1,"user_id":"028ac627-9c99-4ed8-bdaa-c84a8d7cfcfb","device_id":"","session_id":"1508922829098"}}
			m_buff.Write(strTableSerial.c_str(),strTableSerial.size());
			m_buff.Write("\n",1);
		}
		else
		{
			LOG4_ERROR( "%s() type(%s) invalid.empty value.",__FUNCTION__,message.type().c_str());
			return false;
		}
    }
    {//数据体
        /*
        {"account_number":1,"balance":39225,"firstname":"Amber","lastname":"Duke","age":32,"gender":"M","address":"880 Holmes Lane","employer":"Pyrami","email":"amberduke@pyrami.com","city":"Brogan","state":"IL"}
         * */
        m_buff.Write(logBody.c_str(),logBody.size());
        m_buff.Write("\n",1);
    }
    //写日志到ssdb
    {
        auto callback = [] (const DataMem::MemRsp &oRsp,net::Step* pStep)
        {
            net::DataStep* pDataStep = (net::DataStep*)pStep;
            LOG4_TRACE("callback %s",oRsp.err_msg().c_str());
            if (oRsp.err_no() == 0)
            {
                LOG4_TRACE("callback ok %s",oRsp.DebugString().c_str());
            }
            else
            {
                LOG4_WARN("callback failed %s",oRsp.DebugString().c_str());
            }
        };
        std::string sValue(m_buff.GetReadBuff(),m_buff.ReadDataLen());
        char sUniqueId[32];snprintf(sUniqueId,sizeof(sUniqueId),"%llu",util::GetUniqueId(g_pLabor->GetNodeId(),g_pLabor->GetWorkerIndex()));
        char sDate[32];util::GetDateStr(sDate,sizeof(sDate),m_uiCurrentTime);
        //消息备份 1:100:20111101-ssdb hash
        char sRedisKey[64];
        snprintf(sRedisKey,sizeof(sRedisKey),"%d:%d:%s",NOSQL_T_HASH,DATA_GLOBAL_MSG_BACKUP,sDate);

        net::RedisOperator oRedisOperator(0, sRedisKey,"HSET");
        oRedisOperator.AddRedisField(sUniqueId,sValue);
        LOG4_TRACE("%s() Write2SSDBConsumeFile %s:%s",__FUNCTION__,sRedisKey,sValue.c_str());
        if (!net::SendToCallback(new net::DataStep(),oRedisOperator.MakeMemOperate(),callback,"PROXYSSDB"))
        {
            LOG4_ERROR("%s() SendToCallback failed",__FUNCTION__);
            return false;
        }
    }
    return true;
}

bool FileMgr::AddLog(const logqueue::log &logMsg)
{
	m_logMsgVec.push_back(logMsg);
    if (NeedNewLogCycle(SetCurrentTime()))return RoutineWrite(true);//需要新文件并写盘
    else if (NeedWriteLogCycle(m_uiCurrentTime)) return RoutineWrite();//需要写盘
	return true;
}

}//namespace core
