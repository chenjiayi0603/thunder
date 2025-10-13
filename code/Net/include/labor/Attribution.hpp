/*******************************************************************************
* Project:  Thunder
* @file     Attribute.hpp
* @brief 
* @author   cjy
* @date:    2016年4月27日
* @note
* Modify history:
******************************************************************************/
#ifndef SRC_LABOR_DUTY_ATTRIBUTION_HPP_
#define SRC_LABOR_DUTY_ATTRIBUTION_HPP_

#include <stdlib.h>
#include <unordered_map>
#include <sys/mman.h>
#include "libev/ev.h"
#include "util/CBuffer.hpp"
#include "util/StreamCodec.hpp"
#include "util/json/CJsonObject.hpp"
#include "protocol/oss_sys.pb.h"
/**
 * @brief 工作进程属性
 */
struct tagWorkerAttr
{
    int iWorkerIndex = 0;                   ///< 工作进程序号
    int iControlFd= -1;                    ///< 与Manager进程通信的文件描述符（控制流）
    int iDataFd = -1;                       ///< 与Manager进程通信的文件描述符（数据流）
    int32 iLoad = 0;                       ///< 负载
    int32 iConnect = 0;                    ///< 连接数量
    int32 iRecvNum = 0;                   ///< 接收数据包数量
    int32 iRecvByte = 0;                   ///< 接收字节数
    int32 iSendNum = 0;                   ///< 发送数据包数量
    int32 iSendByte = 0;                 ///< 发送字节数
    int32 iClientNum = 0;                  ///< 客户端数量
    ev_tstamp dBeatTime = time(nullptr);               ///< 心跳时间

    tagWorkerAttr() = default;

    tagWorkerAttr(const tagWorkerAttr& stAttr)
    {
        iWorkerIndex = stAttr.iWorkerIndex;
        iControlFd = stAttr.iControlFd;
        iDataFd = stAttr.iDataFd;
        iLoad = stAttr.iLoad;
        iConnect = stAttr.iConnect;
        iRecvNum = stAttr.iRecvNum;
        iRecvByte = stAttr.iRecvByte;
        iSendNum = stAttr.iSendNum;
        iSendByte = stAttr.iSendByte;
        iClientNum = stAttr.iClientNum;
        dBeatTime = stAttr.dBeatTime;
    }

    tagWorkerAttr& operator=(const tagWorkerAttr& stAttr)
    {
        iWorkerIndex = stAttr.iWorkerIndex;
        iControlFd = stAttr.iControlFd;
        iDataFd = stAttr.iDataFd;
        iLoad = stAttr.iLoad;
        iConnect = stAttr.iConnect;
        iRecvNum = stAttr.iRecvNum;
        iRecvByte = stAttr.iRecvByte;
        iSendNum = stAttr.iSendNum;
        iSendByte = stAttr.iSendByte;
        iClientNum = stAttr.iClientNum;
        dBeatTime = stAttr.dBeatTime;
        return(*this);
    }
};
enum ConnectStatus
{
    eConnectStatus_init = 0,
    eConnectStatus_connecting = 1,
    eConnectStatus_ok = 2,
};

/**
 * @brief 连接属性
 * @note  连接属性，因内部带有许多指针，并且没有必要提供深拷贝构造，所以不可以拷贝，也无需拷贝
 */
struct tagConnectionAttr
{
	static const uint32 mc_uiBeat = 0x00000001;
	static const uint32 mc_uiAlive = 0x00000007;   ///< 最近三次心跳任意一次成功则认为在线
    /**
     * @brief 连接状态
     * @note 连接状态 （复用发起连接CMD：
     *  0 connect未返回结果，
     *  CMD_REQ_CONNECT_TO_WORKER 发起连接worker
     *  CMD_RSP_CONNECT_TO_WORKER 已得到对方manager响应，转给worker
     *  CMD_REQ_TELL_WORKER 将己方worker信息通知对方worker
     *  CMD_RSP_TELL_WORKER 收到对方worker响应，连接已就绪）
     */
    unsigned char ucConnectStatus = 0;
    util::CBuffer* pRecvBuff = nullptr;           ///< 在结构体析构时回收
    util::CBuffer* pSendBuff = nullptr;           ///< 在结构体析构时回收
    util::CBuffer* pWaitForSendBuff = nullptr;    ///< 等待发送的数据缓冲区（数据到达时，连接并未建立，等连接建立并且pSendBuff发送完毕后立即发送）
    util::CBuffer* pClientData = nullptr;         ///< 客户端相关数据（例如IM里的用户昵称、头像等，登录或连接时保存起来，后续发消息或其他操作无须客户端再带上来）
    char szRemoteAddr[32] = {0};                  ///< 对端IP地址（不是客户端地址，但可能跟客户端地址相同）
    util::E_CODEC_TYPE eCodecType = util::CODEC_PB_INTERNAL;      ///< 协议（编解码）类型
    ev_tstamp dActiveTime = 0;              ///< 最后一次访问时间
    ev_tstamp dKeepAlive = 0;                ///< 连接保持时间，默认值0为用心跳保持的长连接，大于0的值不做心跳检查，时间到即断连接,小于0为收完数据立即断开连接（主要用于http连接）
    int iFd = 0; 						///< 文件描述符
    uint32 ulSeq = 0;                       ///< 文件描述符创建时对应的序列号
    uint32 ulForeignSeq = 0;                ///< 外来的seq，每个连接的包都是有序的，用作接入Server数据包检查，防止篡包
    uint32 ulMsgNumUnitTime = 0;             ///< 统计单位时间内发送消息数量
    uint32 ulMsgNum = 0;                    ///< 发送消息数量
    std::string strIdentify;            ///< 连接标识（可以为空，不为空时用于标识业务层与连接的关系）
    ev_io* pIoWatcher = nullptr;                  ///< 不在结构体析构时回收
    ev_timer* pTimeWatcher = nullptr;             ///< 不在结构体析构时回收
    std::string strSessionKey;	//会话密钥

