#ifndef SRC_CMD_SYS_CMD_RELOAD_LUA_HPP_
#define SRC_CMD_SYS_CMD_RELOAD_LUA_HPP_

#include "cmd/Cmd.hpp"

namespace net {

class CmdReloadLua : public Cmd {
public:
    CmdReloadLua() = default;
    virtual ~CmdReloadLua() = default;
    virtual bool AnyMessage(
                    const tagMsgShell& stMsgShell,
                    const MsgHead& oInMsgHead,
                    const MsgBody& oInMsgBody);
};

} /* namespace net */

#endif
