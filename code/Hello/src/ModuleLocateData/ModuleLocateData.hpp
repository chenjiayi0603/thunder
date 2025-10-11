/*******************************************************************************
 * Project:  Hello
 * @file     ModuleLocateData.hpp
 * @brief 
 * @author   Tommy
 * @date:    2016年4月19日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_
#define SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_

#include "ModuleLocateData/StepLocateData.hpp"
#include "cmd/Module.hpp"

namespace im
{

class ModuleLocateData: public net::Module
{
public:
    ModuleLocateData() = default;
    virtual ~ModuleLocateData() = default;
    bool Init();
    virtual bool AnyMessage(
                    const net::tagMsgShell& stMsgShell,
                    const HttpMsg& oInHttpMsg);
public:
    StepLocateData* pStepLocateData = nullptr;
    HelloSession* pHelloSession = nullptr;
};

} /* namespace net */

#endif /* SRC_MODULELOCATEDATA_MODULELOCATEDATA_HPP_ */
