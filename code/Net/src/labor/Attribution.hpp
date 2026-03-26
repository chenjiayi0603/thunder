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
#include <algorithm>
#include <cstring>
#include <memory>
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
    std::unique_ptr<util::CBuffer> pRecvBuff;           ///< 在结构体析构时回收
    std::unique_ptr<util::CBuffer> pSendBuff;           ///< 在结构体析构时回收
    std::unique_ptr<util::CBuffer> pWaitForSendBuff;    ///< 等待发送的数据缓冲区（数据到达时，连接并未建立，等连接建立并且pSendBuff发送完毕后立即发送）
    std::unique_ptr<util::CBuffer> pClientData;         ///< 客户端相关数据（例如IM里的用户昵称、头像等，登录或连接时保存起来，后续发消息或其他操作无须客户端再带上来）
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

    bool IsVerify() const
	{
		return(pClientData != nullptr && pClientData->ReadableBytes() > 0);
	}
};


/** Manager/Loader/Worker 共用 MAP_SHARED；shm.seq_* 为跨进程事件序号，m_ack 为本进程已消费序号 */
struct LoaderConfigVersionData
{
	struct LoaderConfigVersionMM
	{
		uint64 seq_config = 0;
		uint64 seq_node_notice = 0;
		uint64 seq_restart_workers = 0;
		uint32 node_id = 0;
		uint32 node_notice_len = 0;
		char server_config_name[64] = {0};
		char server_config_body[16 * 1024] = {0};
		char node_notice_blob[16 * 1024] = {0};
	};

private:
	LoaderConfigVersionMM* m_pShm = nullptr;
	struct {
		uint64 config = 0;
		uint64 node_notice = 0;
		uint64 restart_workers = 0;
	} m_ack{};

public:
	bool m_bLoaderProcess = false;

	void SetLoaderConfigVersionMM(LoaderConfigVersionMM* loaderConfigVersionMM = nullptr)
	{
		if (loaderConfigVersionMM)
		{
			DelLoaderConfigVersionMM();
			m_pShm = loaderConfigVersionMM;
		}
		else if (m_pShm == nullptr)
		{
			m_pShm = static_cast<LoaderConfigVersionMM*>(mmap(
				NULL, sizeof(LoaderConfigVersionMM), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
			memset(m_pShm, 0, sizeof(*m_pShm));
		}
	}

	LoaderConfigVersionMM* GetLoaderConfigVersionMM()
	{
		if (m_pShm == nullptr)
		{
			SetLoaderConfigVersionMM();
		}
		return m_pShm;
	}

	void DelLoaderConfigVersionMM()
	{
		if (m_pShm)
		{
			munmap(m_pShm, sizeof(*m_pShm));
			m_pShm = nullptr;
		}
	}

	bool IsLoaderProcess() const { return m_bLoaderProcess; }

	uint64 IncLoaderConfigVersion() { return m_pShm ? ++m_pShm->seq_config : 0; }

	bool IsConfigVersionChange() const { return m_pShm && (m_pShm->seq_config > m_ack.config); }

	void UpdateLoaderConfigVersion()
	{
		if (m_pShm)
		{
			m_ack.config = m_pShm->seq_config;
		}
	}

	/** Center 等写入；须 IncLoaderConfigVersion() 通知各进程 */
	void SetServerConfigFile(const std::string& configName, const std::string& configContent)
	{
		if (!m_pShm || configName.empty() || configContent.empty())
		{
			return;
		}
		const size_t nameCap = sizeof(m_pShm->server_config_name);
		const size_t nameLen = std::min(configName.size(), nameCap - 1);
		memcpy(m_pShm->server_config_name, configName.c_str(), nameLen);
		m_pShm->server_config_name[nameLen] = '\0';

		const size_t bodyCap = sizeof(m_pShm->server_config_body);
		const size_t bodyLen = std::min(configContent.size(), bodyCap - 1);
		memcpy(m_pShm->server_config_body, configContent.c_str(), bodyLen);
		m_pShm->server_config_body[bodyLen] = '\0';
	}

	bool GetServerConfigFile(std::string& configContent) const
	{
		if (m_pShm && m_pShm->server_config_body[0] != 0)
		{
			configContent = m_pShm->server_config_body;
			return true;
		}
		return false;
	}

	bool SetNodeNotice(const NodeNotice& oNodeNotice)
	{
		const int bs = oNodeNotice.ByteSize();
		if (!m_pShm || bs <= 0 || bs >= static_cast<int>(sizeof(m_pShm->node_notice_blob)))
		{
			return false;
		}
		std::string data = std::move(oNodeNotice.SerializeAsString());
		memcpy(m_pShm->node_notice_blob, data.data(), data.size());
		m_pShm->node_notice_len = static_cast<uint32_t>(data.size());
		++m_pShm->seq_node_notice;
		return true;
	}

	bool GetNodeNotice(NodeNotice& oNodeNotice) const
	{
		if (!m_pShm || m_pShm->node_notice_len == 0)
		{
			return false;
		}
		return oNodeNotice.ParseFromArray(m_pShm->node_notice_blob, static_cast<int>(m_pShm->node_notice_len));
	}

	void UpdateNodeNoticeVersion()
	{
		if (m_pShm)
		{
			m_ack.node_notice = m_pShm->seq_node_notice;
		}
	}

	bool IsNodeNoticeVersionChange() const
	{
		return m_pShm && (m_pShm->seq_node_notice > m_ack.node_notice);
	}

	uint64 GetNodeNoticeVersion() const { return m_pShm ? m_pShm->seq_node_notice : 0; }

	void SetNodeId(uint32 iNodeId)
	{
		if (m_pShm)
		{
			m_pShm->node_id = iNodeId;
		}
	}

	uint32 GetNodeId() const { return m_pShm ? m_pShm->node_id : 0; }

	void IncRestartWorkerOnUpdateConfigVersion()
	{
		if (m_pShm)
		{
			++m_pShm->seq_restart_workers;
		}
	}

	void UpdateRestartWorkerOnUpdateConfigVersion()
	{
		if (m_pShm)
		{
			m_ack.restart_workers = m_pShm->seq_restart_workers;
		}
	}

	bool IsRestartWorkerOnUpdateConfigChange() const
	{
		return m_pShm && (m_pShm->seq_restart_workers > m_ack.restart_workers);
	}
};

#endif /* SRC_LABOR_DUTY_ATTRIBUTION_HPP_ */
