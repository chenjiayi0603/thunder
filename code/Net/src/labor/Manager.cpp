/*******************************************************************************
 * Project:  Net
 * @file     Manager.cpp
 * @brief 
 * @author   cjy
 * @date:    2017年7月27日
 * @note
 * Modify history:
 ******************************************************************************/
#include <memory>
#include <string>
#include "protocol/oss_sys.pb.h"
#include "labor/Manager.hpp"
#include "labor/Worker.hpp"
#include "labor/Loader.hpp"
#include "Interface.hpp"

#include "cmd/sys_cmd/CmdMgrBeat.hpp"
#include "cmd/sys_cmd/CmdMgrLogicConfig.hpp"
#include "cmd/sys_cmd/CmdMgrServerConfig.hpp"

namespace net
{
namespace
{
void TrimCenterToken(std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
    {
        ++i;
    }
    s.erase(0, i);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    {
        s.pop_back();
    }
}
} // namespace

void Manager::SignalCallback(struct ev_loop* loop, struct ev_signal* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Manager* pManager = (Manager*)watcher->data;
        if (SIGCHLD == watcher->signum)
        {
            pManager->OnChildTerminated(watcher);
        }
        else if (SIGUSR1 == watcher->signum)
        {
        	pManager->RefreshServer();//kill -SIGUSR1 xxx    killall -SIGUSR1   HelloThunder
        }
        else if (SIGUSR2 == watcher->signum)
        {
            pManager->RestartWorkers();//kill -SIGUSR2 xxx    killall -SIGUSR1   HelloThunder
        }
        else
        {
            pManager->OnManagerTerminated(watcher);
        }
    }
}

void Manager::IdleCallback(struct ev_loop* loop, struct ev_idle* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Manager* pManager = (Manager*)watcher->data;
        pManager->CheckWorker();
        pManager->ReportToCenter();
    }
}

void Manager::IoCallback(struct ev_loop* loop, struct ev_io* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagManagerIoWatcherData* pData = (tagManagerIoWatcherData*)watcher->data;
        Manager* pManager = pData->pManager;
        int iFd = pData->iFd;
        uint64 ulSeq = pData->ulSeq;
        if (revents & EV_READ)
        {
            pManager->IoRead(pData, watcher);
            // IoRead may have called DestroyConnect which frees pData; re-validate before use
            auto iter = pManager->m_mapFdAttr.find(iFd);
            if (iter == pManager->m_mapFdAttr.end() || iter->second->ulSeq != ulSeq)
            {
                return;
            }
        }
        if (revents & EV_WRITE)
        {
            pManager->IoWrite(pData, watcher);
            // IoWrite may have called DestroyConnect (e.g. send error); re-validate before IoError
            auto iterW = pManager->m_mapFdAttr.find(iFd);
            if (iterW == pManager->m_mapFdAttr.end() || iterW->second->ulSeq != ulSeq)
            {
                return;
            }
        }
        if (revents & EV_ERROR)
        {
            pManager->IoError(pData, watcher);
        }
    }
}

void Manager::IoTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagManagerIoWatcherData* pData = (tagManagerIoWatcherData*)watcher->data;
        Manager* pManager = pData->pManager;
        pManager->IoTimeout(pData, watcher);
    }
}

void Manager::PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Manager* pManager = (Manager*)(watcher->data);
// #ifndef NODE_TYPE_CENTER
        pManager->ReportToCenter();//中心节点也需要定时上报心跳管理状态
// #endif
        pManager->CheckWorker();
        pManager->RefreshServer();
        pManager->RefreshEvent(NODE_BEAT,watcher);
    }
}

void Manager::WaitToExitTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagManagerWaitExitWatcherData* pData = (tagManagerWaitExitWatcherData*)(watcher->data);
        Manager* pManager = pData->pManager;
    #ifndef NODE_TYPE_CENTER
        if(CMD_REQ_NODE_STOP == pData->cmd)
        {
        	LOG4_TRACE("%s() todo CMD_REQ_NODE_STOP", __FUNCTION__);
        }
        else if (CMD_REQ_NODE_RESTART_WORKERS == pData->cmd)
        {
        	LOG4_TRACE("%s() todo CMD_REQ_NODE_RESTART_WORKERS", __FUNCTION__);
        }
    #endif
        pManager->DelEvent(watcher,pData);
    }
}

void Manager::ClientConnFrequencyTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        tagClientConnWatcherData* pData = (tagClientConnWatcherData*)watcher->data;
        Manager* pManager = pData->pManager;
        pManager->ClientConnFrequencyTimeout(pData, watcher);
    }
}

void Manager::SessionTimeoutCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents)
{
    if (watcher->data != nullptr)
    {
        Session* pSession = (Session*)watcher->data;
        ((Manager*)GetLabor())->SessionTimeout(pSession, watcher);
    }
}

Manager::Manager(const std::string& strConfFile)
{
	SetConfFile(strConfFile);

	process_daemonize(m_strWorkPath.c_str());
	InstallSignal();

    CreateLoader();//先创建加载进程

    bool boChanged = false;
    if (!LoadConf(boChanged))
    {
        std::cerr << "LoadConf() error!" << std::endl;
        exit(-1);
    }
    SetProcessName(m_oCurrentConf);

    Init();

    CreateEvents();

    PreloadCmd();

    CreateWorker();
    ReportToCenter();

}

Manager::~Manager()
{
    Destroy();
}

bool Manager::OnManagerTerminated(struct ev_signal* watcher)
{
    LOG4_WARN("%s terminated by signal %d!pid(%d)", m_oCurrentConf("server_name").c_str(), watcher->signum,getpid());
    ev_break (m_loop, EVBREAK_ALL);
    Destroy();
    exit(-1);
}

bool Manager::OnChildTerminated(struct ev_signal* watcher)
{
    pid_t   iPid = 0;
    int     iStatus = 0;
    int     iReturnCode = 0;
    //WUNTRACED
    while((iPid = waitpid(-1, &iStatus, WNOHANG)) > 0)
    {
        if (WIFEXITED(iStatus))
        {
            iReturnCode = WEXITSTATUS(iStatus);
        }
        else if (WIFSIGNALED(iStatus))
        {
            iReturnCode = WTERMSIG(iStatus);
        }
        else if (WIFSTOPPED(iStatus))
        {
            iReturnCode = WSTOPSIG(iStatus);
        }

        LOG4_FATAL("error %d: duty %d exit and sent signal %d with code %d!",
                        iStatus, iPid, watcher->signum, iReturnCode);
        RestartWorker(iPid);
    }
    ReportToCenter(true);
    return(true);
}

bool Manager::IoRead(tagManagerIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (watcher->fd == m_iS2SListenFd)//服务间通信的连接
    {
        return(AcceptServerConn(watcher->fd));
    }
    else
    {
        return(RecvDataAndDispose(pData, watcher));
    }
}

bool Manager::AcceptServerConn(int iFd)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    struct sockaddr_in stClientAddr;
    socklen_t clientAddrSize = sizeof(stClientAddr);
    int iAcceptFd = accept(iFd, (struct sockaddr*) &stClientAddr, &clientAddrSize);
    if (iAcceptFd < 0)
    {
        LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        return(false);
    }
	if (CreateAcceptFdAttr(iAcceptFd,GetFdSequence()) == nullptr)
	{
		LOG4_WARN("fail to CreateAcceptFdAttr");
		return(false);
	}
	return true;
}

