/*******************************************************************************
 * Project:  DataProxyServer
 * @file     StepDbDistribute.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月18日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDLOCATEDATA_STEPDBDISTRIBUTE_HPP_
#define SRC_CMDLOCATEDATA_STEPDBDISTRIBUTE_HPP_

#include "util/json/CJsonObject.hpp"
#include "step/Step.hpp"

namespace net
{

class StepDbDistribute: public Step
{
public:
    StepDbDistribute(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody,
                    const util::CJsonObject* pRedisNode = NULL);
    virtual ~StepDbDistribute();

    virtual E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrShow = "");

    virtual E_CMD_STATUS Callback(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);

    virtual E_CMD_STATUS Timeout();

protected:
    bool Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                    int iErrno, const std::string& strErrMsg);

private:
    util::CJsonObject m_oRedisNode;
};

} /* namespace net */

#endif /* SRC_CMDLOCATEDATA_STEPDBDISTRIBUTE_HPP_ */
