/*******************************************************************************
 * Project:  LoginServer
 * @file     ModuleLocateData.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_
#define SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_

#include "ModuleLocateData/StepLocateData.hpp"
#include "cmd/Module.hpp"

#ifdef __cplusplus
extern "C" {
#endif
oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace starshiplib
{

class ModuleLocateData: public oss::Module
{
public:
    ModuleLocateData();
    virtual ~ModuleLocateData();
    bool Init();
    virtual bool AnyMessage(
                    const oss::tagMsgShell& stMsgShell,
                    const HttpMsg& oInHttpMsg);

public:
    StepLocateData* pStepLocateData;
    HelloSession* pHelloSession;
};

} /* namespace oss */

#endif /* SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_ */