bool Manager::RecvDataAndDispose(tagManagerIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("fd %d, seq %u", pData->iFd, pData->ulSeq);
    auto conn_iter = m_mapFdAttr.find(pData->iFd);
    if (conn_iter == m_mapFdAttr.end())
    {
        LOG4_ERROR("no fd attr for %d!", pData->iFd);
    }
    else
    {
        int iErrno = 0;
        int iReadLen = 0;
    	tagConnectionAttr* pConn = conn_iter->second.get();
        if (pData->ulSeq != pConn->ulSeq)
        {
            LOG4_TRACE("callback seq %u not match the conn attr seq %u", pData->ulSeq, conn_iter->second->ulSeq);
            DelEvent(watcher,pData);
            return(false);
        }
        iReadLen = pConn->pRecvBuff->ReadFD(pData->iFd, iErrno);
        LOG4_TRACE("recv from fd %d data len %d. and conn_iter->second->pRecvBuff->ReadableBytes() = %zu",
                        pData->iFd, iReadLen, conn_iter->second->pRecvBuff->ReadableBytes());
        if (iReadLen > 0)
        {
            while (pConn->pRecvBuff->ReadableBytes() >= gc_uiMsgHeadSize)
            {
                LOG4_TRACE("pConn->pRecvBuff->ReadableBytes() = %zu", pConn->pRecvBuff->ReadableBytes());
                MsgHead oInMsgHead;
                bool bResult = oInMsgHead.ParseFromArray(pConn->pRecvBuff->GetRawReadBuffer(), gc_uiMsgHeadSize);
                if (bResult)
                {
                    LOG4_TRACE("%s() oInMsgHead(%s)",__FUNCTION__,oInMsgHead.DebugString().c_str());
                    MsgBody oInMsgBody;
                    if (pConn->pRecvBuff->ReadableBytes() >= gc_uiMsgHeadSize + oInMsgHead.msgbody_len())
                    {
                        if (0 == oInMsgHead.msgbody_len())  // 无包体的数据包
                        {
                            bResult = true;
                        }
                        else
                        {
                            bResult = oInMsgBody.ParseFromArray(pConn->pRecvBuff->GetRawReadBuffer() + gc_uiMsgHeadSize, oInMsgHead.msgbody_len());
                        }
                        if (bResult)
                        {
                        	pConn->dActiveTime = ev_now(m_loop);
                            bool bContinue = false;     // 是否继续解析下一个数据包
                            auto worker_fd_iter = m_mapWorkerFdPid.find(watcher->fd);
                            if (worker_fd_iter != m_mapWorkerFdPid.end())   // 其他Server发过来要将连接传送到某个指定Worker进程信息
                            {// 本节点的Worker进程发过来的消息
								bContinue = DisposeDataFromWorker(oInMsgHead, oInMsgBody, pConn);
                            }
                            else
                            {
                                LOG4_DEBUG("Received data from connection. strIdentify: %s. Number of center shells: %zu",
                                           conn_iter->second->strIdentify.c_str(), m_mapCenterMsgShell.size());
                                           
								auto center_iter = m_mapCenterMsgShell.find(pConn->strIdentify);
								const bool bCenterCmd =
									(oInMsgHead.cmd() == CMD_REQ_NODE_REGISTER) ||
									(oInMsgHead.cmd() == CMD_RSP_NODE_REGISTER) ||
									(oInMsgHead.cmd() == CMD_REQ_NODE_STATUS_REPORT) ||
									(oInMsgHead.cmd() == CMD_RSP_NODE_STATUS_REPORT);
								if (center_iter != m_mapCenterMsgShell.end() || bCenterCmd)
								{// center发来的，或尚未建立identify但消息类型已明确为center链路
									bContinue = DisposeDataFromCenter(oInMsgHead, oInMsgBody,pConn);
								}
								else
								{//其他节点（非中心）发来信息
									LOG4_TRACE("No matching center found for strIdentify: %s, proceeding with DisposeDataAndTransferFd. m_mapCenterMsgShell.size()=%zu",
											    pConn->strIdentify.c_str(), m_mapCenterMsgShell.size());
				
									bContinue = DisposeDataAndTransferFd(oInMsgHead, oInMsgBody, pConn);
								}
                            }
                            // 跳过已处理的消息头和消息体字节
                            pConn->pRecvBuff->SkipBytes(gc_uiMsgHeadSize + oInMsgBody.ByteSize());
                    
                            // 如果缓冲区超过32KB，则重新分配内存以节省资源
                            pConn->pRecvBuff->Compact(32784);   
                            pConn->pSendBuff->Compact(32784);
                    
                            if (!bContinue)
                            {
                                DestroyConnect(conn_iter);
                                return(false);
                            }
                        }
                        else
                        {
                            LOG4_ERROR("oInMsgBody.ParseFromArray() failed, data is broken from fd %d, close it!", pData->iFd);
                            DestroyConnect(conn_iter);
                            break;
                        }
                    }
                    else
                    {
                        break;  // 头部数据已完整，但body部分数据不完整
                    }
                }
                else
                {
                    LOG4_ERROR("oInMsgHead.ParseFromArray() failed, data is broken from fd %d, close it!", pData->iFd);
                    DestroyConnect(conn_iter);
                    break;
                }
            }
            return(true);
        }
        else if (iReadLen == 0)
        {
            LOG4_TRACE("fd %d closed by peer, error %d %s!",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
            DestroyConnect(conn_iter);
        }
        else
        {
            if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
            {
                LOG4_ERROR("recv from fd %d error %d: %s",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                DestroyConnect(conn_iter);
            }
        }
    }
    return(true);
}

bool Manager::IoWrite(tagManagerIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("%s(%d)", __FUNCTION__, pData->iFd);
    auto attr_iter =  m_mapFdAttr.find(pData->iFd);
    if (attr_iter == m_mapFdAttr.end())
    {
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = attr_iter->second.get();
        // 检查回调参数中的序列号和文件描述符是否与连接属性匹配
        if ((pData->ulSeq != pConn->ulSeq) || (pData->iFd != pConn->iFd))
        {
            LOG4_TRACE("callback seq %u or ifd(%d) not match the conn attr seq %u or ifd(%d)",
                            pData->ulSeq, pData->iFd, pConn->ulSeq, pConn->iFd);
            DelEvent(watcher,pData);
            return(false);
        }
        int iErrno = 0;
        int iWriteLen = 0;
        int iNeedWriteLen = static_cast<int>(pConn->pSendBuff->ReadableBytes());
        iWriteLen = pConn->pSendBuff->WriteFD(pData->iFd, iErrno);
        LOG4_TRACE("iWriteLen = %d, send to fd %d error %d: %s", iWriteLen,pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
        if (iWriteLen < 0)
        {
            if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
            {
                LOG4_ERROR("send to fd %d error %d: %s",pData->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                DestroyConnect(attr_iter);
            }
            else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
            {
            	pConn->dActiveTime = ev_now(m_loop);
                AddIoWriteEvent(pConn);
            }
        }
        else if (iWriteLen > 0)
        {
            pConn->dActiveTime = ev_now(m_loop);
            if (iWriteLen == iNeedWriteLen) // 已无内容可写，取消监听fd写事件
            {
                RemoveIoWriteEvent(pConn);
            }
            else    // 内容未写完，添加或保持监听fd写事件
            {
                AddIoWriteEvent(pConn);
            }
        }
        else    // iWriteLen == 0 写缓冲区为空
        {
            if (pConn->pWaitForSendBuff->ReadableBytes() > 0)    // 存在等待发送的数据，说明本次写事件是connect之后的第一个写事件
            {
                auto index_iter = m_mapSeq2WorkerIndex.find(pConn->ulSeq);
                if (index_iter != m_mapSeq2WorkerIndex.end())
                {
                    tagMsgShell stMsgShell(pData->iFd,pConn->ulSeq);
                    //AddInnerFd(stMsgShell); 只有Worker需要
                    auto center_iter = m_mapCenterMsgShell.find(pConn->strIdentify);
                    if (center_iter == m_mapCenterMsgShell.end())
                    {
                        m_mapCenterMsgShell.insert(std::make_pair(pConn->strIdentify, stMsgShell));
                    }
                    else
                    {
                        center_iter->second = stMsgShell;
                    }
                    ConnectWorker oConnWorker;
                    oConnWorker.set_worker_index(index_iter->second);
                    m_mapSeq2WorkerIndex.erase(index_iter);
                    LOG4_TRACE("send after connect");
                    SendTo(stMsgShell, CMD_REQ_CONNECT_TO_WORKER,GetSequence(), oConnWorker.SerializeAsString());
                }
            }
        }
        return(true);
    }
}

bool Manager::IoError(tagManagerIoWatcherData* pData, struct ev_io* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter =  m_mapFdAttr.find(pData->iFd);
    if (iter == m_mapFdAttr.end())
    {
        return(false);
    }
    else
    {
        if (pData->ulSeq != iter->second->ulSeq || pData->iFd != iter->second->iFd)
        {
            LOG4_TRACE("callback seq %u not match the conn attr seq %u", pData->ulSeq, iter->second->ulSeq);
            DelEvent(watcher,pData);
            return(false);
        }
        int iFdSaved = pData->iFd;
        DestroyConnect(iter);
        auto worker_fd_iter = m_mapWorkerFdPid.find(iFdSaved);
        if (worker_fd_iter != m_mapWorkerFdPid.end())
        {
            kill(worker_fd_iter->first, SIGINT);
        }
    }
    return(true);
}

bool Manager::IoTimeout(tagManagerIoWatcherData* pData, struct ev_timer* watcher)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    bool bRes = false;
    auto iter =  m_mapFdAttr.find(pData->iFd);
    if (iter == m_mapFdAttr.end())
    {
        bRes = false;
    }
    else
    {
        if (pData->ulSeq != iter->second->ulSeq || pData->iFd != iter->second->iFd)      // 文件描述符数值相等，但已不是原来的文件描述符
        {
            bRes = false;
        }
        else
        {
            ev_tstamp after = iter->second->dActiveTime - ev_now(m_loop) + m_dIoTimeout;
            LOG4_TRACE("ev_timer_set(watcher) time(%lf) dKeepAlive(%lf)",ev_now(m_loop),iter->second->dKeepAlive);
            if (after > 0)    // IO在定时时间内被重新刷新过，重新设置定时器
            {
                LOG4_TRACE("%s() ev_timer_set(%lf)", __FUNCTION__,after + ev_time() - ev_now(m_loop));
                RefreshEvent(after,watcher);
                return(true);
            }
            else    // IO已超时，关闭文件描述符并清理相关资源
            {
                LOG4_WARN("%s()", __FUNCTION__);
                DestroyConnect(iter);
            }
            bRes = true;
        }
    }

    DelEvent(watcher,pData);
    return(bRes);
}

bool Manager::ClientConnFrequencyTimeout(tagClientConnWatcherData* pData, struct ev_timer* watcher)
{
    //LOG4_TRACE("%s()", __FUNCTION__);
    bool bRes = false;
    auto iter = m_mapClientConnFrequency.find(pData->iAddr);
    if (iter == m_mapClientConnFrequency.end())
    {
        bRes = false;
    }
    else
    {
        m_mapClientConnFrequency.erase(iter);
        bRes = true;
    }
    DelEvent(watcher,pData);
    return(bRes);
}

void Manager::Run()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_run (m_loop, 0);
}

void Manager::SetProcessName(const util::CJsonObject& oJsonConf)
{
	std::string strServerName = std::move(oJsonConf("server_name"));
	if (strServerName.size() > 0 && m_strServerName != strServerName)
	{
		ngx_setproctitle(strServerName.c_str());
		m_strServerName = strServerName;
	}
}

bool Manager::SendTo(const tagMsgShell& stMsgShell)
{
    auto iter = m_mapFdAttr.find(stMsgShell.iFd);
    if (iter == m_mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in m_mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = iter->second.get();
        if (pConn->ulSeq == stMsgShell.ulSeq && pConn->iFd == stMsgShell.iFd)
        {
            int iErrno = 0;
            int iWriteLen = 0;
            int iNeedWriteLen = static_cast<int>(pConn->pWaitForSendBuff->ReadableBytes());
            int iWriteIdx = pConn->pSendBuff->GetWriteIndex();
            iWriteLen = pConn->pSendBuff->Write(pConn->pWaitForSendBuff.get(), pConn->pWaitForSendBuff->ReadableBytes());
            if (iWriteLen == iNeedWriteLen)
            {
                iNeedWriteLen = static_cast<int>(pConn->pSendBuff->ReadableBytes());
                iWriteLen = iter->second->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
                if (iWriteLen < 0)
                {
                    if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
                    {
                        LOG4_ERROR("send to fd %d error %d: %s",stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        DestroyConnect(iter);
                        return(false);
                    }
                    else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
                    {
                        iter->second->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                }
                else if (iWriteLen > 0)
                {
                    iter->second->dActiveTime = ev_now(m_loop);
                    if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
                    {
                        RemoveIoWriteEvent(pConn);
                    }
                    else    // 内容未写完，添加或保持监听fd写事件
                    {
                        AddIoWriteEvent(pConn);
                    }
                }
                return(true);
            }
            else
            {
                LOG4_ERROR("write to send buff error, iWriteLen = %d!", iWriteLen);
                pConn->pSendBuff->SetWriteIndex(iWriteIdx);
                return(false);
            }
        }
        else
        {
            LOG4_ERROR("pConn iFd(%d) ulSeq(%u) stMsgShell iFd(%d) ulSeq(%u) not match!",
                            pConn->iFd, pConn->ulSeq, stMsgShell.iFd, stMsgShell.ulSeq);
        }
    }
    return(false);
}

bool Manager::SendTo(const tagMsgShell& stMsgShell, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(cmd %u, seq %u)", __FUNCTION__, oMsgHead.cmd(), oMsgHead.seq());
    auto iter = m_mapFdAttr.find(stMsgShell.iFd);
    if (iter == m_mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in m_mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
    	tagConnectionAttr* pConn = iter->second.get();
        if (pConn->ulSeq == stMsgShell.ulSeq && pConn->iFd == stMsgShell.iFd)
        {
            int iErrno = 0;
            int iWriteLen = 0;

            int iNeedWriteLen = oMsgHead.ByteSize();
            int iWriteIdx = pConn->pSendBuff->GetWriteIndex();
            iWriteLen = pConn->pSendBuff->Write(oMsgHead.SerializeAsString().c_str(), oMsgHead.ByteSize());
            LOG4_TRACE("iWriteLen = %d,oSwitchMsgHead size(%u),oMsgHead(%s)",
                            iWriteLen,oMsgHead.ByteSize(),oMsgHead.DebugString().c_str());
            if (iWriteLen != iNeedWriteLen)
            {
                LOG4_ERROR("write to send buff error, iWriteLen = %d!", iWriteLen);
                pConn->pSendBuff->SetWriteIndex(iWriteIdx);
                return(false);
            }
            iNeedWriteLen = oMsgBody.ByteSize();
            iWriteLen = pConn->pSendBuff->Write(oMsgBody.SerializeAsString().c_str(), oMsgBody.ByteSize());
            LOG4_TRACE("iWriteLen = %d,oMsgBody size(%u)", iWriteLen,oMsgBody.ByteSize());
            if (iWriteLen == iNeedWriteLen)
            {
                iNeedWriteLen = static_cast<int>(pConn->pSendBuff->ReadableBytes());
                iWriteLen = pConn->pSendBuff->WriteFD(stMsgShell.iFd, iErrno);
                LOG4_TRACE("iWriteLen = %d, send to fd %d error %d: %s", iWriteLen,
                                stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                if (iWriteLen < 0)
                {
                    if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
                    {
                        LOG4_ERROR("send to fd %d error %d: %s",
                                        stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                        DestroyConnect(iter);
                        return(false);
                    }
                    else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
                    {
                        iter->second->dActiveTime = ev_now(m_loop);
                        AddIoWriteEvent(pConn);
                    }
                }
                else if (iWriteLen > 0)
                {
                    iter->second->dActiveTime = ev_now(m_loop);
                    if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
                    {
                        RemoveIoWriteEvent(pConn);
                    }
                    else    // 内容未写完，添加或保持监听fd写事件
                    {
                        AddIoWriteEvent(pConn);
                    }
                }
                return(true);
            }
            else
            {
                LOG4_ERROR("write to send buff error!");
                pConn->pSendBuff->SetWriteIndex(iWriteIdx);
                return(false);
            }
        }
        else
        {
            LOG4_ERROR("fd %d sequence %u not match the iFd(%d) sequence %u in m_mapFdAttr",
                            stMsgShell.iFd, stMsgShell.ulSeq, iter->second->iFd,iter->second->ulSeq);
            return(false);
        }
    }
}

bool Manager::SendTo(const tagMsgShell& stMsgShell,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(stMsgShell, oOutMsgHead, oOutMsgBody);
}

bool Manager::SetConnectIdentify(const tagMsgShell& stMsgShell, const std::string& strIdentify)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = m_mapFdAttr.find(stMsgShell.iFd);
    if (iter == m_mapFdAttr.end())
    {
        LOG4_ERROR("no fd %d found in m_mapFdAttr", stMsgShell.iFd);
        return(false);
    }
    else
    {
        if (iter->second->ulSeq == stMsgShell.ulSeq)
        {
            iter->second->strIdentify = strIdentify;
            return(true);
        }
        else
        {
            LOG4_ERROR("fd %d sequence %u not match the sequence %u in m_mapFdAttr",
                            stMsgShell.iFd, stMsgShell.ulSeq, iter->second->ulSeq);
            return(false);
        }
    }
}

bool Manager::AutoSend(const std::string& strIdentify, const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    LOG4_TRACE("%s(%s)", __FUNCTION__, strIdentify.c_str());
    int iPosIpPortSeparator = strIdentify.find(':');
    if (iPosIpPortSeparator == std::string::npos)
	{
		LOG4_ERROR("iPosIpPortSeparator == std::string::npos");
		return(false);
	}
    int iPosPortWorkerIndexSeparator = strIdentify.rfind('.');
    std::string strHost = strIdentify.substr(0, iPosIpPortSeparator);
    std::string strPort = strIdentify.substr(iPosIpPortSeparator + 1, iPosPortWorkerIndexSeparator - (iPosIpPortSeparator + 1));
    std::string strWorkerIndex = strIdentify.substr(iPosPortWorkerIndexSeparator + 1, std::string::npos);
    int iPort = atoi(strPort.c_str());
    if (iPort == 0)
	{
		LOG4_ERROR("%d == 0",iPort);
		return(false);
	}
    int iWorkerIndex = atoi(strWorkerIndex.c_str());
    if (iWorkerIndex > 1000)
	{
		LOG4_ERROR("%d  > 1000",iWorkerIndex);
		return(false);
	}
    struct sockaddr addr;
	int iFd = -1;
	if (!HostPort2SockAddr(strHost,iPort,addr,iFd))
	{
		LOG4_ERROR("!HostPort2SockAddr");
		return(false);
	}
    auto worker_fd_iter = m_mapWorkerFdPid.find(iFd);
    if (worker_fd_iter != m_mapWorkerFdPid.end())
    {
        LOG4_TRACE("iFd = %d found in m_mapWorkerFdPid", iFd);
    }
    uint32 ulSeq = GetFdSequence();
    tagConnectionAttr* pConn = CreateFdAttr(iFd, ulSeq);
    if (pConn)
    {
        if(!AddIoTimeout(iFd, ulSeq, 1.5))
        {
        	LOG4_ERROR("AddIoTimeout error");
			DestroyConnect(m_mapFdAttr.find(iFd));
			return(false);
        }
        if (!AddIoReadEvent(pConn))
		{
			LOG4_ERROR("AddIoReadEvent error");
			DestroyConnect(m_mapFdAttr.find(iFd));
			return(false);
		}
		if (!AddIoWriteEvent(pConn))
		{
			LOG4_ERROR("AddIoWriteEvent error");
			DestroyConnect(m_mapFdAttr.find(iFd));
			return(false);
		}
		pConn->pWaitForSendBuff->Write(oMsgHead.SerializeAsString().c_str(), oMsgHead.ByteSize());
		LOG4_TRACE("%s(),write oMsgHead size(%u)", __FUNCTION__,oMsgHead.ByteSize());
		pConn->pWaitForSendBuff->Write(oMsgBody.SerializeAsString().c_str(), oMsgBody.ByteSize());
		LOG4_TRACE("%s(),write oMsgBody size(%u)", __FUNCTION__,oMsgBody.ByteSize());
		pConn->strIdentify = strIdentify;
        LOG4_TRACE("fd %d seq %u identify %s.iter->second->pWaitForSendBuff->ReadableBytes()=%zu",
                        iFd, ulSeq, strIdentify.c_str(), pConn->pWaitForSendBuff->ReadableBytes());
		m_mapSeq2WorkerIndex.insert(std::make_pair(ulSeq, iWorkerIndex));
		auto center_iter = m_mapCenterMsgShell.find(strIdentify);
		if (center_iter != m_mapCenterMsgShell.end())
		{
			center_iter->second.iFd = iFd;
			center_iter->second.ulSeq = ulSeq;
		}
        if (connect(iFd, &addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
        {
            LOG4_ERROR("connect error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
            DestroyConnect(m_mapFdAttr.find(iFd));
            return false;
        }
		return(true);
    }
    else    // 没有足够资源分配给新连接，直接close掉
    {
        close(iFd);
        return(false);
    }
}

void Manager::SetConfFile(const std::string& strConfFile)
{
	if (strConfFile == "")
	{
		std::cerr << "error: no config file!" << std::endl;
		exit(1);
	}
	m_strConfFile = strConfFile;
	char szFilePath[256] = {0};
	if (m_strWorkPath.length() == 0)
	{
		if (getcwd(szFilePath, sizeof(szFilePath)))
		{
			m_strWorkPath = szFilePath;
		}
		else
		{
			std::cerr << "error: getcwd!" << std::endl;
			exit(1);
		}
	}
}

bool Manager::LoadConf(bool & boChanged)
{
	if (GetLoaderConfigVersionData().IsLoaderProcess())//支持加载进程处理配置，才检查（加载进程需管理服务配置）
	{
		if (GetLoaderConfigVersionData().IsConfigVersionChange())
		{
			GetLoaderConfigVersionData().UpdateLoaderConfigVersion();
			SAFE_LOG4_INFO("%s check for config change.", __FUNCTION__);
		}
		else
		{//配置无变化，且已有旧的配置
			if (!m_oLastConf.IsEmpty())
			{
				SAFE_LOG4_INFO("%s no config change.", __FUNCTION__);
				return true;
			}
		}
	}
	//配置已变化

	m_oLastConf = m_oCurrentConf;

	bool boReadConfig(false);
	std::string configContent;
	if (GetLoaderConfigVersionData().GetServerConfigFile(configContent))
	{
		if (m_oCurrentConf.Parse(configContent))
		{
			SAFE_LOG4_INFO("%s Parse config from mem ok.", __FUNCTION__);
			boReadConfig = true;
		}
		else
		{
			SAFE_LOG4_ERROR("%s Parse config failed.", __FUNCTION__);
		}
	}

	if (!boReadConfig)
	{
	    if (!net::GetConfig(m_oCurrentConf,m_strConfFile))//读取配置文件
		{
	    	if (IsInitLogger())
			{
				LOG4_INFO("%s Open conf to read error!", __FUNCTION__);
			}
	    	else
	    	{
	    		std::cerr << "Open conf to read error!" << std::endl;
	    	}
			return false;
		}
	    else
	    {
			SAFE_LOG4_INFO("%s Parse config from file ok.", __FUNCTION__);
	    }
	}

	if (m_oLastConf.ToString() != m_oCurrentConf.ToString())
	{
		boChanged = true;
		m_oCustomConf = m_oCurrentConf["custom"];
		m_oCustomConf.Get("cat_log_system", m_bCatLogSystem);

		m_oCurrentConf.Get("io_timeout", m_dIoTimeout);
		m_oCurrentConf.Get("refresh_interval", m_iRefreshInterval);
		m_oCurrentConf.Get("worker_beat", m_iWorkerBeat);

		if (m_oLastConf.ToString().length() == 0)//第一次加载处理
		{
			m_uiWorkerNum = strtoul(m_oCurrentConf("process_num").c_str(), NULL, 10);
			m_oCurrentConf.Get("node_type", m_strNodeType);
			m_oCurrentConf.Get("inner_host", m_strHostForServer);
			if (m_strHostForServer.size() == 0)
			{
				if (GetHostForServer().size() == 0)
				{
					std::cerr << "DomainToIP "<< m_pErrBuff <<" error!" << std::endl;
					return false;
				}
			}
			m_oCurrentConf.Get("inner_port", m_iPortForServer);
			if (m_oCurrentConf.Get("access_host", m_strHostForClient) && m_strHostForClient.size() == 0)
			{
				if (GetHostForClient().size() == 0)
				{
					std::cerr << "DomainToIP "<< m_pErrBuff <<" error!" << std::endl;
					return false;
				}
			}
			m_oCurrentConf.Get("access_port", m_iPortForClient);
			m_oCurrentConf.Get("gateway", m_strGateway);
			m_oCurrentConf.Get("gateway_port", m_iGatewayPort);
			m_oCurrentConf.Get("client_socket_backlog", m_iClientSocketBackLog);
			m_oCurrentConf.Get("server_socket_backlog", m_iServerSocketBackLog);
		}
		int32 iCodec;
		if (m_oCurrentConf.Get("access_codec", iCodec))
		{
			m_eCodec = util::E_CODEC_TYPE(iCodec);
		}
		m_oCurrentConf["permission"]["addr_permit"].Get("stat_interval", m_dAddrStatInterval);
		m_oCurrentConf["permission"]["addr_permit"].Get("permit_num", m_iAddrPermitNum);
		m_iLogLevel = log4cplus::INFO_LOG_LEVEL;
		(void)ResolveLogLevelFromConf(m_oCurrentConf, m_iLogLevel);
	}
    return(true);
}

bool Manager::Init()
{
	nPid = getpid();
    InitLogger(m_oCurrentConf);
    LOG4_INFO("%s program begin, and work path %s. pid(%d)", m_strServerName.c_str(), m_strWorkPath.c_str(),nPid);

    if (m_strHostForClient.size() > 0 && m_iPortForClient > 0)
    {
        LOG4_INFO("%s() C-scheme enabled, manager always skip client listen.", __FUNCTION__);
    }
    int iFd = -1;
	struct sockaddr addr;
	if (!HostPort2SockAddr(m_strHostForServer,m_iPortForServer,addr,iFd,true))
	{
		LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
		int iErrno = errno;
		exit(iErrno);
	}
	m_iS2SListenFd = iFd;
    if (bind(m_iS2SListenFd, &addr, sizeof(addr)) < 0)
    {
        LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        CloseSocket(m_iS2SListenFd);
        int iErrno = errno;
        exit(iErrno);
    }
    if (listen(m_iS2SListenFd, m_iServerSocketBackLog) < 0)
    {
        LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        CloseSocket(m_iS2SListenFd);
        int iErrno = errno;
        exit(iErrno);
    }
    LOG4_INFO("%s() pid(%d) listen on iPortForServer(%d) strHostForServer(%s) iServerSocketBackLog(%d)",
    		__FUNCTION__,nPid,m_iPortForServer,m_strHostForServer.c_str(),m_iServerSocketBackLog);
    // 创建到Center的连接信息（center 可为 JSON 数组或逗号分隔的 host:port 字符串）
    auto insertCenterIdentify = [this](const std::string& strIdentify)
    {
        tagMsgShell stMsgShell;
        LOG4_TRACE("m_mapCenterMsgShell.insert(%s, fd %d, seq %u)", strIdentify.c_str(),
                        stMsgShell.iFd, stMsgShell.ulSeq);
        m_mapCenterMsgShell.insert(std::make_pair(strIdentify, stMsgShell));
    };
    util::CJsonObject& oCenter = m_oCurrentConf["center"];
    if (oCenter.IsArray() && oCenter.GetArraySize() > 0)
    {
        for (int i = 0; i < oCenter.GetArraySize(); ++i)
        {
            // CenterServer 只有一个 Worker
            std::string strIdentify = oCenter[i]("host") + std::string(":") + oCenter[i]("port") + std::string(".0");
            insertCenterIdentify(strIdentify);
        }
    }
    else
    {
        std::string csv;
        if (m_oCurrentConf.Get("center", csv))
        {
            size_t start = 0;
            while (start < csv.size())
            {
                size_t comma = csv.find(',', start);
                std::string token = (comma == std::string::npos) ? csv.substr(start) : csv.substr(start, comma - start);
                start = (comma == std::string::npos) ? csv.size() : comma + 1;
                TrimCenterToken(token);
                if (token.empty())
                {
                    continue;
                }
                size_t colon = token.rfind(':');
                if (colon == std::string::npos || colon + 1 >= token.size())
                {
                    LOG4_WARN("skip invalid center entry (expect host:port): %s", token.c_str());
                    continue;
                }
                std::string host = token.substr(0, colon);
                std::string port = token.substr(colon + 1);
                TrimCenterToken(host);
                TrimCenterToken(port);
                if (host.empty() || port.empty())
                {
                    continue;
                }
                insertCenterIdentify(host + std::string(":") + port + std::string(".0"));
            }
        }
    }
#ifdef WORKER_OVERDUE
    int iWorkerBeat = WORKER_OVERDUE > ((gc_iBeatInterval << 1) + 1) ? WORKER_OVERDUE:((gc_iBeatInterval << 1) + 1);
    if (iWorkerBeat > m_iWorkerBeat) m_iWorkerBeat = iWorkerBeat;
#else
    int iWorkerBeat = (gc_iBeatInterval << 1) + 1;
    if (iWorkerBeat > m_iWorkerBeat) m_iWorkerBeat = iWorkerBeat;
#endif
    LOG4_TRACE("%s() m_iWorkerBeat:%d gc_iBeatInterval:%d,NODE_BEAT:%f", __FUNCTION__,m_iWorkerBeat,gc_iBeatInterval,NODE_BEAT);
    return(true);
}

void Manager::Destroy()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_mapSysCmd.clear();
    m_mapWorker.clear();
    m_mapWorkerFdPid.clear();
    m_mapWorkerRestartNum.clear();
    while (!m_mapFdAttr.empty())
    {
        DestroyConnect(m_mapFdAttr.begin());
    }
    m_mapClientConnFrequency.clear();

    free(m_pPeriodicTaskWatcher); m_pPeriodicTaskWatcher = nullptr;
    if (m_loop != nullptr)
    {
        StopPostToEventLoop();
        ev_loop_destroy(m_loop);
        m_loop = nullptr;
    }
    GetLoaderConfigVersionData().DelLoaderConfigVersionMM();
    GetRouteNoticeVersionData().DelRouteNoticeVersionMM();
    GetCustomConfigVersionData().DelCustomConfigVersionMM();
}

void Manager::CreateLoader(bool boRestart)
{
	util::CJsonObject oCurrentConf;
    if (!net::GetConfig(oCurrentConf,m_strConfFile))//第一次读取的初始配置文件
	{
    	if (IsInitLogger())
    	{
    		SAFE_LOG4_ERROR("Open conf to read error!");
    	}
    	else
    	{
    		std::cerr << "Open conf to read error!" << std::endl;
    	}
		exit(-1);
		return;
	}
    SetProcessName(oCurrentConf);//先设置进程名
    // Loader 是否启用仅由配置项控制，默认关闭。
    GetLoaderConfigVersionData().m_bLoaderProcess = false;
    oCurrentConf.Get("loader_process", GetLoaderConfigVersionData().m_bLoaderProcess);

    if (GetLoaderConfigVersionData().IsLoaderProcess())
	{
		SAFE_LOG4_INFO("%s fork Loader", __FUNCTION__);
		LoaderConfigVersionData::LoaderConfigVersionMM *pLoaderConfigVersionMM = GetLoaderConfigVersionData().GetLoaderConfigVersionMM();//需要先创建
		RouteNoticeVersionData::RouteNoticeVersionMM *pRouteNoticeVersionMM = GetRouteNoticeVersionData().GetRouteNoticeVersionMM();
        CustomConfigVersionData::CustomConfigVersionMM *pCustomConfigVersionMM = GetCustomConfigVersionData().GetCustomConfigVersionMM();
		pid_t iPid = fork();
		if (iPid == 0)   // 子进程
		{
			StopPostToEventLoop();
			ev_loop_destroy(m_loop);
			CloseSocket(m_iS2SListenFd);
			Loader* pLoader = new Loader(m_strWorkPath,m_strConfFile,oCurrentConf,pLoaderConfigVersionMM);
			pLoader->GetRouteNoticeVersionData().SetRouteNoticeVersionMM(pRouteNoticeVersionMM);
            pLoader->GetCustomConfigVersionData().SetCustomConfigVersionMM(pCustomConfigVersionMM);
			pLoader->Run();
			LOG4_FATAL("Loader terminated");
			delete pLoader;
			exit(-2);
		}
		else if (iPid > 0)   // 父进程
		{
			m_iConfigProcessPid = iPid;
			if (0 == m_iConfigProcessStartTime)
			{
				m_iConfigProcessStartTime = util::GetSecond();
			}
			if (!boRestart)
			{
				int iSleepCounter(0);
				while (!GetLoaderConfigVersionData().IsConfigVersionChange() && (iSleepCounter < 3000))
				{//等待拉取配置,最多等待3秒
					usleep(1000);
					++iSleepCounter;
				}
//				GetLoaderConfigVersionData().UpdateLoaderConfigVersion();
			}
		}
		else
		{
			std::cerr << "error "<<  errno << ":" << strerror_r(errno, m_pErrBuff, gc_iErrBuffLen) << std::endl;
			exit(1);
		}
	}
}

void Manager::CreateWorker()
{
    LOG4_TRACE("%s", __FUNCTION__);
    LoaderConfigVersionData::LoaderConfigVersionMM *pLoaderConfigVersionMM = GetLoaderConfigVersionData().GetLoaderConfigVersionMM();
    RouteNoticeVersionData::RouteNoticeVersionMM *pRouteNoticeVersionMM = GetRouteNoticeVersionData().GetRouteNoticeVersionMM();
    CustomConfigVersionData::CustomConfigVersionMM *pCustomConfigVersionMM = GetCustomConfigVersionData().GetCustomConfigVersionMM();
    for (unsigned int i = 0; i < m_uiWorkerNum; ++i)
    {
        int iControlFds[2];
        int iDataFds[2];
        if (socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds) < 0)
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        }
        if (socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds) < 0)
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        }

        int iPid = fork();
        if (iPid == 0)   // 子进程
        {
            StopPostToEventLoop();
            ev_loop_destroy(m_loop);
            CloseSocket(m_iS2SListenFd);
            close(iControlFds[0]);
            close(iDataFds[0]);
            x_sock_set_block(iControlFds[1], 0);
            x_sock_set_block(iDataFds[1], 0);
            Worker* pWorker = new Worker(m_strWorkPath, iControlFds[1], iDataFds[1], i, m_oCurrentConf);
            pWorker->GetLoaderConfigVersionData().SetLoaderConfigVersionMM(pLoaderConfigVersionMM);
            pWorker->GetRouteNoticeVersionData().SetRouteNoticeVersionMM(pRouteNoticeVersionMM);
            pWorker->GetCustomConfigVersionData().SetCustomConfigVersionMM(pCustomConfigVersionMM);
            pWorker->Run();
            LOG4_FATAL("Worker terminated");
            delete pWorker;
            exit(-2);
        }
        else if (iPid > 0)   // 父进程
        {
            close(iControlFds[1]);
            close(iDataFds[1]);
            x_sock_set_block(iControlFds[0], 0);
            x_sock_set_block(iDataFds[0], 0);
            tagWorkerAttr stWorkerAttr;
            stWorkerAttr.iWorkerIndex = i;
            stWorkerAttr.iControlFd = iControlFds[0];
            stWorkerAttr.iDataFd = iDataFds[0];
            m_mapWorker.insert(std::pair<int, tagWorkerAttr>(iPid, stWorkerAttr));
            m_mapWorkerFdPid.insert(std::make_pair(iControlFds[0], iPid));
            m_mapWorkerFdPid.insert(std::make_pair(iDataFds[0], iPid));
            if (CreateReadFdAttr(iControlFds[0], GetFdSequence()) == nullptr)
			{
				LOG4_ERROR("%s() failed to CreateReadFdAttr for iControlFds[0] %d",__FUNCTION__,iControlFds[0]);
			}
            if (CreateReadFdAttr(iDataFds[0], GetFdSequence()) == nullptr)
			{
				LOG4_ERROR("%s() failed to CreateReadFdAttr for iDataFds[0] %d",__FUNCTION__,iDataFds[0]);
			}
        }
        else
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, m_pErrBuff, gc_iErrBuffLen));
        }
    }
}

