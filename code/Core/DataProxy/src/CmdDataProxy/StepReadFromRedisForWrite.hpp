/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepReadFromRedisForWrite.hpp
 * @brief    读取redis中的数据，为下一步写入之用（适用于update dataset情形）
 * @author   cjy
 * @date:    2016年4月6日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDATAPROXY_STEPREADFROMREDISFORWRITE_HPP_
#define SRC_CMDDATAPROXY_STEPREADFROMREDISFORWRITE_HPP_

#include "util/json/CJsonObject.hpp"
#include "RedisStorageStep.hpp"
#include "StepSendToDbAgent.hpp"
#include "StepWriteToRedis.hpp"

namespace net
{

class StepReadFromRedisForWrite: public RedisStorageStep
{
public:
    StepReadFromRedisForWrite(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate,
                    SessionRedisNode* pNodeSession,
                    const util::CJsonObject& oTableFields,
                    const std::string& strKeyField = "");
    virtual ~StepReadFromRedisForWrite();

    virtual E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");

    virtual E_CMD_STATUS Callback(
                    const redisAsyncContext *c,
                    int status,
                    redisReply* pReply);
protected:
    E_CMD_STATUS ExecUpdate(bool bDbOnly = false);

private:
    tagMsgShell m_stMsgShell;
    MsgHead m_oReqMsgHead;
    DataMem::MemOperate m_oMemOperate;
    util::CJsonObject m_oTableFields;
    std::string m_strKeyField;
    std::string m_strMasterNode;
    std::string m_strSlaveNode;

public:
    SessionRedisNode* m_pRedisNodeSession;
    StepSendToDbAgent* pStepSendToDbAgent;
    StepWriteToRedis* pStepWriteToRedis;
};

} /* namespace net */

#endif /* SRC_CMDDATAPROXY_STEPREADFROMREDISFORWRITE_HPP_ */
