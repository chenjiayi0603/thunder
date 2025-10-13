/*******************************************************************************
* * Project:  Thunder
 * @file     CmdUpdateConfig.hpp
 * @brief    更新配置
 * @author   cjy
 * @date:    2019年11月5日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef SRC_UPDATE_CONFIG_HPP_
#define SRC_UPDATE_CONFIG_HPP_

#include "cmd/Cmd.hpp"

namespace net
{
/**
 * @brief   更新配置指令
 */
class CmdUpdateConfig : public Cmd
{
public:
    CmdUpdateConfig()= default;
    virtual ~CmdUpdateConfig()= default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
private:
    bool ReadConfig();
    std::string m_ReqConfigFileName;
    util::CJsonObject m_ReqConfigContent;
    int m_ReqConfigType = 0;
};

} /* namespace bolt */

#endif /* SRC_CMD_SYS_CMD_CMDBEAT_HPP_ */
