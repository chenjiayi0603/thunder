#include "ModuleLua.hpp"
#include "labor/Labor.hpp"
#include <dirent.h>
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

// ── NodeTypeStep: 向指定节点类型发送消息（支持回调异步 / fire-and-forget）──
class NodeTypeStep : public net::Step
{
public:
    NodeTypeStep(ModuleLua* mod, net::tagMsgShell shell, int cbRef,
                 std::string nodeType, uint32 cmd, std::string body,
                 std::string targetId)
        : net::Step(shell), m_pMod(mod), m_iCbRef(cbRef), m_shell(shell)
        , m_nodeType(std::move(nodeType)), m_cmd(cmd), m_body(std::move(body))
        , m_targetId(std::move(targetId)) {}

    virtual net::E_CMD_STATUS Timeout() override {
        GetLabor()->SendToClientFast(m_shell, R"({"code":1,"msg":"sendtonodetype timeout"})", 38);
        if (m_iCbRef != LUA_NOREF)
            luaL_unref(m_pMod->GetLua(), LUA_REGISTRYINDEX, m_iCbRef);
        return net::STATUS_CMD_RUNNING;
    }

    virtual net::E_CMD_STATUS Emit(int, const std::string&, const std::string&) override {
        // seq 必须回填为本 Step 的序列号: 框架靠回包头里的 seq 在 mapCallbackStep 中
        // 找回本 Step 并触发 Callback。缺 seq → 回包 seq=0 → 匹配不到 → 永不触发。
        MsgHead h;
        h.set_cmd(m_cmd);
        h.set_seq(GetSequence());
        MsgBody b;
        b.set_body(m_body);
        if (!m_targetId.empty())
            b.set_targetid(m_targetId);
        h.set_msgbody_len(static_cast<uint32_t>(b.ByteSizeLong()));

        if (m_iCbRef != LUA_NOREF)
        {
            // 异步等待回调: 使用 SendToSession (按 targetId 一致性哈希 / 轮询)
            GetLabor()->SendToSession(m_nodeType, h, b);
        }
        else
        {
            // Fire-and-forget: 广播到该类型所有节点，不期待回包
            GetLabor()->SendToNodeType(m_nodeType, h, b);
        }
        return net::STATUS_CMD_RUNNING;
    }

    virtual net::E_CMD_STATUS Callback(const net::tagMsgShell&, const MsgHead&, const MsgBody& oBody, void*) override {
        if (m_iCbRef == LUA_NOREF)
            return net::STATUS_CMD_COMPLETED;

        lua_State* L = m_pMod->GetLua();
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_iCbRef);
        lua_pushlstring(L, oBody.body().data(), oBody.body().size());
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1))
        {
            size_t len;
            const char* resp = lua_tolstring(L, -1, &len);
            GetLabor()->SendToClientFast(m_shell, resp, len);
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, m_iCbRef);
        return net::STATUS_CMD_RUNNING;
    }

private:
    ModuleLua* m_pMod;
    int m_iCbRef;
    net::tagMsgShell m_shell;
    std::string m_nodeType;
    uint32 m_cmd;
    std::string m_body;
    std::string m_targetId;
};

