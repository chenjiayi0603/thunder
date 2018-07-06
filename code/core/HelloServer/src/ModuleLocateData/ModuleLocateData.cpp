/*******************************************************************************
 * Project:  HelloServer
 * @file     ModuleLocateData.cpp
 * @brief 	 返回数据分布信息
 * @author   cjy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#include "ModuleLocateData.hpp"

#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create()
{
    oss::Cmd* pCmd = new starshiplib::ModuleLocateData();
    return(pCmd);
}
#ifdef __cplusplus
}
#endif

namespace starshiplib
{

ModuleLocateData::ModuleLocateData()
    : pStepLocateData(NULL),pHelloSession(NULL)
{
}

ModuleLocateData::~ModuleLocateData()
{
}

bool ModuleLocateData::Init()
{
    return(true);
}

bool ModuleLocateData::AnyMessage(
                const oss::tagMsgShell& stMsgShell,
                const HttpMsg& oInHttpMsg)
{
/*
{
    "note":"factor or factor_string(only the factor_type was 3) should be set.",
    "factor":userid%100,
    "tb_name":"tb_userinfo",
    "redis_key":"1:3:userid"
}
*/
    pStepLocateData = new StepLocateData (stMsgShell, oInHttpMsg);
    if (RegisterCallback(pStepLocateData))
    {
        if (oss::STATUS_CMD_RUNNING == pStepLocateData->Emit(ERR_OK))
        {
            return(true);
        }
        else
        {
            DeleteCallback(pStepLocateData);
            return(false);
        }
    }
    return(false);
}

} /* namespace oss */
