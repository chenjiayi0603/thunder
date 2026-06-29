#include "CmdReloadLua.hpp"
#include "labor/Worker.hpp"
#include "util/json/CJsonObject.hpp"

namespace net {

bool CmdReloadLua::AnyMessage(
                const tagMsgShell& stMsgShell,
                const MsgHead& oInMsgHead,
                const MsgBody& oInMsgBody)
{
    util::CJsonObject oModuleConfJson;
    if (!oModuleConfJson.Parse(oInMsgBody.body())) {
        LOG4_WARN("CmdReloadLua: failed to parse json: %s", oInMsgBody.body().c_str());
        return true;
    }
    int count = oModuleConfJson.GetArraySize();
    LOG4_INFO("CMD_REQ_RELOAD_LUA: %d lua module(s), reloading in-place (no SO restart)", count);
    static_cast<Worker*>(GetLabor())->LuaReloadScript(oModuleConfJson);
    return true;
}

} /* namespace net */
