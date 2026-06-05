/**
 * @file     EtcdParse.hpp
 * @brief    EtcdCenterConnector 的纯解析/决策逻辑（无 I/O，便于单元测试）
 *
 * 把"解析 etcd HTTP gateway 响应"与"注册动作决策"从带副作用的 curl 调用中拆出来，
 * 单测可直接喂真实响应字符串验证，不依赖运行中的 etcd。
 *
 * 设计要点（踩坑记录）：
 *   - grpc-gateway 把所有 int64 字段（revision/compact_revision/lease）编码为 JSON
 *     字符串（如 "84"），不是 JSON 数字。必须按字符串取再转换，否则 Get 失败、
 *     revision 取不到 → start_revision 退化为 1 → 低于 compact_revision → watch 被
 *     etcd 立即取消，陷入每秒重连风暴 (issus #20)。
 *   - watch 的 created / canceled / compact_revision 都在 result 子对象里，不在顶层。
 */
#ifndef ETCD_PARSE_HPP_
#define ETCD_PARSE_HPP_

#include <cstdint>
#include <string>

#include "util/json/CJsonObject.hpp"

namespace net
{
namespace etcd_parse
{

/// 从 JSON 对象按"字符串优先、int64 兜底"取一个 gateway int64 字段。
/// 注意：CJsonObject 的整型重载是 `long long`（util::int64），与平台 int64_t(long)
/// 不是同一类型，引用不可隐式转换，故兜底用 long long 局部变量再赋值。
inline bool GetGatewayInt64(const util::CJsonObject& o, const std::string& key, int64_t& out)
{
    auto& mo = const_cast<util::CJsonObject&>(o);
    std::string s;
    if (mo.Get(key, s) && !s.empty())
    {
        try { out = static_cast<int64_t>(std::stoll(s)); return true; }
        catch (...) { /* 落到 int64 兜底 */ }
    }
    long long v = 0;  // 匹配 CJsonObject::Get(const std::string&, int64&) 重载
    if (mo.Get(key, v)) { out = static_cast<int64_t>(v); return true; }
    return false;
}

/// 解析 /v3/kv/range 响应，取出 header.revision。空 keyspace 也有 header.revision。
inline bool ParseRangeRevision(const std::string& resp, int64_t& outRev)
{
    if (resp.empty()) return false;
    util::CJsonObject o;
    if (!o.Parse(resp)) return false;
    util::CJsonObject h;
    if (!o.Get("header", h)) return false;
    return GetGatewayInt64(h, "revision", outRev);
}

/// 解析注册键 /v3/kv/range 响应：存在则取出 node_id 与该键绑定的 lease（无租约为 0）。
/// @return true 表示 key 存在且 node_id 解析成功
inline bool ParseRegistryKv(const std::string& resp, uint32_t& outNodeId, int64_t& outLease)
{
    outNodeId = 0;
    outLease  = 0;
    if (resp.empty()) return false;

    util::CJsonObject o;
    if (!o.Parse(resp)) return false;

    int64_t count = 0;
    if (!GetGatewayInt64(o, "count", count) || count <= 0) return false;

    util::CJsonObject kvs;
    if (!o.Get("kvs", kvs) || !kvs.IsArray() || kvs.GetArraySize() <= 0) return false;

    util::CJsonObject kv;
    if (!kvs.Get(0, kv)) return false;

    // lease 字段：gateway int64 字符串，缺省 "0"
    GetGatewayInt64(kv, "lease", outLease);

    std::string valueB64;
    if (!kv.Get("value", valueB64)) return false;
    // value 的 base64 解码与 node_id 提取交给调用方（B64Dec 在连接器里），
    // 这里只判定 key 是否存在 + lease；node_id 由调用方补。为可独立测试，
    // 同时支持 value 已是明文 JSON 的情况。
    util::CJsonObject val;
    if (val.Parse(valueB64))
    {
        int64_t nid = 0;
        if (GetGatewayInt64(val, "node_id", nid) && nid > 0 && nid <= 255)
        {
            outNodeId = static_cast<uint32_t>(nid);
            return true;
        }
    }
    // 调用方仍可凭 outLease 判断 key 存在；node_id 为 0 表示需调用方解码 base64。
    return true;
}

/// watch 单行响应的控制信息（created/canceled/compact_revision 均在 result 子对象内）。
struct WatchControl
{
    bool    created          = false;
    bool    canceled         = false;
    int64_t compactRevision  = 0;
};

/// 解析 watch 单行 JSON 的控制位。返回 false 表示该行不是合法 watch 响应行。
inline bool ParseWatchControl(const std::string& line, WatchControl& out)
{
    if (line.empty() || line[0] != '{') return false;
    util::CJsonObject o;
    if (!o.Parse(line)) return false;

    util::CJsonObject result;
    if (!o.Get("result", result)) return false;

    result.Get("created", out.created);
    result.Get("canceled", out.canceled);
    GetGatewayInt64(result, "compact_revision", out.compactRevision);
    return true;
}

/// 注册动作（纯决策，issus #19 核心）。
enum class RegAction
{
    Claim,   ///< etcd 中无该注册键 → 走槽位抢占
    Rebind,  ///< 键存在但绑在别的租约（旧进程残留/已失效）→ 必须重绑到当前租约
    Fresh,   ///< 键存在且正绑在当前租约 → 真·幂等，续租即可
};

/// 决策：found=注册键是否存在；existingLease=该键绑定的租约；currentLease=本进程当前租约。
inline RegAction DecideRegAction(bool found, int64_t existingLease, int64_t currentLease)
{
    if (!found) return RegAction::Claim;
    if (currentLease != 0 && existingLease == currentLease) return RegAction::Fresh;
    return RegAction::Rebind;
}

}  // namespace etcd_parse
}  // namespace net

#endif /* ETCD_PARSE_HPP_ */