bool Manager::CreateEvents()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_loop = ev_loop_new(EVFLAG_FORKCHECK | EVFLAG_SIGNALFD |EVBACKEND_EPOLL);
    if (m_loop == nullptr)
    {
        return(false);
    }
    if (CreateReadFdAttr(m_iS2SListenFd, GetFdSequence()) == nullptr)
    {
    	LOG4_ERROR("%s() failed to CreateReadFdAttr for m_iS2SListenFd %d",__FUNCTION__,m_iS2SListenFd);
    	return(false);
    }

    AddSignal(SIGCHLD,SignalCallback);
    //SIGINT
    AddSignal(SIGILL,SignalCallback);
    AddSignal(SIGBUS,SignalCallback);
    AddSignal(SIGFPE,SignalCallback);
    AddSignal(SIGKILL,SignalCallback);
    AddSignal(SIGTERM,SignalCallback);
	AddSignal(SIGUSR1,SignalCallback);//自定义信号1 （处理进程初始化）
    AddSignal(SIGUSR2,SignalCallback);//自定义信号2（处理进程初始化）

    AddPeriodicTaskEvent();
    InitPostToEventLoop();
    return(true);
}

void Manager::AddCmd(Cmd* pCmd,int iCmd)
{
	pCmd->SetCmd(iCmd);
	m_mapSysCmd.insert(std::make_pair(iCmd, std::unique_ptr<Cmd>(pCmd)));
}

