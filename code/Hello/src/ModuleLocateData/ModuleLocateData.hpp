/*******************************************************************************
 * Project:  Hello
 * @file     ModuleLocateData.hpp
 * @brief
 * @author   cjy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_
#define SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_

#include "StepLocateData.hpp"
#include "cmd/Module.hpp"

namespace robot
{

class ModuleLocateData : public net::Module
{
public:
    ModuleLocateData();
    virtual ~ModuleLocateData();
    bool         Init();
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg);

public:
    StepLocateData* pStepLocateData;
    HelloSession*   pHelloSession;
};

} // namespace robot

#endif /* SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_ */