// ── SendToNodeType Lua 绑定 ──
// 签名: SendToNodeType(nodeType, cmd, body, [targetId], [timeout], [callback])
//   nodeType  (string, 必填)  目标节点类型
//   cmd       (number, 必填)  命令字
//   body      (string, 必填)  消息体
//   targetId  (string, 可选)  目标标识（一致性哈希路由）
//   timeout   (number, 可选)  超时秒数（默认 0.5）
//   callback  (function, 可选) 异步回调函数; 缺省时 fire-and-forget
static int lua_SendToNodeType(lua_State* L)
{
    int n = lua_gettop(L);
    if (n < 3) { lua_pushboolean(L, 0); return 1; }

    const char* nodeType = lua_tostring(L, 1);
    if (!nodeType) { lua_pushboolean(L, 0); return 1; }

    int cmd = static_cast<int>(lua_tointeger(L, 2));

    size_t blen;
    const char* body = lua_tolstring(L, 3, &blen);
    if (!body) { lua_pushboolean(L, 0); return 1; }

    // 解析可选参数 (4..n): targetId(string), timeout(number), callback(function)
    std::string targetId;
    double timeoutSec = 0.5;
    int cbRef = LUA_NOREF;

    for (int i = 4; i <= n; i++)
    {
        if (lua_isfunction(L, i))
        {
            lua_pushvalue(L, i);
            cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        else if (lua_isnumber(L, i))
        {
            // 必须在 lua_isstring 之前: lua_isstring 对数字也返回 true
            timeoutSec = lua_tonumber(L, i);
        }
        else if (lua_isstring(L, i))
        {
            const char* s = lua_tostring(L, i);
            if (s) targetId = s;
        }
    }

    lua_pushstring(L, "__module_instance"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* self = static_cast<ModuleLua*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!self)
    {
        if (cbRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
        lua_pushboolean(L, 0); return 1;
    }

    lua_pushstring(L, "__current_shell"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* curShell = static_cast<net::tagMsgShell*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    net::tagMsgShell shell = curShell ? *curShell : net::tagMsgShell();

    auto* step = new NodeTypeStep(self, shell, cbRef,
                                  nodeType, static_cast<uint32>(cmd),
                                  std::string(body, blen), targetId);
    step->SetTimeout(timeoutSec);
    net::Step* baseStep = step;
    bool ok = GetLabor()->RegisterCallback(std::unique_ptr<net::Step>(step));
    if (ok)
    {
        baseStep->Emit(0);
    }
    else if (cbRef != LUA_NOREF)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── SendToLogic 兼容封装（内部委托给 SendToNodeType 模式）──
static int lua_SendToLogic(lua_State* L)
{
    // 保持原签名: SendToLogic(body, callback)
    // 调度到 NodeTypeStep(nodeType="LOGIC", cmd=GET_TOKEN_GEN, ...)
    size_t blen; const char* body = lua_tolstring(L, 1, &blen);
    if (!body || !lua_isfunction(L, 2)) { lua_pushboolean(L, 0); return 1; }

    lua_pushstring(L, "__module_instance"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* self = static_cast<ModuleLua*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!self) { lua_pushboolean(L, 0); return 1; }

    lua_pushstring(L, "__current_shell"); lua_rawget(L, LUA_REGISTRYINDEX);
    auto* curShell = static_cast<net::tagMsgShell*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    net::tagMsgShell shell = curShell ? *curShell : net::tagMsgShell();

    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);

    auto* step = new NodeTypeStep(self, shell, cbRef,
                                  "LOGIC", GET_TOKEN_GEN,
                                  std::string(body, blen), "");
    net::Step* baseStep = step;
    bool ok = GetLabor()->RegisterCallback(std::unique_ptr<net::Step>(step));
    if (ok) baseStep->Emit(0);
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
    lua_pushcfunction(m_pLua, lua_SendToNodeType); lua_setglobal(m_pLua, "SendToNodeType");
    lua_pushcfunction(m_pLua, lua_SendToLogic); lua_setglobal(m_pLua, "SendToLogic");

    // 从 SetModuleConf 读取配置
    if (m_strScriptPath.empty())
        GetModuleConf().Get("script_path", m_strScriptPath);
    std::string scriptContent;
    GetModuleConf().Get("script_content", scriptContent);

    // 无配置: opendir 查找 scripts/ 下第一个 .lua 文件
    if (m_strScriptPath.empty() && scriptContent.empty()) {
        DIR* dir = opendir("scripts");
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                std::string name(ent->d_name);
                if (name.size() > 4 && name.substr(name.size()-4) == ".lua") {
                    m_strScriptPath = std::string("scripts/") + name;
                    break;
                }
            }
            closedir(dir);
        }
    }

    LOG4_TRACE("ModuleLua::Init: cmd=%d script_path='%s' script_content_len=%zu",
              GetCmd(), m_strScriptPath.c_str(), scriptContent.size());
    if (!scriptContent.empty()) {
        // etcd 下发的内联脚本内容优先于文件路径
        if (luaL_loadbuffer(m_pLua, scriptContent.data(), scriptContent.size(), m_strScriptPath.empty() ? "etcd" : m_strScriptPath.c_str()) != LUA_OK) {
            LOG4_ERROR("ModuleLua: loadbuffer error: %s", lua_tostring(m_pLua, -1));
            return false;
        }
        if (lua_pcall(m_pLua, 0, 0, 0) != LUA_OK) {
            LOG4_ERROR("ModuleLua: exec error: %s", lua_tostring(m_pLua, -1));
            return false;
        }
    } else if (!m_strScriptPath.empty()) {
        if (luaL_dofile(m_pLua, m_strScriptPath.c_str()) != LUA_OK) {
            LOG4_ERROR("ModuleLua: dofile %s error: %s", m_strScriptPath.c_str(), lua_tostring(m_pLua, -1));
            return false;
        }
    } else {
        return true; // 未配置脚本, 模块加载成功但无处理函数
    }
    lua_getglobal(m_pLua, "handle_request");
    if (!lua_isfunction(m_pLua, -1)) return false;
    m_iFuncRef = luaL_ref(m_pLua, LUA_REGISTRYINDEX);
    return true;
}

bool ModuleLua::AnyMessage(const net::tagMsgShell& stMsgShell, const MsgHead& oInMsgHead, const MsgBody& oInMsgBody) {
    // cmd 模式（Logic 节点使用）：将请求体作为字符串传给 Lua
    LOG4_TRACE("ModuleLua(cmd) ENTER: cmd=%u seq=%u fd=%d", oInMsgHead.cmd(), oInMsgHead.seq(), stMsgShell.iFd);
    if (m_iFuncRef == LUA_NOREF) {
        LOG4_ERROR("ModuleLua(cmd): m_iFuncRef is LUA_NOREF, handler not loaded");
        return false;
    }
    lua_pushstring(m_pLua, "__current_shell");
    lua_pushlightuserdata(m_pLua, const_cast<net::tagMsgShell*>(&stMsgShell));
    lua_rawset(m_pLua, LUA_REGISTRYINDEX);
    lua_rawgeti(m_pLua, LUA_REGISTRYINDEX, m_iFuncRef);
    std::string body = oInMsgBody.body();
    lua_pushlstring(m_pLua, body.data(), body.size());
    LOG4_TRACE("ModuleLua(cmd): calling handle_request, body_len=%zu", body.size());
    int nret = 0;
    if (lua_pcall(m_pLua, 1, 1, 0) != LUA_OK) {
        LOG4_ERROR("ModuleLua(cmd): %s", lua_tostring(m_pLua, -1));
        lua_pop(m_pLua, 1);
        lua_pushstring(m_pLua, "__current_shell"); lua_pushnil(m_pLua); lua_rawset(m_pLua, LUA_REGISTRYINDEX);
        return false;
    }
    nret = lua_gettop(m_pLua);
    LOG4_TRACE("ModuleLua(cmd): handle_request returned, nret=%d", nret);
    if (nret > 0 && lua_isstring(m_pLua, -1)) {
        size_t len;
        const char* resp = lua_tolstring(m_pLua, -1, &len);
        LOG4_TRACE("ModuleLua(cmd): response string len=%zu, sending via SendToClient", len);
        // SendToClient(MsgHead, string) 内部会 set_cmd(cmd+1)，
        // 所以这里传原始请求 cmd 即可（不预先 +1）
        MsgHead oOutMsgHead;
        oOutMsgHead.set_cmd(oInMsgHead.cmd());  // SendToClient 会 +1 → 10002
        oOutMsgHead.set_seq(oInMsgHead.seq());
        oOutMsgHead.set_msgbody_len(static_cast<uint32_t>(len));
        bool ok = GetLabor()->SendToClient(stMsgShell, oOutMsgHead, std::string(resp, len));
        LOG4_TRACE("ModuleLua(cmd): SendToClient returned %d", ok);
        lua_pop(m_pLua, 1);
    } else if (nret > 0) {
        LOG4_TRACE("ModuleLua(cmd): non-string return value, discarding");
        lua_pop(m_pLua, 1);
    } else {
        LOG4_TRACE("ModuleLua(cmd): no return value from handle_request");
    }
    lua_pushstring(m_pLua, "__current_shell"); lua_pushnil(m_pLua); lua_rawset(m_pLua, LUA_REGISTRYINDEX);
    LOG4_TRACE("ModuleLua(cmd) EXIT OK");
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

bool ModuleLua::ReloadScript() {
    if (!m_pLua) {
        LOG4_ERROR("ModuleLua::ReloadScript() no lua_State");
        return false;
    }
    // 释放旧 handle_request 引用（不关 VM，保留 C 函数注册、metatable、JIT trace）
    if (m_iFuncRef != LUA_NOREF) {
        luaL_unref(m_pLua, LUA_REGISTRYINDEX, m_iFuncRef);
        m_iFuncRef = LUA_NOREF;
    }
    // 清理全局表（清除旧脚本定义的变量和函数）
    lua_pushnil(m_pLua); lua_setglobal(m_pLua, "handle_request");

    // 重新从模块配置读取脚本路径
    GetModuleConf().Get("script_path", m_strScriptPath);

    // 加载新脚本
    std::string scriptContent;
    GetModuleConf().Get("script_content", scriptContent);
    if (!scriptContent.empty()) {
        if (luaL_loadbuffer(m_pLua, scriptContent.data(), scriptContent.size(),
                            m_strScriptPath.empty() ? "etcd" : m_strScriptPath.c_str()) != LUA_OK) {
            LOG4_ERROR("ModuleLua::ReloadScript: loadbuffer error: %s", lua_tostring(m_pLua, -1));
            lua_pop(m_pLua, 1);
            return false;
        }
    } else if (!m_strScriptPath.empty()) {
        if (luaL_dofile(m_pLua, m_strScriptPath.c_str()) != LUA_OK) {
            LOG4_ERROR("ModuleLua::ReloadScript: dofile %s error: %s",
                       m_strScriptPath.c_str(), lua_tostring(m_pLua, -1));
            lua_pop(m_pLua, 1);
            return false;
        }
    }
    // pcall 执行脚本（定义新的 handle_request）
    if (lua_pcall(m_pLua, 0, 0, 0) != LUA_OK) {
        LOG4_ERROR("ModuleLua::ReloadScript: exec error: %s", lua_tostring(m_pLua, -1));
        lua_pop(m_pLua, 1);
        return false;
    }
    // 注册新的 handle_request
    lua_getglobal(m_pLua, "handle_request");
    if (!lua_isfunction(m_pLua, -1)) {
        LOG4_ERROR("ModuleLua::ReloadScript: no handle_request in new script");
        lua_pop(m_pLua, 1);
        return false;
    }
    m_iFuncRef = luaL_ref(m_pLua, LUA_REGISTRYINDEX);
    LOG4_INFO("ModuleLua::ReloadScript() ok: %s", m_strScriptPath.c_str());
    return true;
}

extern "C" net::Module* create() { return new ModuleLua(); }
