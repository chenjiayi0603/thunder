#include "ModuleRaw.hpp"
#include "labor/Labor.hpp"

MUDULE_CREATE(ModuleRaw);

bool ModuleRaw::AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg&)
{
    static const char kResp[] = "{\"code\":0,\"msg\":\"ok\"}";
    GetLabor()->SendToClientFast(stMsgShell, kResp, sizeof(kResp) - 1);
    return true;
}