void Manager::PreloadCmd()
{
	AddCmd(new CmdMgrBeat(),CMD_REQ_BEAT);
	AddCmd(new CmdMgrLogicConfig(),CMD_REQ_RELOAD_LOGIC_CONFIG);
	AddCmd(new CmdMgrServerConfig(),CMD_REQ_SERVER_CONFIG);
}

bool Manager::RestartWorker(int iDeathPid)
{
    LOG4_TRACE("%s(%d)", __FUNCTION__, iDeathPid);
    auto worker_iter = m_mapWorker.find(iDeathPid);
    if (worker_iter != m_mapWorker.end())
    {
        int iNewPid = 0;
        char errMsg[1024] = {0};
        LOG4_TRACE("restart worker %d, close control fd %d and data fd %d first.",worker_iter->second.iWorkerIndex, worker_iter->second.iControlFd, worker_iter->second.iDataFd);
        int iWorkerIndex = worker_iter->second.iWorkerIndex;
        auto fd_iter = m_mapWorkerFdPid.find(worker_iter->second.iControlFd);
        if (fd_iter != m_mapWorkerFdPid.end())
        {
            m_mapWorkerFdPid.erase(fd_iter);
        }
        fd_iter = m_mapWorkerFdPid.find(worker_iter->second.iDataFd);
        if (fd_iter != m_mapWorkerFdPid.end())
        {
            m_mapWorkerFdPid.erase(fd_iter);
        }
        DestroyConnect(m_mapFdAttr.find(worker_iter->second.iControlFd));
        DestroyConnect(m_mapFdAttr.find(worker_iter->second.iDataFd));
        m_mapWorker.erase(worker_iter);

		LOG4_INFO("worker %d had been restarted %d times!", iWorkerIndex, m_mapWorkerRestartNum[iWorkerIndex]);
        //父子进程使用unixsocket通信
        int iControlFds[2];
        int iDataFds[2];
        if (socketpair(PF_UNIX, SOCK_STREAM, 0, iControlFds) < 0)
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, errMsg, 1024));
        }
        if (socketpair(PF_UNIX, SOCK_STREAM, 0, iDataFds) < 0)
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, errMsg, 1024));
        }
        LoaderConfigVersionData::LoaderConfigVersionMM *pLoaderConfigVersionMM = GetLoaderConfigVersionData().GetLoaderConfigVersionMM();
        RouteNoticeVersionData::RouteNoticeVersionMM *pRouteNoticeVersionMM = GetRouteNoticeVersionData().GetRouteNoticeVersionMM();
        CustomConfigVersionData::CustomConfigVersionMM *pCustomConfigVersionMM = GetCustomConfigVersionData().GetCustomConfigVersionMM();
        iNewPid = fork();
        if (iNewPid == 0)   // 子进程
        {
            StopPostToEventLoop();
            ev_loop_destroy(m_loop);
            CloseSocket(m_iS2SListenFd);
            close(iControlFds[0]);
            close(iDataFds[0]);
            x_sock_set_block(iControlFds[1], 0);
            x_sock_set_block(iDataFds[1], 0);
            sleep(1);// 子进程重启避免过快重启导致快速触发错误
            Worker* pWorker = new Worker(m_strWorkPath, iControlFds[1], iDataFds[1], iWorkerIndex, m_oCurrentConf);
            pWorker->GetLoaderConfigVersionData().SetLoaderConfigVersionMM(pLoaderConfigVersionMM);
            pWorker->GetRouteNoticeVersionData().SetRouteNoticeVersionMM(pRouteNoticeVersionMM);
            pWorker->GetCustomConfigVersionData().SetCustomConfigVersionMM(pCustomConfigVersionMM);
            pWorker->Run();
            LOG4_FATAL("Worker terminated");
            delete pWorker;
            exit(-2);   // 子进程worker没有正常运行
        }
        else if (iNewPid > 0)   // 父进程
        {
            LOG4_INFO("worker %d restart successfully", iWorkerIndex);
            ev_loop_fork(m_loop);
            close(iControlFds[1]);
            close(iDataFds[1]);
            x_sock_set_block(iControlFds[0], 0);
            x_sock_set_block(iDataFds[0], 0);
            tagWorkerAttr stWorkerAttr;
            stWorkerAttr.iWorkerIndex = iWorkerIndex;
            stWorkerAttr.iControlFd = iControlFds[0];
            stWorkerAttr.iDataFd = iDataFds[0];
            LOG4_TRACE("m_mapWorker insert (iNewPid %d, worker_index %d)", iNewPid, iWorkerIndex);
            m_mapWorker.insert(std::make_pair(iNewPid, stWorkerAttr));
            m_mapWorkerFdPid.insert(std::make_pair(iControlFds[0], iNewPid));
            m_mapWorkerFdPid.insert(std::make_pair(iDataFds[0], iNewPid));
            CreateReadFdAttr(iControlFds[0], GetFdSequence());
            CreateReadFdAttr(iDataFds[0], GetFdSequence());
            m_mapWorkerRestartNum[iWorkerIndex]++;
            ReportToCenter();     // 重启Worker进程后向Center重发注册请求，以获得center下发其他节点的信息
            return(true);
        }
        else
        {
            LOG4_ERROR("error %d: %s", errno, strerror_r(errno, errMsg, 1024));
        }
    }
    else if (m_iConfigProcessPid >= 0 && iDeathPid == m_iConfigProcessPid)
    {
    	SAFE_LOG4_INFO("%s restart Loader", __FUNCTION__);
		if (++m_iConfigProcessRestartCounter >= 3)
		{
			if (util::GetSecond() <= m_iConfigProcessStartTime + 20)//节点启动时Loader重启次数太多的存在问题，不再重启
			{
				SAFE_LOG4_ERROR("%s restart Loader too many,failed to restart", __FUNCTION__);
				return(false);
			}
		}
    	CreateLoader(true);
    }
    return(false);
}

