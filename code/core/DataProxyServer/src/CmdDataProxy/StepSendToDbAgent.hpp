/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepReadFromDb.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDATAPROXY_STEPSENDTODBAGENT_HPP_
#define SRC_CMDDATAPROXY_STEPSENDTODBAGENT_HPP_

#include "DbStorageStep.hpp"
#include "SessionSyncDbData.hpp"
#include "StepWriteBackToRedis.hpp"

namespace oss
{

class StepSendToDbAgent: public DbStorageStep
{
public:
    /**
     * @brief 向DbAgent发出数据库操作请求
     * @param stMsgShell 客户端连接通道
     * @param oInMsgHead 客户端请求消息头
     * @param oMemOperate 存储操作请求数据
     * @param pNodeSession redis节点session
     * @param iRelative  redis数据结构与表之间的关系
     * @param pTableField 数据库表字段（json数组）
     * @param strKeyField redis数据结构键字段
     * @param pJoinField  redis数据结构串联字段
     */
    StepSendToDbAgent(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate,
                    SessionRedisNode* pNodeSession = NULL,
                    int iRelative = RELATIVE_TABLE,
                    const loss::CJsonObject* pTableField = NULL,
                    const std::string& strKeyField = "",
                    const loss::CJsonObject* pJoinField = NULL);
    virtual ~StepSendToDbAgent();

    virtual E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");

    virtual E_CMD_STATUS Callback(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);

    virtual E_CMD_STATUS Timeout();

protected:
    void WriteBackToRedis(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const DataMem::MemRsp& oRsp);

private:
    tagMsgShell m_stMsgShell;
    MsgHead m_oReqMsgHead;
    DataMem::MemOperate m_oMemOperate;
    int m_iRelative;
    std::string m_strKeyField;
    loss::CJsonObject m_oTableField;
    loss::CJsonObject m_oJoinField;
    bool m_bFieldFilter;        ///< 是否需要筛选字段返回给请求方
    bool m_bNeedResponse;       ///< 是否需要响应请求方（如果是同时写redis和mysql，写redis已经给了响应，写mysql不再回响应）

public:
    SessionRedisNode* m_pNodeSession;
    StepWriteBackToRedis* pStepWriteBackToRedis;
};

} /* namespace oss */

#endif /* SRC_CMDDATAPROXY_STEPSENDTODBAGENT_HPP_ */
