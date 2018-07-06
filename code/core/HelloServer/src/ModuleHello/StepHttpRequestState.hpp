/*******************************************************************************
 * Project:  HelloThunder
 * @file     StepHttpRequest.hpp
 * @brief 
 * @author   chenjiayi
 * @date:    2017年11月3日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDHELLO_StepHttpRequestState_HPP_
#define SRC_CMDHELLO_StepHttpRequestState_HPP_
#include "step/HttpStep.hpp"
#include "step/StepState.hpp"
#include "step/StepCo.hpp"

namespace starshiplib
{

class StepHttpRequestState: public oss::StepState
{
public:
	StepHttpRequestState(const oss::tagMsgShell& stMsgShell,const HttpMsg& oInHttpMsg);
    virtual ~StepHttpRequestState();
    oss::E_CMD_STATUS State0();//回调成员函数调用不带自定义参数（参数在step成员变量中实现）
    oss::E_CMD_STATUS State1();
    oss::E_CMD_STATUS State2();
    oss::E_CMD_STATUS State3();
    oss::E_CMD_STATUS State4();

    void Response(int nCode);
    HttpMsg m_oInHttpMsg;
    uint32 m_uiTestVal;
};

} /* namespace hello */

#endif /* SRC_CMDHELLO_STEPHTTPREQUEST_HPP_ */
