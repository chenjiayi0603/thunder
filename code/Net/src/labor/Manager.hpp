/*******************************************************************************
 * Project:  Net
 * @file     Manager.hpp
 * @brief    Node管理者
 * @author   cjy
 * @date:    2017年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef NodeManager_HPP_
#define NodeManager_HPP_
#include <memory>
#include "../NetDefine.hpp"
#include "../NetError.hpp"
#include "labor/Labor.hpp"
#include "labor/types/WorkerAttr.hpp"
#include "session/Session.hpp"

class NodeReportRsp;

namespace net
{

class Manager;

struct tagClientConnWatcherData
{
    in_addr_t iAddr = 0;
    Manager* pManager = nullptr;     // 不在结构体析构时回收
    tagClientConnWatcherData() = default;
};

struct tagManagerIoWatcherData
{
    int iFd = 0;
    uint32 ulSeq = 0;
    Manager* pManager = nullptr;     // 不在结构体析构时回收
    tagManagerIoWatcherData() = default;
};

struct tagManagerWaitExitWatcherData
{
    tagMsgShell stMsgShell;
    uint32 cmd = 0;
    uint32 seq = 0;
    Manager* pManager = nullptr;     // 不在结构体析构时回收
    tagManagerWaitExitWatcherData() = default;
};

/**
 * @brief 框架层管理者
 * @note 框架层管理者实现类，工作者包括Manager和Worker,Worker是子进程,Manager是父进程
 */
class Manager : public Labor
{
public:
    Manager(const std::string& strConfFile);
    virtual ~Manager();
    void Run();
    /**
     * @brief libev回调函数
     */
    static void SignalCallback(struct ev_loop* loop, struct ev_signal* watcher, int revents);
    static void IdleCallback(struct ev_loop* loop, struct ev_idle* watcher, int revents);
    static void IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents);
    static void IoTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);
    static void PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);  // 周期任务回调，用于替换IdleCallback
    static void WaitToExitTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents); //优雅等待关闭进程
    static void ClientConnFrequencyTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);

    static void SessionTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);
    /**
	 * @brief 信号处理函数
	 * @note Manager、Child结束信号处理
	 */
    bool OnManagerTerminated(struct ev_signal* watcher);
    bool OnChildTerminated(struct ev_signal* watcher);

    /**
   	* @brief 异步回调函数具体实现，io处理函数，timer处理函数
   	* @param pData 观察者数据
   	*/
    bool IoRead(tagManagerIoWatcherData* pData, struct ev_io* watcher);
    bool IoWrite(tagManagerIoWatcherData* pData, struct ev_io* watcher);
    bool IoError(tagManagerIoWatcherData* pData, struct ev_io* watcher);
    bool IoTimeout(tagManagerIoWatcherData* pData, struct ev_timer* watcher);
    bool FdTransfer(int iFd);
	bool AcceptServerConn(int iFd);
	bool RecvDataAndDispose(tagManagerIoWatcherData* pData, struct ev_io* watcher);
    bool ClientConnFrequencyTimeout(tagClientConnWatcherData* pData, struct ev_timer* watcher);

	bool SessionTimeout(Session* pSession, struct ev_timer* watcher);
public:
    /**
	 * @brief 设置进程名
	 */
    virtual void SetProcessName(const util::CJsonObject& oJsonConf)override;
    /**
	 * @brief 发送
	 */
    virtual bool SendTo(const tagMsgShell& stMsgShell)override;
    virtual bool SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    virtual bool SendTo(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody)override;
    virtual bool SetConnectIdentify(const tagMsgShell& stMsgShell, const std::string& strIdentify)override;
    virtual bool AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)override;
    bool ReportToCenter(bool boRegister = false);// 向管理中心上报负载信息
    bool SendTo(tagConnectionAttr* pConn,uint32 cmd,uint32 seq,const std::string & strBody);
	bool SendTo(tagConnectionAttr* pConn,const MsgHead& oMsgHead, const MsgBody& oMsgBody);
	bool SendTo(tagConnectionAttr* pConn);
	/**
	 * @brief 负载记录
	 */
    void SetWorkerLoad(int iPid, util::CJsonObject& oJsonLoad);
    void AddWorkerLoad(int iPid, int iLoad = 1);