bool Manager::AddPeriodicTaskEvent()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_pPeriodicTaskWatcher = (ev_timer*)malloc(sizeof(ev_timer));
    if (m_pPeriodicTaskWatcher == nullptr)
    {
        LOG4_ERROR("new timeout_watcher error!");
        return(false);
    }
    m_pPeriodicTaskWatcher->data = static_cast<void*>(this);
    AddEvent(NODE_BEAT,m_pPeriodicTaskWatcher,PeriodicTaskCallback);
    return(true);
}

bool Manager::AddIoReadEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s(fd %d)", __FUNCTION__, pConn->iFd);
    ev_io* io_watcher = nullptr;
	if (nullptr == pConn->pIoWatcher)
	{
		io_watcher = new ev_io();
		if (io_watcher == nullptr)
		{
			LOG4_ERROR("new io_watcher error!");
			return(false);
		}
		tagManagerIoWatcherData* pData = new tagManagerIoWatcherData();
		if (pData == nullptr)
		{
			LOG4_ERROR("new tagIoWatcherData error!");
			delete io_watcher;
			return(false);
		}
		pData->iFd = pConn->iFd;
		pData->ulSeq = pConn->ulSeq;
		pData->pManager = this;
		pConn->pIoWatcher = io_watcher;
		io_watcher->data = static_cast<void*>(pData);

		AddEvent(EV_READ,io_watcher,IoCallback,pData->iFd);
	}
	else
	{
		RefreshEvent(pConn->pIoWatcher->events | EV_READ,pConn->pIoWatcher);
	}
    return(true);
}

bool Manager::AddIoWriteEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s(fd %d)", __FUNCTION__, pConn->iFd);
    ev_io* io_watcher = nullptr;
	if (nullptr == pConn->pIoWatcher)
	{
		io_watcher = new ev_io();
		if (io_watcher == nullptr)
		{
			LOG4_ERROR("new io_watcher error!");
			return(false);
		}
		tagManagerIoWatcherData* pData = new tagManagerIoWatcherData();
		if (pData == nullptr)
		{
			LOG4_ERROR("new tagIoWatcherData error!");
			delete io_watcher;
			return(false);
		}
		pData->iFd = pConn->iFd;
		pData->ulSeq = pConn->ulSeq;
		pData->pManager = this;
		pConn->pIoWatcher = io_watcher;
		io_watcher->data = static_cast<void*>(pData);

		AddEvent(EV_WRITE,io_watcher,IoCallback,pData->iFd);
	}
	else
	{
		RefreshEvent(pConn->pIoWatcher->events | EV_WRITE,pConn->pIoWatcher);
	}
    return(true);
}

bool Manager::RemoveIoWriteEvent(tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s", __FUNCTION__);
	if (pConn->pIoWatcher)
	{
		if (pConn->pIoWatcher->events & EV_WRITE)
		{
			RefreshEvent(pConn->pIoWatcher->events & (~EV_WRITE),pConn->pIoWatcher);
		}
	}
    return(true);
}

bool Manager::AddIoTimeout(int iFd, uint32 ulSeq, ev_tstamp dTimeout)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_timer* timeout_watcher = new ev_timer();
    if (timeout_watcher == nullptr)
    {
        LOG4_ERROR("new timeout_watcher error!");
        return(false);
    }
    tagManagerIoWatcherData* pData = new tagManagerIoWatcherData();
    if (pData == nullptr)
    {
        LOG4_ERROR("new tagIoWatcherData error!");
        delete timeout_watcher;
        return(false);
    }
    pData->iFd = iFd;
    pData->ulSeq = ulSeq;
    pData->pManager = this;
    timeout_watcher->data = static_cast<void*>(pData);
    AddEvent(dTimeout,timeout_watcher, IoTimeoutCallback);
    return(true);
}

bool Manager::AddClientConnFrequencyTimeout(in_addr_t iAddr, ev_tstamp dTimeout)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ev_timer* timeout_watcher = new ev_timer();
    if (timeout_watcher == nullptr)
    {
        LOG4_ERROR("new timeout_watcher error!");
        return(false);
    }
    tagClientConnWatcherData* pData = new tagClientConnWatcherData();
    if (pData == nullptr)
    {
        LOG4_ERROR("new tagClientConnWatcherData error!");
        delete timeout_watcher;
        return(false);
    }
    pData->pManager = this;
    pData->iAddr = iAddr;
    timeout_watcher->data = static_cast<void*>(pData);
    AddEvent(dTimeout,timeout_watcher,ClientConnFrequencyTimeoutCallback);
    return(true);
}

tagConnectionAttr* Manager::CreateAcceptFdAttr(int iFd, uint32 ulSeq)
{
	SetSocketAttr(iFd);
	x_sock_set_block(iFd, 0);
	tagConnectionAttr* pConn = CreateFdAttr(iFd, ulSeq);
	if (pConn)
	{
		if(!AddIoTimeout(iFd, ulSeq))     // 为了防止大量连接攻击，初始化连接只有一秒即超时，在正常发送第一个数据包之后才采用正常配置的网络IO超时检查
		{
			DestroyConnect(m_mapFdAttr.find(iFd));
			return(nullptr);
		}
		if (!AddIoReadEvent(pConn))
		{
			DestroyConnect(m_mapFdAttr.find(iFd));
			return(nullptr);
		}
		return(pConn);
	}
	else    // 没有足够资源分配给新连接，直接close掉
	{
		LOG4_ERROR("%s() failed to create ptagConnectionAttr for %d",__FUNCTION__,iFd);
		close(iFd);
		return(nullptr);
	}
}