    std::unordered_map<uint32,uint32> mapCmdsUnitMsgCounter;//连接的单位时间指令统计
    ev_tstamp dUnitLimitLastTime = 0;              ///< 最后一次统计时间

    tagConnectionAttr() = default;

    ~tagConnectionAttr()
    {
    	SAFE_DELETE(pRecvBuff);
    	SAFE_DELETE(pSendBuff);
    	SAFE_DELETE(pWaitForSendBuff);
    	SAFE_DELETE(pClientData);
    }
    bool IsVerify() const
	{
		return(pClientData != nullptr && pClientData->ReadableBytes() > 0);
	}
};


struct LoaderConfigVersionData
{
	struct LoaderConfigVersionMM {
		uint64 uiConfigVersion = 0;
		char szServerConfigName[64] = {0};//服务配置名（服务器的第一个服务配置）
		char szServerConfigContent[16 * 1024] = {0};//服务配置内容

		uint32 iNodeId = 0;//节点id

	    uint32 iNodeNoticeLen = 0;//节点变化通知长度
	    uint64 uiNodeNoticeVersion = 0;
	    char szNodeNotice[16 * 1024];//节点变化通知(可存储500+个节点)

	    uint64 uiRestartWorkerOnUpdateConfigVersion = 0;//由于更新配置而重启工作者（由主进程操作）
	};
	struct LoaderConfigVersionMM *m_LoaderConfigVersionMM = nullptr;//配置递增序号共享内存(从1开始递增)
	uint64 m_uiLoaderConfigVersion = 0;//配置版本递增序号(从1开始递增) （每个进程得到一个拷贝,对应uiConfigVersion）
	uint64 m_uiNodeNoticeVersion = 0;//节点通知版本递增序号(从1开始递增) （每个进程得到一个拷贝，对应uiNodeNoticeVersion）

	uint64 m_uiRestartWorkerOnUpdateConfigVersion = 0;

	bool m_bLoaderProcess = false;

	void SetLoaderConfigVersionMM(LoaderConfigVersionMM *loaderConfigVersionMM = nullptr)
	{
		if (loaderConfigVersionMM)
		{
			DelLoaderConfigVersionMM();
			m_LoaderConfigVersionMM = loaderConfigVersionMM;
		}
		else if (m_LoaderConfigVersionMM == nullptr)
		{
			m_LoaderConfigVersionMM = (LoaderConfigVersionMM *)mmap(NULL, sizeof(LoaderConfigVersionMM), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANON, -1, 0);//映射区不与任何文件关联
			memset(m_LoaderConfigVersionMM, 0, sizeof(*m_LoaderConfigVersionMM));  //初始化内存
		}
	}

	LoaderConfigVersionMM * GetLoaderConfigVersionMM()
	{
		if (nullptr == m_LoaderConfigVersionMM)
		{
			SetLoaderConfigVersionMM();
		}
		return m_LoaderConfigVersionMM;
	}

	void DelLoaderConfigVersionMM()
	{
		if (m_LoaderConfigVersionMM)
		{
			munmap(m_LoaderConfigVersionMM,sizeof(*m_LoaderConfigVersionMM));                         //释放映射区
			m_LoaderConfigVersionMM = nullptr;
		}
	}

	void SetLoaderConfigVersion(uint64 uiLoaderConfigVersion){m_uiLoaderConfigVersion = uiLoaderConfigVersion;}
	uint64 GetLoaderConfigVersion()const{return m_uiLoaderConfigVersion;}

	uint64 IncLoaderConfigVersion()
	{
		if (m_LoaderConfigVersionMM)
		{
			return ++m_LoaderConfigVersionMM->uiConfigVersion;
		}
		return 0;
	}

	bool IsConfigVersionChange()
	{
		if (m_LoaderConfigVersionMM && m_LoaderConfigVersionMM->uiConfigVersion > m_uiLoaderConfigVersion)
		{
			return true;
		}
		return false;
	}