protected:
    /**
	 * @brief 初始化配置
	 */
    void SetConfFile(const std::string& strConfFile);
    bool LoadConf(bool & boChanged);
    bool Init();
    void Destroy();
    void CreateWorker();
    void CreateLoader(bool boRestart = false);
    bool CreateEvents();
    void AddCmd(Cmd* pCmd,int iCmd);
    void PreloadCmd();
    bool RestartWorker(int iDeathPid);
    /**
	 * @brief io事件
	 */
    bool AddIoReadEvent(tagConnectionAttr* pConn);
    bool AddIoWriteEvent(tagConnectionAttr* pConn);
    bool RemoveIoWriteEvent(tagConnectionAttr* pConn);
    /**
	 * @brief 定时事件
	 */
    bool AddPeriodicTaskEvent();
    bool AddIoTimeout(int iFd, uint32 ulSeq, ev_tstamp dTimeout = 1.0);
    bool AddClientConnFrequencyTimeout(in_addr_t iAddr, ev_tstamp dTimeout = 60.0);
    /**
	 * @brief 连接创建与销毁
	 */
    tagConnectionAttr* CreateFdAttr(int iFd, uint32 ulSeq);
    tagConnectionAttr* CreateReadFdAttr(int iFd, uint32 ulSeq);
    tagConnectionAttr* CreateAcceptFdAttr(int iFd, uint32 ulSeq);
    bool DestroyConnect(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter);
    /**
	 * @brief 子进程负载、重启、检查心跳
	 */
    std::pair<int, int> GetMinLoadWorkerDataFd();
    bool CheckWorker();
    bool RestartWorkers();
    /**
	 * @brief 刷新配置
	 */
    void RefreshServer();
public:
    /**
	 * @brief 发送到工作者
	 */
    bool SendToWorker(const MsgHead& oMsgHead, const MsgBody& oMsgBody);    // 向Worker发送数据
    bool SendToWorker(uint32 cmd,uint32 seq,const std::string & strBody);
    bool SendToWorkerWithMod(uint64 uiModFactor,const MsgHead& oMsgHead, const MsgBody& oMsgBody);    // 向Worker发送数据


    /**
	* @brief 注册会话
	*/
    virtual bool RegisterCallback(Session* pSession)override;
    /**
	* @brief 删除会话
	*/
    virtual void DeleteCallback(Session* pSession)override;

protected:
    /**
   	 * @brief 数据接收处理
   	 */
    bool DisposeDataFromWorker(const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, tagConnectionAttr* pConn);
    bool DisposeDataAndTransferFd(const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, tagConnectionAttr* pConn);
    bool DisposeDataFromCenter(const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, tagConnectionAttr* pConn);
    void UpdateRaftLeaderHintFromNodeReportRsp(const NodeReportRsp& oNodeReportRsp);
private:
    /**
	 * @brief 配置、记录成员
	 */
    int32 m_iLogLevel = log4cplus::INFO_LOG_LEVEL;
    int32 m_iWorkerBeat = 0;                      ///< worker进程心跳超时时间，若大于此心跳未收到worker进程上报，则重启worker进程

    int32 m_iRefreshInterval = 0;                 ///< 刷新Server的间隔周期

    util::E_CODEC_TYPE m_eCodec = util::CODEC_PB_INTERNAL;            ///< 接入端编解码器
    int32 m_iS2SListenFd = -1;                     ///< Server to Server监听文件描述符（Server与Server之间的连接较少，但每个Server的每个Worker均与其他Server的每个Worker相连）
    int32 m_iC2SListenFd = -1;                     ///< Client to Server监听文件描述符（Client与Server之间的连接较多，但每个Client只需连接某个Server的某个Worker）

    ev_timer* m_pPeriodicTaskWatcher = nullptr;              ///< 周期任务定时器

    std::unordered_map<int32, tagWorkerAttr> m_mapWorker;       ///< 业务逻辑工作进程及进程属性，key为pid
    std::unordered_map<int32, int32> m_mapWorkerRestartNum;       ///< 进程被重启次数，key为WorkerIdx
    std::unordered_map<int32, int32> m_mapWorkerFdPid;            ///< 工作进程通信FD对应的进程号
    std::unordered_map<std::string, tagMsgShell> m_mapCenterMsgShell; ///< 到center服务器的连接
    /** 与 m_mapCenterMsgShell 的 key 一致（Center WorkerIdentify）；空表示对所有 Center 上报/注册 */
    std::string m_strRaftLeaderCenterKey;

    std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>> m_mapFdAttr;  ///< 连接的文件描述符属性
    std::unordered_map<uint32, int32> m_mapSeq2WorkerIndex;      ///< 序列号对应的Worker进程编号（用于connect成功后，向对端Manager发送希望连接的Worker进程编号）
    std::unordered_map<in_addr_t, uint32> m_mapClientConnFrequency; ///< 客户端连接频率 (unsigned long,uint32)
    std::unordered_map<int32, std::unique_ptr<Cmd>> m_mapSysCmd;

    std::unordered_map<std::string, std::unordered_map<std::string, Session*> > m_mapCallbackSession;

    int32 m_iConfigProcessPid = -1;
    uint32 m_iConfigProcessStartTime = 0;
    uint32 m_iConfigProcessRestartCounter = 0;
};

} /* namespace net */

#endif /* NodeManager_HPP_ */
