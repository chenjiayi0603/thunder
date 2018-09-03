/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepWriteBackToRedis.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年3月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDATAPROXY_STEPWRITEBACKTOREDIS_HPP_
#define SRC_CMDDATAPROXY_STEPWRITEBACKTOREDIS_HPP_

#include "RedisStorageStep.hpp"
#include "StepSetTtl.hpp"

namespace net
{

class StepWriteBackToRedis: public RedisStorageStep
{
public:
    StepWriteBackToRedis(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate,
                    SessionRedisNode* pNodeSession,
                    int iRelative = RELATIVE_TABLE,
                    const std::string& strKeyField = "",
                    const util::CJsonObject* pJoinField = NULL);
    virtual ~StepWriteBackToRedis();

    virtual E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "")
    {
        return(STATUS_CMD_COMPLETED);
    }

    virtual E_CMD_STATUS Emit(const DataMem::MemRsp& oRsp);

    virtual E_CMD_STATUS Callback(
                    const redisAsyncContext *c,
                    int status,
                    redisReply* pReply);

protected:
    bool MakeCmdWithJoin(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdWithDataSet(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdWithoutDataSet(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdForHashWithDataSet(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdForHashWithoutDataSet(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdForHashWithoutDataSetWithField(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);
    bool MakeCmdForHashWithoutDataSetWithoutField(const DataMem::MemRsp& oRsp, util::RedisCmd* pRedisCmd);

private:
    tagMsgShell m_stMsgShell;
    MsgHead m_oReqMsgHead;
    DataMem::MemOperate m_oMemOperate;
    int m_iRelative;
    std::string m_strKeyField;
    util::CJsonObject m_oJoinField;
    std::string m_strMasterNode;
    std::string m_strSlaveNode;

public:
    SessionRedisNode* m_pNodeSession;
    StepSetTtl* pStepSetTtl;
};

} /* namespace net */

#endif /* SRC_CMDDATAPROXY_STEPWRITEBACKTOREDIS_HPP_ */
