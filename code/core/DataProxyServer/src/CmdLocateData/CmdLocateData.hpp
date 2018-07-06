/*******************************************************************************
 * Project:  DataProxyServer
 * @file     CmdLocateData.hpp
 * @brief    查询数据落在集群的哪个节点
 * @author   cjy
 * @date:    2016年4月18日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_
#define SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_

#include "cmd/Cmd.hpp"
#include "storage/StorageOperator.hpp"
#include "storage/dataproxy.pb.h"
#include "SessionRedisNode.hpp"
#include "StepDbDistribute.hpp"

#ifdef __cplusplus
extern "C" {
#endif
    oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace oss
{

/**
 * @brief 查询数据落在集群的哪个节点
 * @note
 */
class CmdLocateData: public Cmd
{
public:
    CmdLocateData();
    virtual ~CmdLocateData();

    virtual bool Init();

    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
protected:
    bool ReadDataProxyConf();
    bool ReadTableRelation();
    bool RedisOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const DataMem::MemOperate& oMemOperate);
    bool DbOnly(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
    bool RedisAndDb(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
    void Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                        int iErrno, const std::string& strErrMsg);

private:
    SessionRedisNode* m_pRedisNodeSession;
    loss::CJsonObject m_oJsonTableRelative;
    std::map<std::string, std::set<uint32> > m_mapFactorSection; //分段因子区间配置，key为因子类型

public:
    StepDbDistribute* pStepDbDistribute;
};

} /* namespace oss */

#endif /* SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_ */