	void UpdateLoaderConfigVersion()
	{
		if (m_LoaderConfigVersionMM)
		{
			m_uiLoaderConfigVersion = m_LoaderConfigVersionMM->uiConfigVersion;
		}
	}
	bool IsLoaderProcess()const {return m_bLoaderProcess;}

	void SetServerConfigFile(const std::string& configName,const std::string& configContent)
	{
		if (m_LoaderConfigVersionMM && configName.size() > 0 && configContent.size() > 0)
		{
			if (m_LoaderConfigVersionMM->szServerConfigName[0] == 0)
			{
				memcpy(m_LoaderConfigVersionMM->szServerConfigName,configName.c_str(),std::min(configName.size(),sizeof(m_LoaderConfigVersionMM->szServerConfigName)) );
				memcpy(m_LoaderConfigVersionMM->szServerConfigContent,configContent.c_str(),std::min(configContent.size(),sizeof(m_LoaderConfigVersionMM->szServerConfigContent)) );
			}
			else if (m_LoaderConfigVersionMM->szServerConfigName[0] != 0 && strcmp(m_LoaderConfigVersionMM->szServerConfigName,configName.c_str()) == 0)
			{
				memcpy(m_LoaderConfigVersionMM->szServerConfigContent,configContent.c_str(),std::min(configContent.size(),sizeof(m_LoaderConfigVersionMM->szServerConfigContent)) );
			}
		}
	}
	bool GetServerConfigFile(std::string& configContent)const
	{
		if (m_LoaderConfigVersionMM && m_LoaderConfigVersionMM->szServerConfigContent[0] != 0)
		{
			configContent = m_LoaderConfigVersionMM->szServerConfigContent;
			return true;
		}
		return false;
	}

	bool SetNodeNotice(const NodeNotice& oNodeNotice)
	{
		if (m_LoaderConfigVersionMM && oNodeNotice.ByteSize() > 0 && oNodeNotice.ByteSize() < (int)sizeof(m_LoaderConfigVersionMM->szNodeNotice))
		{
			std::string data = std::move(oNodeNotice.SerializeAsString());
			memcpy(m_LoaderConfigVersionMM->szNodeNotice,data.data(),data.size());
			m_LoaderConfigVersionMM->iNodeNoticeLen = data.size();
			++m_LoaderConfigVersionMM->uiNodeNoticeVersion;
			return true;
		}
		return false;
	}
	bool GetNodeNotice(NodeNotice& oNodeNotice)const
	{
		if (m_LoaderConfigVersionMM && m_LoaderConfigVersionMM->iNodeNoticeLen > 0)
		{
			if (oNodeNotice.ParseFromArray(m_LoaderConfigVersionMM->szNodeNotice,m_LoaderConfigVersionMM->iNodeNoticeLen))
			{
				return true;
			}
		}
		return false;
	}
	void UpdateNodeNoticeVersion()
	{
		if (m_LoaderConfigVersionMM)
		{
			m_uiNodeNoticeVersion = m_LoaderConfigVersionMM->uiNodeNoticeVersion;
		}
	}
	bool IsNodeNoticeVersionChange()
	{
		if (m_LoaderConfigVersionMM && m_LoaderConfigVersionMM->uiNodeNoticeVersion > m_uiNodeNoticeVersion)
		{
			return true;
		}
		return false;
	}
	uint64 GetNodeNoticeVersion()const
	{
		if (m_LoaderConfigVersionMM)
		{
			return m_LoaderConfigVersionMM->uiNodeNoticeVersion;
		}
		return 0;
	}
	void SetNodeId(uint32 iNodeId)
	{
		if (m_LoaderConfigVersionMM)
		{
			m_LoaderConfigVersionMM->iNodeId = iNodeId;
		}
	}
	uint32 GetNodeId()const
	{
		if (m_LoaderConfigVersionMM)
		{
			return m_LoaderConfigVersionMM->iNodeId;
		}
		return 0;
	}

	void IncRestartWorkerOnUpdateConfigVersion()
	{
		if (m_LoaderConfigVersionMM)
		{
			++m_LoaderConfigVersionMM->uiRestartWorkerOnUpdateConfigVersion;
		}
	}

	void UpdateRestartWorkerOnUpdateConfigVersion()
	{
		if (m_LoaderConfigVersionMM)
		{
			m_uiRestartWorkerOnUpdateConfigVersion = m_LoaderConfigVersionMM->uiRestartWorkerOnUpdateConfigVersion;
		}
	}

	bool IsRestartWorkerOnUpdateConfigChange()
	{
		if (m_LoaderConfigVersionMM && m_LoaderConfigVersionMM->uiRestartWorkerOnUpdateConfigVersion > m_uiRestartWorkerOnUpdateConfigVersion)
		{
			return true;
		}
		return false;
	}
};

#endif /* SRC_LABOR_DUTY_ATTRIBUTION_HPP_ */
