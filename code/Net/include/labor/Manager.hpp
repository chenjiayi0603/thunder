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
#include "labor/ManagerContext.hpp"
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
class Manager : public Labor, private ManagerRuntimeContext
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
     * @brief IoBackend I/O 完成回调（静态）
     */
    static void OnIoComplete(int fd, uint32_t seq, IoOp op, int result, void* user_data);
    /**
     * @brief IoBackend 读/写完成处理
     */
    bool HandleIoReadComplete(tagConnectionAttr* pConn, int result);
    bool HandleIoWriteComplete(tagConnectionAttr* pConn, int result);
    /** @brief send_zc NOTIF 完成（buffer 可复用），驱动回收/重提/倒 pWaitForSendBuff */
    bool HandleIoWriteNotifComplete(tagConnectionAttr* pConn, int result);
    /** @brief 写完成后的回收/重提/倒 pWaitForSendBuff 决策（Write 与 WriteNotif 共用） */
    bool FinishWriteAndDrain(tagConnectionAttr* pConn, int result);
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
};

} /* namespace net */

#endif /* NodeManager_HPP_ */
