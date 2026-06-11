#pragma once
#include "cmd/Module.hpp"
#include "step/Step.hpp"
#include <string>
#include <lua.hpp>

class ModuleLua : public net::Module
{
public:
    ModuleLua();
    virtual ~ModuleLua();
    virtual bool Init() override;
    virtual bool AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg) override;
    void SetScriptPath(const std::string& path) { m_strScriptPath = path; }
    lua_State* GetLua() { return m_pLua; }
    static void OnLogicResponse(const MsgHead&, const MsgBody&, net::StepParam*, net::Step* pStep);
private:
    lua_State* m_pLua = nullptr;
    int m_iFuncRef = LUA_NOREF;
    std::string m_strScriptPath;
};
extern "C" { net::Module* create_echo(); net::Module* create_route(); net::Module* create_limit(); }
