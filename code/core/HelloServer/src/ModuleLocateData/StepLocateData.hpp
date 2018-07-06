/*******************************************************************************
 * Project:  LoginServer
 * @file     StepLocateData.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_MODULELOCATEDATA_STEPLOCATEDATA_HPP_
#define SRC_MODULELOCATEDATA_STEPLOCATEDATA_HPP_
#include "../HelloSession.h"
#include "step/Step.hpp"
#include "step/HttpStep.hpp"
#include "storage/MemOperator.hpp"

namespace starshiplib
{

class StepLocateData: public oss::Step
{
public:
    StepLocateData(const oss::tagMsgShell& stInMsgShell, const HttpMsg& oInHttpMsg);
    virtual ~StepLocateData();

    virtual oss::E_CMD_STATUS Emit(int iErrno, const std::string& strErrMsg = "", const std::string& strErrClientShow = "");

    virtual oss::E_CMD_STATUS Callback(
                    const oss::tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody,
                    void* data = NULL);

    virtual oss::E_CMD_STATUS Timeout();

protected:
    bool Response(int iErrno, const std::string& strErrMsg, const std::string& strErrClientShow);

private:
    oss::tagMsgShell m_stInMsgShell;
    HttpMsg m_oInHttpMsg;
};

} /* namespace oss */

#endif /* SRC_MODULELOCATEDATA_STEPLOCATEDATA_HPP_ */
