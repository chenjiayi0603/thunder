/*******************************************************************************
 * Project:  DataProxy
 * @file     CmdDataProxy.hpp
 * @brief    数据访问代理
 * @author   cjy
 * @date:    2016年12月8日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDDATAPROXY_CMDDATAPROXY_HPP_
#define SRC_CMDDATAPROXY_CMDDATAPROXY_HPP_

#include <dirent.h>
#include <map>
#include <set>
#include "cmd/Cmd.hpp"
#include "storage/StorageOperator.hpp"
#include "storage/dataproxy.pb.h"
#include "SessionRedisNode.hpp"
#include "SessionSyncDbData.hpp"
#include "StepReadFromRedis.hpp"
#include "StepSendToDbAgent.hpp"
#include "StepWriteToRedis.hpp"
#include "StepReadFromRedisForWrite.hpp"
#include "StepSyncToDb.hpp"

#ifdef __cplusplus
extern "C" {
#endif
    oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace oss
{

const int gc_iErrBuffSize = 256;

class CmdDataProxy: public oss::Cmd
{
public:
    CmdDataProxy();
    virtual ~CmdDataProxy();

    virtual bool Init();

    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);

protected:
    bool ReadDataProxyConf();
    bool ReadTableRelation();
    bool ScanSyncData();

    bool Preprocess(DataMem::MemOperate& oMemOperate);
    bool CheckRequest(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate);
    bool CheckRedisOperate(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate::RedisOperate& oRedisOperate);
    bool CheckDbOperate(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate::DbOperate& oDbOperate);
    //检查发送数据目的的合法性
    bool CheckDataSet(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate, const std::string& strRedisDataPurpose);
    bool CheckJoinField(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate, const std::string& strRedisDataPurpose);
    bool PrepareForWriteBothWithDataset(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    DataMem::MemOperate& oMemOperate, const std::string& strRedisDataPurpose);
    bool PrepareForWriteBothWithFieldJoin(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    DataMem::MemOperate& oMemOperate, const std::string& strRedisDataPurpose);

    bool RedisOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate);
    bool DbOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate);
    bool ReadEither(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate);
    bool WriteBoth(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    DataMem::MemOperate& oMemOperate);
    bool UpdateBothWithDataset(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    DataMem::MemOperate& oMemOperate);

    void Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    int iErrno, const std::string& strErrMsg);

private:
    bool GetNodeSession(int32 iDataType, int32 iSectionFactorType, uint64 uiFactor);

private:
    char* m_pErrBuff;
    SessionRedisNode* m_pRedisNodeSession;
    loss::CJsonObject m_oJsonTableRelative;
    std::map<std::string, std::set<uint32> > m_mapFactorSection; //分段因子区间配置，key为因子类型
    std::map<std::string, std::set<std::string> > m_mapTableFields; //表的组成字段，key为表名，value为字段名集合，用于查找请求的字段名是否存在
    uint32 m_ScanSyncDataTime;
    uint32 m_ScanSyncDataLastTime;
public:
    StepSendToDbAgent* pStepSendToDbAgent;
    StepReadFromRedis* pStepReadFromRedis;
    StepWriteToRedis* pStepWriteToRedis;
    StepReadFromRedisForWrite* pStepReadFromRedisForWrite;
    StepSyncToDb* pStepSyncToDb;
};

} /* namespace oss */

#endif /* SRC_CMDDATAPROXY_CMDDATAPROXY_HPP_ */