tagConnectionAttr* Manager::CreateReadFdAttr(int iFd, uint32 ulSeq)
{
	tagConnectionAttr* ptagConnectionAttr = CreateFdAttr(iFd, ulSeq);
	if (!ptagConnectionAttr)
	{
		LOG4_ERROR("%s() failed to create ptagConnectionAttr for %d",__FUNCTION__,iFd);
		return nullptr;
	}
	if (!AddIoReadEvent(ptagConnectionAttr))
	{
		LOG4_ERROR("%s() failed to AddIoReadEvent(ptagConnectionAttr) for %d",__FUNCTION__,iFd);
		delete ptagConnectionAttr;
		return nullptr;
	}
	return ptagConnectionAttr;
}



tagConnectionAttr* Manager::CreateFdAttr(int iFd, uint32 ulSeq)
{
    LOG4_TRACE("%s(iFd %d, seq %u)", __FUNCTION__, iFd, ulSeq);
    auto fd_attr_iter = m_mapFdAttr.find(iFd);
    if (fd_attr_iter == m_mapFdAttr.end())
    {
        auto pConnAttr = std::make_unique<tagConnectionAttr>();
        pConnAttr->iFd = iFd;
        pConnAttr->pRecvBuff = std::make_unique<util::CBuffer>();
        pConnAttr->pSendBuff = std::make_unique<util::CBuffer>();
        pConnAttr->pWaitForSendBuff = std::make_unique<util::CBuffer>();
        pConnAttr->dActiveTime = ev_now(m_loop);
        pConnAttr->ulSeq = ulSeq;
        tagConnectionAttr* pRaw = pConnAttr.get();
        m_mapFdAttr.insert(std::make_pair(iFd, std::move(pConnAttr)));
        return(pRaw);
    }
    else
    {
        LOG4_ERROR("fd %d is exist!", iFd);
        return(nullptr);
    }
}

bool Manager::DestroyConnect(std::unordered_map<int32, std::unique_ptr<tagConnectionAttr>>::iterator iter)
{
    if (iter == m_mapFdAttr.end())
    {
        return(false);
    }
    LOG4_TRACE("%s() iter->second->pIoWatcher = 0x%p, fd %d, data 0x%p", __FUNCTION__,
                    iter->second->pIoWatcher, iter->second->pIoWatcher->fd, iter->second->pIoWatcher->data);
    auto center_iter = m_mapCenterMsgShell.find(iter->second->strIdentify);
    if (center_iter != m_mapCenterMsgShell.end())
    {
        if (!m_strRaftLeaderCenterKey.empty() && iter->second->strIdentify == m_strRaftLeaderCenterKey)
        {
            m_strRaftLeaderCenterKey.clear();
            LOG4_INFO("%s lost connection to cached raft leader %s, fan-out all centers", __FUNCTION__, iter->second->strIdentify.c_str());
        }
        center_iter->second.iFd = 0;
        center_iter->second.ulSeq = 0;
    }
    DelEvent(iter->second->pIoWatcher,(tagManagerIoWatcherData*)iter->second->pIoWatcher->data);
    close(iter->first);
    m_mapFdAttr.erase(iter);
    return(true);
}

std::pair<int, int> Manager::GetMinLoadWorkerDataFd()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    int iMinLoadWorkerFd = 0;
    int iMinLoad = -1;
    std::pair<int, int> worker_pid_fd;
    for (auto iter = m_mapWorker.begin(); iter != m_mapWorker.end(); ++iter)
    {
		if (iter == m_mapWorker.begin())
		{
		   iMinLoadWorkerFd = iter->second.iDataFd;
		   iMinLoad = iter->second.iLoad;
		   worker_pid_fd = std::make_pair(iter->first, iMinLoadWorkerFd);//pid=>iFd
		}
		else if (iter->second.iLoad < iMinLoad)
		{
		   iMinLoadWorkerFd = iter->second.iDataFd;
		   iMinLoad = iter->second.iLoad;
		   worker_pid_fd = std::make_pair(iter->first, iMinLoadWorkerFd);
		}
    }
    return(worker_pid_fd);
}

void Manager::SetWorkerLoad(int iPid, util::CJsonObject& oJsonLoad)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = m_mapWorker.find(iPid);
    if (iter != m_mapWorker.end())
    {
        oJsonLoad.Get("load", iter->second.iLoad);
        oJsonLoad.Get("connect", iter->second.iConnect);
        oJsonLoad.Get("recv_num", iter->second.iRecvNum);
        oJsonLoad.Get("recv_byte", iter->second.iRecvByte);
        oJsonLoad.Get("send_num", iter->second.iSendNum);
        oJsonLoad.Get("send_byte", iter->second.iSendByte);
        oJsonLoad.Get("client", iter->second.iClientNum);
        iter->second.dBeatTime = ev_now(m_loop);
    }
}

void Manager::AddWorkerLoad(int iPid, int iLoad)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    auto iter = m_mapWorker.find(iPid);
    if (iter != m_mapWorker.end())
    {
        iter->second.iLoad += iLoad;
    }
}

bool Manager::CheckWorker()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    if (m_iRefreshInterval <= 0)
    {
        return(true);
    }
    for (auto worker_iter:m_mapWorker)
    {
        LOG4_TRACE("now %lf, worker's dBeatTime %lf, worker_beat %d",ev_now(m_loop), worker_iter.second.dBeatTime,m_iWorkerBeat);
        if ((ev_now(m_loop) - worker_iter.second.dBeatTime) > m_iWorkerBeat)
        {
            LOG4_INFO( "worker_%d pid %d is unresponsive, terminate it.", worker_iter.second.iWorkerIndex, worker_iter.first);
            kill(worker_iter.first, SIGKILL);
//            RestartWorker(worker_iter->first);
        }
    }
    return(true);
}

bool Manager::RestartWorkers()
{
    LOG4_TRACE("%s()", __FUNCTION__);
    for (const auto& worker_iter:m_mapWorker)
    {
        LOG4_TRACE("RestartWorkers:now %lf, worker's dBeatTime %lf, worker_beat %d",ev_now(m_loop), worker_iter.second.dBeatTime, m_iWorkerBeat);
        {
            LOG4_INFO( "terminate worker_%d pid %d",worker_iter.second.iWorkerIndex, worker_iter.first);
            kill(worker_iter.first, SIGKILL);
        }
    }
    return(true);
}

void Manager::RefreshServer()
{
	LOG4_TRACE("%s()", __FUNCTION__);
	bool boChanged = false;
    if (LoadConf(boChanged))
    {
    	if (boChanged)
    	{
   		    if (m_oLastConf["custom"].ToString() != m_oCustomConf.ToString())
			{
				std::string strCustom = m_oCurrentConf["custom"].ToString();
				LOG4_INFO("update custom:(%s)",strCustom.c_str());
				SendToWorker(CMD_REQ_SET_NODE_CUSTOM_CONFIG, GetSequence(),strCustom);
			}
			if (m_oLastConf("log_level") != m_oCurrentConf("log_level"))
			{
				LOG4_TRACE("update log_level:(%s)",m_oCurrentConf("log_level").c_str());
				ResetLogLevel(m_iLogLevel);
				LogLevel oLogLevel;
				oLogLevel.set_log_level(m_iLogLevel);
				SendToWorker(CMD_REQ_SET_LOG_LEVEL, GetSequence(),oLogLevel.SerializeAsString());
			}

    	}
    }
    else
    {
        LOG4_WARN("LoadConf() error, please check the config file.");
    }

}

/**
 * @brief 上报节点状态信息
 * @return 上报是否成功
 * @note 节点状态信息结构如：
 * {
 *     "node_type":"ACCESS",
 *     "node_ip":"192.168.11.12",
 *     "node_port":9988,
 *     "access_ip":"120.234.2.106",
 *     "access_port":10001,
 *     "worker_num":10,
 *     "active_time":16879561651.06,
 *     "node":{
 *         "load":1885792, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":495870
 *     },
 *     "worker":
 *     [
 *          {"load":655666, "connect":495873, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}},
 *          {"load":655235, "connect":485872, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}},
 *          {"load":585696, "connect":415379, "recv_num":98755266, "recv_byte":98856648832, "send_num":154846322, "send_byte":648469320222,"client":195870}}
 *     ]
 * }
 */
bool Manager::ReportToCenter(bool boRegister)
{
	if (m_mapCenterMsgShell.size() > 0)
	{
		int iLoad = 0;
		int iConnect = 0;
		int iRecvNum = 0;
		int iRecvByte = 0;
		int iSendNum = 0;
		int iSendByte = 0;
		int iClientNum = 0;
		NodeReport oNodeReport;
		MsgHead oMsgHead;
		MsgBody oMsgBody;
		oNodeReport.set_node_type(m_strNodeType);
		oNodeReport.set_node_id(m_uiNodeId);
		oNodeReport.set_node_ip(m_strNodeType);
		oNodeReport.set_node_ip(m_strHostForServer);
		oNodeReport.set_node_port(m_iPortForServer);
		if (m_strGateway.size() > 0)
		{
			oNodeReport.set_access_ip(m_strGateway);
		}
		else
		{
			oNodeReport.set_access_ip(m_strHostForClient);
		}
		
		if (m_iGatewayPort > 0)
		{
			oNodeReport.set_access_port(m_iGatewayPort);
		}
		else
		{
			oNodeReport.set_access_port(m_iPortForClient);
		}
		oNodeReport.set_worker_num(m_mapWorker.size());
		oNodeReport.set_active_time(ev_now(m_loop));

		for (const auto & worker_iter:m_mapWorker)
		{
			const tagWorkerAttr& oTagWorkerAttr = worker_iter.second;
			iLoad += oTagWorkerAttr.iLoad;
			iConnect += oTagWorkerAttr.iConnect;
			iRecvNum += oTagWorkerAttr.iRecvNum;
			iRecvByte += oTagWorkerAttr.iRecvByte;
			iSendNum += oTagWorkerAttr.iSendNum;
			iSendByte += oTagWorkerAttr.iSendByte;
			iClientNum += oTagWorkerAttr.iClientNum;

			auto oWorker = oNodeReport.add_workers();
			oWorker->set_load(oTagWorkerAttr.iLoad);
			oWorker->set_connect(oTagWorkerAttr.iConnect);
			oWorker->set_recv_num(oTagWorkerAttr.iRecvNum);
			oWorker->set_recv_byte(oTagWorkerAttr.iRecvByte);
			oWorker->set_send_num(oTagWorkerAttr.iSendNum);
			oWorker->set_send_byte(oTagWorkerAttr.iSendByte);
			oWorker->set_client(oTagWorkerAttr.iClientNum);
		}
		auto oNode = oNodeReport.mutable_node();
		oNode->set_load(iLoad);
		oNode->set_connect(iConnect);
		oNode->set_recv_num(iRecvNum);
		oNode->set_recv_byte(iRecvByte);
		oNode->set_send_num(iSendNum);
		oNode->set_send_byte(iSendByte);
		oNode->set_client(iClientNum);

		oMsgBody.set_body(oNodeReport.SerializeAsString());
		oMsgHead.set_cmd(CMD_REQ_NODE_STATUS_REPORT);
		oMsgHead.set_seq(GetSequence());
		oMsgHead.set_msgbody_len(oMsgBody.ByteSize());
		LOG4_TRACE("%s():%s", __FUNCTION__, oNodeReport.DebugString().c_str());

		auto sendToOneCenter = [&](const std::string& strCenterKey, const tagMsgShell& stShell) {
			if (stShell.iFd == 0)
			{
				oMsgHead.set_cmd(CMD_REQ_NODE_REGISTER);
				LOG4_TRACE("%s() cmd %d -> %s", __FUNCTION__, oMsgHead.cmd(), strCenterKey.c_str());
				AutoSend(strCenterKey, oMsgHead, oMsgBody);
			}
			else
			{
				if (boRegister)
				{
					oMsgHead.set_cmd(CMD_REQ_NODE_REGISTER);
					LOG4_TRACE("%s() cmd %d -> %s", __FUNCTION__, oMsgHead.cmd(), strCenterKey.c_str());
					SendTo(stShell, oMsgHead, oMsgBody);
				}
				else
				{
					oMsgHead.set_cmd(CMD_REQ_NODE_STATUS_REPORT);
					LOG4_TRACE("%s() cmd %d -> %s", __FUNCTION__, oMsgHead.cmd(), strCenterKey.c_str());
					SendTo(stShell, oMsgHead, oMsgBody);
				}
			}
		};

		if (m_strRaftLeaderCenterKey.empty())
		{
			for (const auto& center_iter : m_mapCenterMsgShell)
			{
				sendToOneCenter(center_iter.first, center_iter.second);
			}
		}
		else
		{
			auto leader_it = m_mapCenterMsgShell.find(m_strRaftLeaderCenterKey);
			if (leader_it != m_mapCenterMsgShell.end())
			{
				sendToOneCenter(leader_it->first, leader_it->second);
			}
			else
			{
				LOG4_WARN("%s cached leader %s not in center map, clear and fan-out", __FUNCTION__, m_strRaftLeaderCenterKey.c_str());
				m_strRaftLeaderCenterKey.clear();
				for (const auto& center_iter : m_mapCenterMsgShell)
				{
					sendToOneCenter(center_iter.first, center_iter.second);
				}
			}
		}
	}
	else
	{
		LOG4_TRACE("%s()", __FUNCTION__);
	}
    return(true);
}

