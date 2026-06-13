#include "ModuleLua.hpp"
#include "labor/Labor.hpp"
#include <lua.hpp>
#include <cstring>
#define GET_TOKEN_GEN (10001)

// ── metatable ──
static int msg_path(lua_State* L) {
    auto* msg = (HttpMsg*)lua_touserdata(L, 1);
    lua_pushstring(L, msg ? msg->path().c_str() : ""); return 1;
}
static int msg_body(lua_State* L) {
    auto* msg = (HttpMsg*)lua_touserdata(L, 1);
    lua_pushstring(L, msg ? msg->body().c_str() : ""); return 1;
}
static int msg_method(lua_State* L) {
    auto* msg = (HttpMsg*)lua_touserdata(L, 1);
    lua_pushinteger(L, msg ? msg->method() : 0); return 1;
}
static int msg_headers(lua_State* L) {
    auto* msg = (HttpMsg*)lua_touserdata(L, 1);
    if (!msg) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, (void*)&msg->headers());
    luaL_getmetatable(L, "HttpHeaders"); lua_setmetatable(L, -2); return 1;
}
static int headers_index(lua_State* L) {
    auto* h = (std::unordered_map<std::string, std::string>*)lua_touserdata(L, 1);
    const char* key = lua_tostring(L, 2);
    if (!h || !key) { lua_pushnil(L); return 1; }
    auto it = h->find(key);
    lua_pushstring(L, it != h->end() ? it->second.c_str() : ""); return 1;
}

// ── LogicStep: 发到 LOGIC 等响应 ──
class LogicStep : public net::Step
{
public:
    LogicStep(ModuleLua* mod, net::tagMsgShell shell, int cbRef, std::string body)
        : net::Step(shell), m_pMod(mod), m_iCbRef(cbRef), m_shell(shell), m_body(std::move(body)) {}
    virtual net::E_CMD_STATUS Timeout() override {
        GetLabor()->SendToClientFast(m_shell, R"({"code":1,"msg":"logic timeout"})", 33);
        luaL_unref(m_pMod->GetLua(), LUA_REGISTRYINDEX, m_iCbRef);
        return net::STATUS_CMD_RUNNING;
    }
    virtual net::E_CMD_STATUS Emit(int, const std::string&, const std::string&) override {
        // seq 必须回填为本 Step 的序列号: 框架靠回包头里的 seq 在 mapCallbackStep 中
        // 找回本 Step 并触发 Callback (见 Worker.cpp mapCallbackStep.find(oInMsgHead.seq()))。
        // 缺 seq → LOGIC 回包 seq=0 → 匹配不到 → Callback 永不触发 → 超时。
        MsgHead h; h.set_cmd(GET_TOKEN_GEN); h.set_seq(GetSequence());
        MsgBody b; b.set_body(m_body);
        h.set_msgbody_len(static_cast<uint32_t>(b.ByteSizeLong()));
        GetLabor()->SendToSession("LOGIC", h, b);
        return net::STATUS_CMD_RUNNING;
    }
    virtual net::E_CMD_STATUS Callback(const net::tagMsgShell&, const MsgHead&, const MsgBody& oBody, void*) override {
        lua_State* L = m_pMod->GetLua();
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_iCbRef);
        lua_pushlstring(L, oBody.body().data(), oBody.body().size());
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
            size_t len; const char* resp = lua_tolstring(L, -1, &len);
            GetLabor()->SendToClientFast(m_shell, resp, len); lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, m_iCbRef);
        return net::STATUS_CMD_RUNNING;
    }
private:
    ModuleLua* m_pMod; int m_iCbRef; net::tagMsgShell m_shell; std::string m_body;
};

