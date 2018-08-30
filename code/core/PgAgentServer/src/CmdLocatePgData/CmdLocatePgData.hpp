/*******************************************************************************
 * Project:  DbAgent
 * @file     CmdLocatePgData.hpp
 * @brief 
 * @author   cjy
 * @date:    2016年4月18日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_
#define SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_

#include "util/json/CJsonObject.hpp"
#include "cmd/Cmd.hpp"
#include "storage/dataproxy.pb.h"

#ifdef __cplusplus
extern "C" {
#endif
    oss::Cmd* create();
#ifdef __cplusplus
}
#endif

namespace oss
{

class CmdLocatePgData: public Cmd
{
public:
    CmdLocatePgData();
    virtual ~CmdLocatePgData();

    virtual bool Init();

    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);

protected:
    std::string GetFullTableName(const std::string& strTableName, uint32 uiFactor);
    void Response(const tagMsgShell& stMsgShell, const MsgHead& oInMsgHead,
                            int iErrno, const std::string& strErrMsg);

private:
    loss::CJsonObject m_oDbConf;
    std::map<std::string, std::set<uint32> > m_mapFactorSection; //分段因子区间配置，key为因子类型
    std::map<std::string, loss::CJsonObject*> m_mapDbInstanceInfo;  //数据库配置信息key为("%u:%u:%u", uiDataType, uiFactor, uiFactorSection)
};

} /* namespace oss */

#endif /* SRC_CMDLOCATEDATA_CMDLOCATEDATA_HPP_ */