bool Manager::SendToWorker(uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendToWorker(oOutMsgHead, oOutMsgBody);
}

bool Manager::SendToWorker(const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    for (const auto& worker_iter :m_mapWorker)
    {
        auto worker_conn_iter = m_mapFdAttr.find(worker_iter.second.iControlFd);
        if (worker_conn_iter != m_mapFdAttr.end())
        {
        	LOG4_TRACE("send cmd %d seq %u to worker %d", oMsgHead.cmd(), oMsgHead.seq(), worker_iter.second.iWorkerIndex);
        	if (SendTo(worker_conn_iter->second.get(),oMsgHead,oMsgBody))
			{
        		LOG4_TRACE("send to worker %d success, cmd %d seq %u",worker_iter.second.iWorkerIndex, oMsgHead.cmd(), oMsgHead.seq());
			}
        }
    }
    return(true);
}

bool Manager::SendTo(tagConnectionAttr* pConn,uint32 cmd,uint32 seq,const std::string & strBody)
{
	MsgHead oOutMsgHead;
	MsgBody oOutMsgBody;
	oOutMsgBody.set_body(strBody);
	oOutMsgHead.set_cmd(cmd);
	oOutMsgHead.set_seq(seq);
	oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
	return SendTo(pConn,oOutMsgHead,oOutMsgBody);
}


bool Manager::SendTo(tagConnectionAttr* pConn,const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
	pConn->pSendBuff->Write(oMsgHead.SerializeAsString().c_str(), oMsgHead.ByteSize());
	pConn->pSendBuff->Write(oMsgBody.SerializeAsString().c_str(), oMsgBody.ByteSize());
	return SendTo(pConn);
}