// ── SendToLogic Lua 绑定 ──
static int lua_SendToLogic(lua_State* L)
{
    size_t blen; const char* body = lua_tolstring(L, 1, &blen);
    if (!body || !lua_isfunction(L, 2)) { lua_pushboolean(L, 0); return 1; }
    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__module_instance"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* self = (ModuleLua*)lua_touserdata(L, -1); lua_pop(L, 1);
    if (!self) { lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, "__current_shell"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* curShell = (net::tagMsgShell*)lua_touserdata(L, -1); lua_pop(L, 1);
    net::tagMsgShell shell = curShell ? *curShell : net::tagMsgShell();
    auto* step = new LogicStep(self, shell, cbRef, std::string(body, blen));
    net::Step* baseStep = step; // 基类指针: Emit 可见默认参数(ERR_OK=0, 空 str)
    bool ok = GetLabor()->RegisterCallback(std::unique_ptr<net::Step>(step));
    // 框架规范: RegisterCallback 后必须显式 Emit 发送消息
    if (ok) {
        baseStep->Emit(0);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── ModuleLua ──
ModuleLua::ModuleLua() {}
ModuleLua::~ModuleLua() { if (m_pLua) { luaL_unref(m_pLua, LUA_REGISTRYINDEX, m_iFuncRef); lua_close(m_pLua); } }

bool ModuleLua::Init() {
    m_pLua = luaL_newstate();
    if (!m_pLua) return false;
    luaL_openlibs(m_pLua);
    lua_pushstring(m_pLua, "__module_instance");
    lua_pushlightuserdata(m_pLua, this); lua_rawset(m_pLua, LUA_REGISTRYINDEX);

    luaL_newmetatable(m_pLua, "HttpMsg");
    lua_pushcfunction(m_pLua, msg_path); lua_setfield(m_pLua, -2, "path");
    lua_pushcfunction(m_pLua, msg_body); lua_setfield(m_pLua, -2, "body");
    lua_pushcfunction(m_pLua, msg_method); lua_setfield(m_pLua, -2, "method");
    lua_pushcfunction(m_pLua, msg_headers); lua_setfield(m_pLua, -2, "headers");
    lua_pushvalue(m_pLua, -1); lua_setfield(m_pLua, -2, "__index"); lua_pop(m_pLua, 1);
    luaL_newmetatable(m_pLua, "HttpHeaders");
    lua_pushcfunction(m_pLua, headers_index); lua_setfield(m_pLua, -2, "__index"); lua_pop(m_pLua, 1);

    lua_register(m_pLua, "SendToClientFast", [](lua_State* L) {
        size_t len; const char* b = lua_tolstring(L, 1, &len);
        if (!b) return 0;
        lua_pushstring(L, "__current_shell"); lua_rawget(L, LUA_REGISTRYINDEX);
        auto* shell = (net::tagMsgShell*)lua_touserdata(L, -1); lua_pop(L, 1);
        if (shell) GetLabor()->SendToClientFast(*shell, b, len);
        return 0;
    });
    lua_pushcfunction(m_pLua, lua_SendToLogic); lua_setglobal(m_pLua, "SendToLogic");

    if (m_strScriptPath.empty())
        GetModuleConf().Get("script_path", m_strScriptPath);
    if (!m_strScriptPath.empty()) {
        if (luaL_dofile(m_pLua, m_strScriptPath.c_str()) != LUA_OK) return false;
        lua_getglobal(m_pLua, "handle_request");
        if (!lua_isfunction(m_pLua, -1)) return false;
        m_iFuncRef = luaL_ref(m_pLua, LUA_REGISTRYINDEX);
    }
    return true;
}

bool ModuleLua::AnyMessage(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg) {
    if (m_iFuncRef == LUA_NOREF) return false;
    lua_pushstring(m_pLua, "__current_shell");
    lua_pushlightuserdata(m_pLua, const_cast<net::tagMsgShell*>(&stMsgShell));
    lua_rawset(m_pLua, LUA_REGISTRYINDEX);
    lua_rawgeti(m_pLua, LUA_REGISTRYINDEX, m_iFuncRef);
    lua_pushlightuserdata(m_pLua, const_cast<HttpMsg*>(&oInHttpMsg));
    luaL_getmetatable(m_pLua, "HttpMsg"); lua_setmetatable(m_pLua, -2);
    if (lua_pcall(m_pLua, 1, 0, 0) != LUA_OK) {
        LOG4_ERROR("ModuleLua: %s", lua_tostring(m_pLua, -1));
        lua_pop(m_pLua, 1);
        lua_pushstring(m_pLua, "__current_shell"); lua_pushnil(m_pLua); lua_rawset(m_pLua, LUA_REGISTRYINDEX);
        return false;
    }
    lua_pushstring(m_pLua, "__current_shell"); lua_pushnil(m_pLua); lua_rawset(m_pLua, LUA_REGISTRYINDEX);
    return true;
}

extern "C" net::Module* create() { return new ModuleLua(); }