bool Manager::SendTo(tagConnectionAttr* pConn)
{
	util::CBuffer* pSendBuff = pConn->pSendBuff.get();
	int iWriteLen = 0;
	int iErrno = 0;
	int iNeedWriteLen = pSendBuff->ReadableBytes();
	iWriteLen = pSendBuff->WriteFD(pConn->iFd, iErrno);
	if (iWriteLen < 0)
	{
		if (EAGAIN != iErrno && EINTR != iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
		{
			LOG4_ERROR("send to fd %d error %d: %s",pConn->iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
			return(false);
		}
		else if (EAGAIN == iErrno)  // 内容未写完，添加或保持监听fd写事件
		{
			AddIoWriteEvent(pConn);
		}
	}
	else if (iWriteLen > 0)
	{
		if (iWriteLen == iNeedWriteLen)  // 已无内容可写，取消监听fd写事件
		{
			RemoveIoWriteEvent(pConn);
		}
		else    // 内容未写完，添加或保持监听fd写事件
		{
			AddIoWriteEvent(pConn);
		}
	}
	pConn->pSendBuff->Compact(32784);
	return true;
}

bool Manager::SendToWorkerWithMod(uint64 uiModFactor,const MsgHead& oMsgHead, const MsgBody& oMsgBody)
{
    int target_identify = uiModFactor % m_mapWorker.size();
    int i = 0;
    for (const auto& worker_iter : m_mapWorker)
    {
        if (i++ == target_identify)
        {
            auto worker_conn_iter = m_mapFdAttr.find(worker_iter.second.iControlFd);
            if (worker_conn_iter != m_mapFdAttr.end())
            {
            	if (SendTo(worker_conn_iter->second.get(),oMsgHead,oMsgBody))
				{
					LOG4_TRACE("send to worker %d success, cmd %d seq %u",worker_iter.second.iWorkerIndex, oMsgHead.cmd(), oMsgHead.seq());
				}
            }
            return(true);
        }
    }
    return(false);
}

bool Manager::RegisterCallback(Session* pSession)
{
    LOG4_TRACE("%s(Session* 0x%p, lifetime %lf)", __FUNCTION__, &pSession, pSession->GetTimeout());
    if (pSession == nullptr)
    {
        return(false);
    }
    if (pSession->IsRegistered())  // 已注册过，不必重复注册，不过认为本次注册成功
    {
        LOG4_WARN("Session(session_id %s) registered already.", pSession->GetSessionId().c_str());
        return(true);
    }
    pSession->SetRegistered();
    pSession->SetActiveTime(ev_now(m_loop));

    std::pair<std::unordered_map<std::string, Session*>::iterator, bool> ret;
    std::unordered_map<std::string, std::unordered_map<std::string, Session*> >::iterator name_iter = m_mapCallbackSession.find(pSession->GetSessionClass());
    if (name_iter == m_mapCallbackSession.end())
    {
        std::unordered_map<std::string, Session*> mapSession;
        ret = mapSession.insert(std::pair<std::string, Session*>(pSession->GetSessionId(), pSession));
        if(!ret.second)
        {
            LOG4_WARN("insert Session(session_id %s).SessionClass(%s) register failed.", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        else
        {
            LOG4_TRACE("inserted Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        m_mapCallbackSession.insert(std::pair<std::string, std::unordered_map<std::string, Session*> >(pSession->GetSessionClass(), mapSession));
    }
    else
    {
        LOG4_TRACE("try insert Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                        pSession->GetSessionClass().c_str());
        ret = name_iter->second.insert(std::pair<std::string, Session*>(pSession->GetSessionId(), pSession));
        if(!ret.second)
        {
            LOG4_WARN("insert Session(session_id %s).SessionClass(%s) register failed.", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
        else
        {
            LOG4_TRACE("inserted Session(session_id %s).SessionClass(%s)", pSession->GetSessionId().c_str(),
                            pSession->GetSessionClass().c_str());
        }
    }
    if (ret.second)
    {
    	AddSession(pSession,pSession->GetTimeout(),SessionTimeoutCallback);
        LOG4_TRACE("Session(session_id %s) register successful.", pSession->GetSessionId().c_str());
    }
    else
    {
        LOG4_WARN("Session(session_id %s) register failed.", pSession->GetSessionId().c_str());
    }
    return(ret.second);
}

void Manager::DeleteCallback(Session* pSession)
{
    LOG4_TRACE("%s(Session* 0x%p)", __FUNCTION__, &pSession);
    if (pSession == nullptr)
    {
        return;
    }
    DelEvent(pSession->m_pTimeoutWatcher);
    auto name_iter = m_mapCallbackSession.find(pSession->GetSessionClass());
    if (name_iter != m_mapCallbackSession.end())
    {
        auto id_iter = name_iter->second.find(pSession->GetSessionId());
        if (id_iter != name_iter->second.end())
        {
            LOG4_TRACE("delete session(session_id %s)", pSession->GetSessionId().c_str());
            delete id_iter->second; id_iter->second = nullptr;
            name_iter->second.erase(id_iter);
        }
    }
}

bool Manager::SessionTimeout(Session* pSession, struct ev_timer* watcher)
{
    ev_tstamp after = pSession->GetActiveTime() - ev_now(m_loop) + pSession->GetTimeout();
    if (after > 0)    // 定时时间内被重新刷新过，重新设置定时器
    {
    	RefreshEvent(after,watcher);
        return(true);
    }
    else    // 会话已超时
    {
        //LOG4_TRACE("%s(session_name: %s,  session_id: %s)",__FUNCTION__, pSession->GetSessionClass().c_str(), pSession->GetSessionId().c_str());
        if (STATUS_CMD_RUNNING == pSession->Timeout())   // 定时器时间到，执行定时操作，session时间刷新
        {
        	RefreshEvent(pSession->GetTimeout(),watcher);
            return(true);
        }
        else
        {
            DeleteCallback(pSession);
            return(true);
        }
    }
}

bool Manager::DisposeDataFromWorker(const MsgHead& oInMsgHead, const MsgBody& oInMsgBody, tagConnectionAttr* pConn)
{
	tagMsgShell stMsgShell(pConn->iFd,pConn->ulSeq);
    LOG4_TRACE("%s(cmd %u, seq %u)", __FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
    uint32 uiCmd = gc_uiCmdBit & oInMsgHead.cmd();
	auto cmd_iter = m_mapSysCmd.find(uiCmd);
	if (cmd_iter != m_mapSysCmd.end())
	{
		cmd_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
	}
	else
	{
		if (CMD_REQ_UPDATE_WORKER_LOAD == oInMsgHead.cmd())    // 新请求
		{
			LOG4_TRACE("%s CMD_REQ_UPDATE_WORKER_LOAD", __FUNCTION__);
			auto iter = m_mapWorkerFdPid.find(stMsgShell.iFd);
			if (iter != m_mapWorkerFdPid.end())
			{
				util::CJsonObject oJsonLoad;
				oJsonLoad.Parse(oInMsgBody.body());
				SetWorkerLoad(iter->second, oJsonLoad);
			}
		}
		else if (CMD_REQ_NODE_STOP == oInMsgHead.cmd())
		{
			LOG4_TRACE("%s todo CMD_REQ_NODE_STOP", __FUNCTION__);//退出节点
		}
		else if (CMD_REQ_NODE_RESTART_WORKERS == oInMsgHead.cmd())
		{
			LOG4_TRACE("%s todo CMD_REQ_NODE_RESTART_WORKERS", __FUNCTION__);//重启工作者
		}
		else
		{
			LOG4_WARN("unknow cmd %d from worker!", oInMsgHead.cmd());
		}
	}
    return(true);
}

bool Manager::DisposeDataAndTransferFd(const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s(cmd %u, seq %u)", __FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
    if (oInMsgHead.cmd() == CMD_REQ_NODE_REGISTER ||
        oInMsgHead.cmd() == CMD_RSP_NODE_REGISTER ||
        oInMsgHead.cmd() == CMD_REQ_NODE_STATUS_REPORT ||
        oInMsgHead.cmd() == CMD_RSP_NODE_STATUS_REPORT)
    {
        // 某些连接在identify尚未建立时会先收到Center链路消息，按Center通路处理，避免误判为transfer-fd。
        return DisposeDataFromCenter(oInMsgHead, oInMsgBody, pConn);
    }

    ConnectWorker oConnWorker;
    OrdinaryResponse oRes;
    LOG4_TRACE("oInMsgHead.cmd() = %u, seq() = %u", oInMsgHead.cmd(), oInMsgHead.seq());
    if (oInMsgHead.cmd() == CMD_REQ_CONNECT_TO_WORKER)
    {
        if (oConnWorker.ParseFromString(oInMsgBody.body()))
        {
        	bool boFound = false;
            for (const auto& worker_iter :m_mapWorker)
            {
                if (oConnWorker.worker_index() == worker_iter.second.iWorkerIndex)
                {
                    oRes.set_err_no(ERR_OK);
                    oRes.set_err_msg("OK");
                    //目前是单向传送文件描述符
                    char szIp[16] = {0};
                    strncpy(szIp, "0.0.0.0", 16);   // 内网其他Server的IP不重要
                    int iErrno = send_fd_with_attr(worker_iter.second.iDataFd, pConn->iFd, szIp, 16, util::CODEC_PB_INTERNAL);
                    if (iErrno != 0)
                    {
                        LOG4_ERROR("send_fd_with_attr error %d: %s!",iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
                    }
                    boFound = true;
                    break;
                }
            }
            if (!boFound)
            {
                oRes.set_err_no(ERR_NO_SUCH_WORKER_INDEX);
                oRes.set_err_msg("no such worker index!");
            }
        }
        else
        {
            LOG4_ERROR("oConnWorker.ParseFromString() error!");
            oRes.set_err_no(ERR_PARASE_PROTOBUF);
            oRes.set_err_msg("oConnWorker.ParseFromString() error!");
        }
    }
    else
    {
        oRes.set_err_no(ERR_UNKNOWN_CMD);
        oRes.set_err_msg("unknow cmd!");
        LOG4_TRACE("unknow cmd %d!", oInMsgHead.cmd());
    }
    SendTo(pConn,oInMsgHead.cmd() + 1,oInMsgHead.seq(),oRes.SerializeAsString());
    return(false);
}

void Manager::UpdateRaftLeaderHintFromNodeReportRsp(const NodeReportRsp& oNodeReportRsp)
{
    const uint32_t err = oNodeReportRsp.errcode();
    if (err == 2u)
    {
        m_strRaftLeaderCenterKey.clear();
        LOG4_TRACE("%s err=2 (no stable raft leader), fan-out all centers", __FUNCTION__);
        return;
    }
    const std::string& leader = oNodeReportRsp.current_leader_identify();
    if (!leader.empty())
    {
        if (m_mapCenterMsgShell.find(leader) != m_mapCenterMsgShell.end())
        {
            if (m_strRaftLeaderCenterKey != leader)
            {
                LOG4_INFO("%s cache raft leader center %s (err=%u raft_term=%llu)", __FUNCTION__, leader.c_str(), err,
                          static_cast<unsigned long long>(oNodeReportRsp.raft_term()));
            }
            m_strRaftLeaderCenterKey = leader;
        }
        else
        {
            m_strRaftLeaderCenterKey.clear();
            LOG4_TRACE("%s leader %s not in local center config, fan-out all", __FUNCTION__, leader.c_str());
        }
    }
    else if (err == 0u)
    {
        m_strRaftLeaderCenterKey.clear();
        LOG4_TRACE("%s err=0 without leader hint, fan-out all", __FUNCTION__);
    }
}

bool Manager::DisposeDataFromCenter(const MsgHead& oInMsgHead,const MsgBody& oInMsgBody,tagConnectionAttr* pConn)
{
    LOG4_TRACE("%s(cmd %u, seq %u)", __FUNCTION__, oInMsgHead.cmd(), oInMsgHead.seq());
	tagMsgShell stMsgShell(pConn->iFd,pConn->ulSeq);
    int iErrno = 0;
    // 判断是否为新请求，若是则直接转发给Worker，并回复Center已收到请求
    if (gc_uiCmdReq & oInMsgHead.cmd())
    {
		uint32 uiCmd = gc_uiCmdBit & oInMsgHead.cmd();
		auto cmd_iter = m_mapSysCmd.find(uiCmd);
		if (cmd_iter != m_mapSysCmd.end())
		{
			cmd_iter->second->AnyMessage(stMsgShell, oInMsgHead, oInMsgBody);
		}
		else
		{
			if (CMD_REQ_NODE_STOP == oInMsgHead.cmd())
			{
				LOG4_TRACE("%s CMD_REQ_NODE_STOP", __FUNCTION__);//退出节点
				return(true);
			}
			else if (CMD_REQ_NODE_RESTART_WORKERS == oInMsgHead.cmd())
			{
				LOG4_TRACE("%s CMD_REQ_NODE_RESTART_WORKERS", __FUNCTION__);//重启工作者
				return(true);
			}
			else if (CMD_REQ_SET_NODE_CUSTOM_CONFIG == oInMsgHead.cmd())
			{
				OrdinaryResponse oRes;
				ConfigInfo oConfigInfo;
				if (!oConfigInfo.ParseFromString(oInMsgBody.body()))
				{
					oRes.set_err_no(1);
					oRes.set_err_msg("invalid ConfigInfo");
				}
				else if (!GetCustomConfigVersionData().SetCustomConfig(oConfigInfo.file_content()))
				{
					oRes.set_err_no(2);
					oRes.set_err_msg("write custom shm failed");
				}
				else
				{
					oRes.set_err_no(0);
					oRes.set_err_msg("OK");
					LOG4_INFO("%s() custom mirror updated, version=%llu, bytes=%zu",
							__FUNCTION__,
							static_cast<unsigned long long>(GetCustomConfigVersionData().GetCustomVersion()),
							oConfigInfo.file_content().size());
				}
				SendTo(stMsgShell, oInMsgHead.cmd() + 1, oInMsgHead.seq(), oRes.SerializeAsString());
				return true;
			}
			SendToWorker(oInMsgHead, oInMsgBody);
			OrdinaryResponse oRes;
			oRes.set_err_no(0);
			oRes.set_err_msg("OK");
			SendTo(stMsgShell, oInMsgHead.cmd() + 1, oInMsgHead.seq(),oRes.SerializeAsString());
		}
    }
    else    // 回调
    {
        if (CMD_RSP_NODE_REGISTER == oInMsgHead.cmd()||CMD_RSP_NODE_STATUS_REPORT == oInMsgHead.cmd()) //Manager这层只有向center注册会收到回调，上报状态不收回调或者收到回调不必处理
        {
        	NodeReportRsp oNodeReportRsp;
        	if (oNodeReportRsp.ParseFromString(oInMsgBody.body()))
        	{
        		UpdateRaftLeaderHintFromNodeReportRsp(oNodeReportRsp);
        		if (oNodeReportRsp.errcode() == 0)
        		{
                    // 1) 路由镜像：由 Manager 负责落地到共享内存（只有当订阅快照内容变化才写）
                    //    Worker 只轮询版本并消费，不依赖此处主动通知。
                    {
                        const NodeNotice& oSnapshot = oNodeReportRsp.subscribed_route_snapshot();
                        // 按 Center 现有逻辑：仅当 node_arry_reg_size() > 0 时才会附带路由快照
                        if (oSnapshot.node_arry_reg_size() > 0)
                        {
                            NodeNotice oldSnapshot;
                            bool hasOld = GetRouteNoticeVersionData().GetNodeNotice(oldSnapshot);
                            std::string newSer = oSnapshot.SerializeAsString();
                            std::string oldSer;
                            if (hasOld)
                            {
                                oldSer = oldSnapshot.SerializeAsString();
                            }

                            if (!hasOld || oldSer != newSer)
                            {
                                const size_t bytes = newSer.size();
                                if (GetRouteNoticeVersionData().SetNodeNotice(oSnapshot))
                                {
                                    LOG4_INFO("%s() route mirror updated, version=%llu, bytes=%zu",
                                              __FUNCTION__,
                                              static_cast<unsigned long long>(GetRouteNoticeVersionData().GetNodeNoticeVersion()),
                                              bytes);
                                }
                                else
                                {
                                    const auto bs = oSnapshot.ByteSize();
                                    LOG4_ERROR("%s() route mirror shm write failed, bytes=%d, reg_size=%d, exit_size=%d",
                                                __FUNCTION__,
                                                bs,
                                                oSnapshot.node_arry_reg_size(),
                                                oSnapshot.node_arry_exit_size());
                                }
                            }
                            else
                            {
                                LOG4_TRACE("%s() route mirror unchanged, skip write. version=%llu, bytes=%zu",
                                           __FUNCTION__,
                                           static_cast<unsigned long long>(GetRouteNoticeVersionData().GetNodeNoticeVersion()),
                                           newSer.size());
                            }
                        }
                    }

                    // 2) node_id 更新：仅 node_id 变化时主动通知 Worker
                    if (m_uiNodeId != oNodeReportRsp.node_id())
                    {
                        m_uiNodeId = oNodeReportRsp.node_id();
                        LOG4_INFO("SetNodeId node_id(%u)!", oNodeReportRsp.node_id());

                        MsgHead oMsgHead;
                        oMsgHead.set_cmd(CMD_REQ_REFRESH_NODE_ID);
                        oMsgHead.set_seq(oInMsgHead.seq());
                        oMsgHead.set_msgbody_len(oInMsgHead.msgbody_len());
                        SendToWorker(oMsgHead, oInMsgBody);
                    }
        		}
        		else
        		{
        			LOG4_WARN("NodeReport/Register rsp from center error, errcode %u!", oNodeReportRsp.errcode());
        		}
        	}
        	else
        	{
        		LOG4_WARN("oNodeReportRsp parse error!");
        	}
        }
        else if (CMD_RSP_CONNECT_TO_WORKER == oInMsgHead.cmd()) // 连接center时的回调
        {
            MsgHead oOutMsgHead;
            MsgBody oOutMsgBody;
            TargetWorker oTargetWorker;
            oTargetWorker.set_err_no(0);
            oTargetWorker.set_worker_identify(GetNodeIdentify());
            oTargetWorker.set_node_type(GetNodeType());
            oTargetWorker.set_err_msg("OK");
            oOutMsgBody.set_body(oTargetWorker.SerializeAsString());
            oOutMsgHead.set_cmd(CMD_REQ_TELL_WORKER);//CMD_REQ_TELL_WORKER之后，连同之前的数据，开始发送数据了
            oOutMsgHead.set_seq(GetSequence());
            oOutMsgHead.set_msgbody_len(oOutMsgBody.ByteSize());
            pConn->pSendBuff->Write(oOutMsgHead.SerializeAsString().c_str(), oOutMsgHead.ByteSize());
            pConn->pSendBuff->Write(oOutMsgBody.SerializeAsString().c_str(), oOutMsgBody.ByteSize());
            pConn->pSendBuff->Write(pConn->pWaitForSendBuff.get(), pConn->pWaitForSendBuff->ReadableBytes());
            if (!SendTo(pConn))
            {
            	LOG4_ERROR("send to fd %d error %d: %s",stMsgShell.iFd, iErrno, strerror_r(iErrno, m_pErrBuff, gc_iErrBuffLen));
				return(false);
            }
        }
    }
    return(true);
}

} /* namespace net */
