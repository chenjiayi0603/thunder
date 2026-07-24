/**
 * ModuleMqttBroker.cpp — MQTT Broker 模块 (MQTT 3.1.1 协议核心)
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  第〇层 · 使用意图
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *   意图: 在 Thunder 框架内提供标准 MQTT 3.1.1 Broker, 使 IoT 设备
 *         (传感器 / 嵌入式设备 / 移动端) 能通过 MQTT 协议接入系统。
 *
 *   使用方式:
 *     1. 部署 MqttBroker 节点 (access_port=21883, codec=CODEC_MQTT)
 *     2. 加载 ModuleMqttBroker.so (Init 中默认开启 echo)
 *     3. IoT 设备 → tcp://host:21883 直接连接, 无需代理网关
 *
 *   不依赖:
 *     - 不需要 Interface/LB 网关 (MQTT 是二进制 TCP, 不走 HTTP)
 *     - 不需要 Protobuf / Redis / MySQL (纯内存 Broker)
 *     - 不需要 Lua 脚本
 *   仅依赖:
 *     - etcd (节点注册, 可选 — 单节点部署可关闭)
 *
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  第一层 · 端到端数据流向 (PUBLISH 完整路径)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  ┌──────────┐  TCP/MQTT    ┌──────────────────────────────────────┐  TCP/MQTT   ┌──────────┐
 *  │ 发布设备  │ ───────────→ │           Thunder Worker            │ ──────────→ │ 订阅设备A │
 *  │(publisher)│ (a)          │                                      │  (f)        │(subscriber│
 *  └──────────┘              │  ┌──────────┐    ┌───────────────┐  │            └──────────┘
 *                            │  │CodecMqtt │    │ModuleMqttBroker│  │
 *  ┌──────────┐  TCP/MQTT    │  │ ::Decode │    │  (本文件)      │  │  TCP/MQTT   ┌──────────┐
 *  │ 订阅设备B │ ←─────────── │  │          │    │               │  │ ──────────→ │ 订阅设备C │
 *  │(subscriber│  (f)        │  └────┬─────┘    └───────┬───────┘  │            │(subscriber│
 *  └──────────┘              │       │                  │          │            └──────────┘
 *                            │    (b) Decode         (d) Match     │
 *                            │    MsgHead+MsgBody    + Broadcast   │
 *                            │       │                  │          │
 *                            │       └───────(c)───────┘          │
 *                            │         AnyMessage(sh,head,body)    │
 *                            │                                    │
 *                            │  ┌──────────┐                      │
 *                            │  │CodecMqtt │ ← (e) SendTo(sh,     │
 *                            │  │ ::Encode │        head, body)   │
 *                            │  └──────────┘                      │
 *                            └────────────────────────────────────┘
 *
 *  步骤说明:
 *   (a) 发布设备发 TCP/MQTT 字节流到 Worker 的 21883 端口
 *   (b) CodecMqtt::Decode: 解析 MQTT Fixed Header → MsgHead.cmd + MsgBody.body(raw bytes)
 *   (c) Worker 调用 ModuleMqttBroker::AnyMessage(sh, head, body) — MQTT 消息总入口
 *   (d) HandlePublish: 解析 topic → MatchSubscribers 通配符匹配 → 遍历订阅者
 *   (e) SendMqttPacket → GetLabor()->SendTo(sh, head, body) → 发回给 Worker
 *   (f) CodecMqtt::Encode → TCP 字节流 → 发到各订阅设备
 *
 *   关键: SendTo() 而非 SendToClient() — SendToClient 会自动 cmd+1
 *         (框架的请求→响应惯例), 破坏 MQTT 包类型。MQTT 的 cmd 是
 *         精确编码的, 不能被框架自动修改。SendTo() 原样透传。
 *
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  第二层 · 模块内部分发路由 (AnyMessage → HandleXxx)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *   AnyMessage(sh, MsgHead, MsgBody)
 *     │
 *     ├─ cmd 逆运算: pt = (cmd - MQTT_CMD_BASE + 1) / 2
 *     │   (例: cmd=21001 → pt=1=MQTT_CONNECT)
 *     │
 *     ├─ 更新 KeepAlive 活跃时间 (除 CONNECT 外所有包)
 *     │
 *     └─ switch(pt):
 *          ├─ MQTT_CONNECT     → HandleConnect()     建连+协议校验+Will
 *          ├─ MQTT_SUBSCRIBE   → HandleSubscribe()   注册订阅+投递Retain
 *          ├─ MQTT_UNSUBSCRIBE → HandleUnsubscribe() 移除订阅
 *          ├─ MQTT_PUBLISH     → HandlePublish()     解析+匹配+广播+[Echo Demo]
 *          ├─ MQTT_PUBACK      → HandlePuback()      QoS 1 投递确认, 清理 pending
 *          ├─ MQTT_PINGREQ     → HandlePingreq()     回 PINGRESP
 *          └─ MQTT_DISCONNECT  → HandleDisconnect()  清理 fd→clientId 映射
 *
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  第三层 · Echo Demo 机制
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  Init() 中默认开启 echo:
 *    g_bEchoEnabled = true   ← 不需要额外 SO, 无需外部配置
 *
 *  HandlePublish 中:
 *    if (topic starts with "echo/")
 *        → 构造 echoTopic = topic + "/response"
 *        → MatchSubscribers(echoTopic) → 广播给匹配的订阅者
 *
 *  示例:
 *    客户端 A: subscribe "echo/+/response"
 *    客户端 B: publish "echo/ping" payload="hello"
 *    → 客户端 A 收到: topic="echo/ping/response" payload="hello"
 *
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  第四层 · 数据结构总览
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *   m_mapTopicSubscribers  : topicFilter → [{fd, seq}]        订阅表
 *   m_mapRetained          : topic → {payload, qos}           保留消息
 *   m_mapWill              : fd → {topic, message, qos}       遗嘱消息
 *   m_mapKeepAlive         : fd → {interval, lastActivity}    心跳追踪
 *   m_mapFdToClientId      : fd → clientId                    连接标识
 *   m_ucMaxQos             : Broker 最大 QoS (默认 1)         QoS 协商上限
 *   g_bEchoEnabled         : Echo 功能开关 (跨 SO 可见)
 *
 *
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  支持的 MQTT 功能清单
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *   P0 (必须):     CONNECT/CONNACK · SUBSCRIBE/SUBACK · PUBLISH(QoS 0/1)
 *                  PINGREQ/PINGRESP · DISCONNECT
 *   P1 (重要):     Retain 保留消息 · Topic 通配符(+ / #) · Will 遗嘱
 *   P1.5:          UNSUBSCRIBE/UNSUBACK
 *   Demo:          Echo 回显 (echo/ping → echo/ping/response)
 */
#include "ModuleMqttBroker.hpp"
#include "codec/CodecMqtt.hpp"
#include "labor/Labor.hpp"
#include "util/CBuffer.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <sstream>
#include <ctime>
#include <algorithm>

MUDULE_CREATE(mqtt::ModuleMqttBroker);

namespace mqtt
{

// ===== MQTT 二进制解析工具函数 ===============================================
// 这些函数处理 MQTT 包体的原始字节, 不依赖 CodecMqtt (CodecMqtt 只解固定头,
// 可变头+payload 以原始字节传入 Module)

// 大端序读取 uint16 (MQTT 协议中所有多字节整数均为大端)
static inline uint16_t ReadU16(const uint8_t* p) { return (p[0]<<8)|p[1]; }

// 大端序写入 uint16 → std::string 尾部
static void WriteU16(uint16_t v, std::string& o) { o.push_back((v>>8)&0xFF); o.push_back(v&0xFF); }

// 解析 MQTT Remaining Length (变长编码, 每字节低 7 位为数据, 最高位=1 表示后续还有字节)
// 返回: fixed header 总字节数 (type flag 1B + remaining length N 字节)
//   rl 输出解析出的 remaining length 值
//   最长 4 字节 (MQTT 3.1.1 spec), 超过则返回 5 (整个 fixed header)
static uint8_t ParseHdrLen(const uint8_t* p, size_t n, uint32_t& rl) {
    rl=0; uint32_t m=1; const uint8_t* e=p+n; const uint8_t* c=p+1;
    for (uint8_t b=0; b<4 && c<e; ++b,++c) { rl+=(*c&0x7F)*m; m*=128; if((*c&0x80)==0) return 1+b+1; }
    return 5;
}

// ===== Topic 匹配引擎 ====================================================
// MQTT 3.1.1 §4.7 Topic Filters:
//   +  匹配单级 (如 sensor/+/temperature 匹配 sensor/room1/temperature)
//   #  匹配剩余所有级别 (如 home/# 匹配 home/floor1/room/temp)
//   注意: # 必须是 filter 的最后一个字符
bool ModuleMqttBroker::TopicMatches(const std::string& filter, const std::string& topic) {
    // 按 '/' 切分 topic filter 和真实 topic
    auto split=[](const std::string& s)->std::vector<std::string>{
        std::vector<std::string> p; if(s.empty()) return p;
        size_t st=0; if(s[0]=='/') st=1;  // 跳过前导 '/'
        std::stringstream ss(s.substr(st)); std::string part;
        while(std::getline(ss,part,'/')) p.push_back(part);
        return p;
    };
    auto fp=split(filter), tp=split(topic);
    size_t fi=0,ti=0;
    while(fi<fp.size()) {
        if(fp[fi]=="#") return true;       // # 通配符 → 无条件匹配
        if(ti>=tp.size()) return false;    // filter 还有但 topic 已结束 → 不匹配
        if(fp[fi]!="+"&&fp[fi]!=tp[ti]) return false;  // 需要逐字匹配
        ++fi;++ti;
    }
    return ti==tp.size();  // filter 和 topic 同时结束才算完全匹配
}

// 遍历所有订阅, 找出 filter 匹配 topic 的订阅者
void ModuleMqttBroker::MatchSubscribers(const std::string& topic, std::vector<SubscriptionInfo>& out) {
    out.clear(); std::lock_guard<std::mutex> lk(m_mutexSubscribers);
    for(auto& p:m_mapTopicSubscribers) if(TopicMatches(p.first,topic))
        for(auto& s:p.second) out.push_back(s);
}

// Echo Demo: Init() 中设为 true, 默认开启
static bool g_bEchoEnabled = false;

ModuleMqttBroker::ModuleMqttBroker() {}; ModuleMqttBroker::~ModuleMqttBroker() {}
bool ModuleMqttBroker::Init() {
    g_bEchoEnabled = true;
    return true;
}
// HttpMsg 路径不处理 (MQTT 不走 HTTP)
bool ModuleMqttBroker::AnyMessage(const net::tagMsgShell&, const HttpMsg&) { return false; }

// ===== MQTT 消息总入口: 分路由到各 Handler ==================================
// Worker 解码 MQTT 包后 (CodecMqtt::Decode → MsgHead+MsgBody) 调用此函数。
// body 内是完整的 MQTT 包原始字节 (fixed header + variable header + payload),
// 各 Handler 自行解析 body 内容。
//
// cmd 逆运算: cmd = MQTT_CMD_BASE + t*2 - 1 → t = (cmd - MQTT_CMD_BASE + 1) / 2
//   例: CONNECT(t=1) → cmd=21001 → pt=(21001-21000+1)/2=1 ✓
bool ModuleMqttBroker::AnyMessage(const net::tagMsgShell& sh, const MsgHead& h, const MsgBody& b) {
    // 逆运算: 从 cmd 反算 MQTT 包类型编号
    uint8_t pt = static_cast<uint8_t>((h.cmd() - net::MQTT_CMD_BASE + 1) / 2);

    // KeepAlive 活性追踪: 除 CONNECT 外的所有包都更新最后活跃时间
    if (pt != net::MQTT_CONNECT) { auto it=m_mapKeepAlive.find(sh.iFd);
        if(it!=m_mapKeepAlive.end()) it->second.lastActivity = static_cast<double>(time(nullptr)); }

    // 按包类型分发到对应 Handler
    switch(pt) {
    case net::MQTT_CONNECT:     HandleConnect(sh,b);    break;
    case net::MQTT_SUBSCRIBE:   HandleSubscribe(sh,b);  break;
    case net::MQTT_UNSUBSCRIBE: HandleUnsubscribe(sh,b);break;
    case net::MQTT_PUBLISH:     HandlePublish(sh,b);    break;
    case net::MQTT_PUBACK:      HandlePuback(sh,b);     break;
    case net::MQTT_PINGREQ:     HandlePingreq(sh);      break;
    case net::MQTT_DISCONNECT:  HandleDisconnect(sh);   break;
    default: LOG4_WARN("MQTT unhandled type=%u",pt);    break;
    }
    return true;
}

// ===== CONNECT 处理器 ======================================================
// 解析 CONNECT 包 (MQTT 3.1.1 §3.1):
//   Fixed Header:  type=1, flags=0
//   Variable Header: Protocol Name("MQTT"), Protocol Level(4), Connect Flags, Keep Alive
//   Payload: ClientId, [Will Topic, Will Message], [Username], [Password]
//
// 处理:
//   1. 协议合法性校验: Protocol Name="MQTT", Protocol Level=4
//   2. 记录 clientId → fd 映射
//   3. 记录 KeepAlive 间隔 (后续用于心跳超时检测)
//   4. 解析 Will 遗嘱 (如果 Connect Flags 中 Will Flag=1):
//      Will 保存在 m_mapWill, 当连接异常断开时由 HandleUnexpectedDisconnect 发布
//   5. 返回 CONNACK ACCEPTED
void ModuleMqttBroker::HandleConnect(const net::tagMsgShell& sh, const MsgBody& b) {
    const std::string& r=b.body(); if(r.size()<2) return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(r.data()); size_t n=r.size();

    // Step 1: 跳过 Fixed Header, 定位到 Variable Header
    uint32_t rl; uint8_t hl=ParseHdrLen(p,n,rl);
    const uint8_t* v=p+hl; size_t vn=n-hl; if(vn<10) return;

    // Step 2: 读 Protocol Name 和 Protocol Level
    uint16_t pnl=ReadU16(v);  // Protocol Name Length (应为 4: "MQTT")
    if(pnl!=4||vn<2u+pnl+4) return;
    std::string pn(reinterpret_cast<const char*>(v+2),pnl);  // "MQTT"
    uint8_t pl=v[2+pnl];      // Protocol Level (应为 4 = MQTT 3.1.1)
    uint8_t cf=v[2+pnl+1];    // Connect Flags

    // 协议不符 → CONNACK 拒连
    if(pn!="MQTT"||pl!=4) {
        SendMqttPacket(sh,net::MQTT_CONNACK,
            std::string(1,0)+std::string(1,net::MQTT_CONNACK_REFUSED_PROTO_VER));
        return;
    }

    // Step 3: 读 Payload — ClientId
    const uint8_t* pload=v+2+pnl+4;  // 跳过 Variable Header (10 字节)
    size_t plen=vn-(2+pnl+4);        // Payload 剩余长度
    if(plen<2) {
        SendMqttPacket(sh,net::MQTT_CONNACK,
            std::string(1,0)+std::string(1,net::MQTT_CONNACK_REFUSED_ID_REJECTED));
        return;
    }
    uint16_t cidLen=ReadU16(pload);
    if(plen<2u+cidLen) return;
    std::string cid(reinterpret_cast<const char*>(pload+2),cidLen);
    m_mapFdToClientId[sh.iFd]=cid;  // fd → clientId 映射

    // Step 4: 记录 KeepAlive 间隔
    uint16_t ka=ReadU16(v+2+pnl+2);  // Keep Alive (秒)
    if(ka>0) {
        m_mapKeepAlive[sh.iFd]={ka,0.0};
        LOG4_INFO("MQTT CONNECT: %s keepAlive=%u",cid.c_str(),ka);
    }

    // Step 5: 解析 Will 遗嘱 (如果 Connect Flags bit2=1)
    bool hasWill=(cf>>2)&1;
    const uint8_t* pc=pload+2+cidLen;  // 跳过 ClientId
    size_t rem=plen-(2+cidLen);        // Payload 剩余
    if(hasWill&&rem>=2) {
        uint16_t wtl=ReadU16(pc); pc+=2; rem-=2;       // Will Topic
        if(rem>=wtl) {
            WillInfo w;
            w.topic.assign(reinterpret_cast<const char*>(pc),wtl);
            pc+=wtl; rem-=wtl;
            if(rem>=2) {
                uint16_t wml=ReadU16(pc); pc+=2; rem-=2;  // Will Message
                if(rem>=wml) {
                    w.message.assign(reinterpret_cast<const char*>(pc),wml);
                    w.qos=(cf>>3)&3;        // Will QoS
                    w.retain=(cf>>5)&1;     // Will Retain
                    std::lock_guard<std::mutex> lk(m_mutexWill);
                    m_mapWill[sh.iFd]=w;
                }
            }
        }
    }

    // Step 6: 返回 CONNACK ACCEPTED
    // CONNACK 格式: [0x20, 0x02, 0x00 (Session Present), 0x00 (Return Code)]
    SendMqttPacket(sh,net::MQTT_CONNACK,
        std::string(1,0)+std::string(1,net::MQTT_CONNACK_ACCEPTED));
}

// ===== SUBSCRIBE 处理器 ====================================================
// 解析 SUBSCRIBE 包 (MQTT 3.1.1 §3.8):
//   Variable Header: Packet Identifier (2 字节)
//   Payload: 多个 (Topic Filter + Requested QoS) 对
//
// 处理:
//   1. 遍历每个 topic filter, 注册订阅: m_mapTopicSubscribers[filter] += {fd,seq}
//      (同一连接可订阅多个 topic, 同一 topic 可有多个订阅者)
//   2. 对每个新订阅, 查 m_mapRetained 是否有匹配的保留消息, 有则立即投递
//   3. QoS downgrade: 协商 QoS = min(requestedQoS, brokerMaxQos)
//   4. 返回 SUBACK (PacketId + 每个 subscription 的 Granted QoS)
void ModuleMqttBroker::HandleSubscribe(const net::tagMsgShell& sh, const MsgBody& b) {
    const std::string& r=b.body(); if(r.size()<2) return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(r.data()); size_t n=r.size();
    uint32_t rl; uint8_t hl=ParseHdrLen(p,n,rl);
    const uint8_t* v=p+hl; size_t vn=n-hl;
    if(vn<2) return;
    uint16_t pid=ReadU16(v);  // Packet Identifier
    const uint8_t* c=v+2,*e=v+vn;

    // 遍历每个 (Topic Filter, Requested QoS) 对
    std::string rcs;  // Return Codes (Granted QoS)
    while(c+3<=e) {
        uint16_t tl=ReadU16(c); c+=2;            // Topic Filter Length
        if(c+tl+1>e) break;
        std::string tf(reinterpret_cast<const char*>(c),tl);  // Topic Filter
        c+=tl;
        uint8_t rq=*c++;                          // Requested QoS
        uint8_t gq=(rq<=m_ucMaxQos)?rq:m_ucMaxQos;  // QoS downgrade
        rcs.push_back(static_cast<char>(gq));

        // 注册订阅: 同一 filter 可以有多个订阅者
        {
            std::lock_guard<std::mutex> lk(m_mutexSubscribers);
            m_mapTopicSubscribers[tf].push_back({tf,(uint32_t)sh.iFd,sh.ulSeq});
        }

        // 投递保留消息 (如果有匹配的 Retained 消息)
        DeliverRetained(sh,tf);
    }

    // 返回 SUBACK
    std::string body; WriteU16(pid,body); body+=rcs;
    SendMqttPacket(sh,net::MQTT_SUBACK,body);
}

// ===== UNSUBSCRIBE 处理器 (P1.5) ============================================
// 解析 UNSUBSCRIBE 包 (MQTT 3.1.1 §3.10), 从订阅表移除
void ModuleMqttBroker::HandleUnsubscribe(const net::tagMsgShell& sh, const MsgBody& b) {
    const std::string& r=b.body(); if(r.size()<2) return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(r.data()); size_t n=r.size();
    uint32_t rl; uint8_t hl=ParseHdrLen(p,n,rl);
    const uint8_t* v=p+hl; size_t vn=n-hl; if(vn<2) return;
    uint16_t pid=ReadU16(v);
    const uint8_t* c=v+2,*e=v+vn;
    while(c+2<=e) { uint16_t tl=ReadU16(c); c+=2; if(c+tl>e) break;
        std::string tf(reinterpret_cast<const char*>(c),tl); c+=tl;
        { std::lock_guard<std::mutex> lk(m_mutexSubscribers); auto it=m_mapTopicSubscribers.find(tf);
            if(it!=m_mapTopicSubscribers.end()) { auto& subs=it->second;
                subs.erase(std::remove_if(subs.begin(),subs.end(),[&](const SubscriptionInfo& s){return s.fd==(uint32_t)sh.iFd;}),subs.end());
                if(subs.empty()) m_mapTopicSubscribers.erase(it); } }
        LOG4_INFO("MQTT UNSUBSCRIBE: fd=%d filter=%s",sh.iFd,tf.c_str());
    }
    std::string body; WriteU16(pid,body); SendMqttPacket(sh,net::MQTT_UNSUBACK,body);
}


// ===== PUBLISH 处理器 ======================================================
// 解析 PUBLISH 包 (MQTT 3.1.1 §3.3), 广播给匹配的订阅者。
//
// PUBLISH 包格式:
//   Fixed Header:  Type(3), DUP flag, QoS, Retain flag
//   Variable Header: Topic Name, [Packet Identifier (QoS>0 时)]
//   Payload: 消息内容
//
// 处理流程:
//   1. 解析 topic + payload + QoS + retain 标志
//   2. QoS 1 → 立即回 PUBACK (简单确认, 不落盘)
//   3. retain 标志 → 存入 m_mapRetained (空 payload = 清除保留消息)
//   4. MatchSubscribers(topic) 匹配订阅者 (支持通配符 + 和 #)
//   5. 遍历订阅者, 逐条发送 PUBLISH (排除发布者自身)
//   6. [可选] Echo Demo: echo/ping → echo/ping/response
void ModuleMqttBroker::HandlePublish(const net::tagMsgShell& sh, const MsgBody& b) {
    const std::string& r=b.body(); if(r.size()<2) return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(r.data()); size_t n=r.size();

    // Step 1: 解析 Fixed Header flags
    uint8_t fl=p[0]&0x0F;           // 低 4 位
    uint8_t qos=(fl>>1)&3;          // bits 1-2: QoS
    bool retain=fl&1;               // bit 0: Retain

    // Step 2: 跳过 Fixed Header, 定位 Variable Header
    uint32_t rl; uint8_t hl=ParseHdrLen(p,n,rl);
    const uint8_t* v=p+hl; size_t vn=n-hl;
    if(vn<2) return;

    // Step 3: 读 Topic Name (UTF-8 编码, 前面 2 字节为长度)
    uint16_t tl=ReadU16(v);         // Topic Length
    if(vn<2u+tl) return;
    std::string topic(reinterpret_cast<const char*>(v+2),tl);

    // Step 4: QoS>0 时读 Packet Identifier (2 字节, 跟在 Topic 之后)
    uint16_t pid=0; size_t po=2+tl; // payload offset
    if(qos>0&&vn>=po+2) { pid=ReadU16(v+po); po+=2; }

    // Step 5: 读 Payload (消息体)
    std::string msg(reinterpret_cast<const char*>(v+po),vn-po);

    // Step 6: QoS 1 → 立即回 PUBACK (简单确认, 不做持久化/重传)
    if(qos==1) SendPuback(pid,sh);

    // Step 7: Retain 处理: 存储保留消息 (空 payload = 清除)
    if(retain) {
        std::lock_guard<std::mutex> lk(m_mutexRetained);
        if(msg.empty()) m_mapRetained.erase(topic);
        else m_mapRetained[topic]={topic,msg,qos};
    }

    // Step 8: 匹配订阅者并广播
    std::vector<SubscriptionInfo> subs;
    MatchSubscribers(topic, subs);

    for(auto& s:subs) {
        if(s.fd==(uint32_t)sh.iFd&&s.seq==sh.ulSeq) continue;

        if (qos > 0) {
            // QoS 1 转发: body = TopicLen(2) + Topic + PacketId(2) + Payload
            uint16_t fwdPid = AllocPacketId();
            std::string qosBody;
            WriteU16(tl, qosBody);
            qosBody += topic;
            WriteU16(fwdPid, qosBody);
            qosBody += msg;

            {
                std::lock_guard<std::mutex> lk(m_mutexPending);
                m_mapPending[fwdPid] = {(int32)s.fd, s.seq, qosBody, fwdPid, time(nullptr)};
            }
            SendMqttPacket(net::tagMsgShell((int32)s.fd, s.seq),
                           net::MQTT_PUBLISH, qosBody, 2);  // seq=2 = QoS 1
        } else {
            // QoS 0 转发: body = TopicLen(2) + Topic + Payload
            std::string body;
            WriteU16(tl, body); body += topic; body += msg;
            SendMqttPacket(net::tagMsgShell((int32)s.fd, s.seq),
                           net::MQTT_PUBLISH, body);
        }
    }

    // ===== Echo Demo =====
    // 收到 echo/xxx → 广播 echo/xxx/response 给所有订阅 echo/+/response 的客户端
    if (g_bEchoEnabled && topic.size() > 5 && topic.compare(0, 5, "echo/") == 0)
    {
        std::string echoTopic = topic + "/response";
        uint16_t etl = static_cast<uint16_t>(echoTopic.size());

        std::vector<SubscriptionInfo> echoSubs;
        MatchSubscribers(echoTopic, echoSubs);
        for (auto& s : echoSubs)
        {
            if (qos > 0) {
                uint16_t echoPid = AllocPacketId();
                std::string echoBody;
                WriteU16(etl, echoBody); echoBody += echoTopic;
                WriteU16(echoPid, echoBody); echoBody += msg;
                {
                    std::lock_guard<std::mutex> lk(m_mutexPending);
                    m_mapPending[echoPid] = {(int32)s.fd, s.seq, echoBody, echoPid, time(nullptr)};
                }
                SendMqttPacket(net::tagMsgShell((int32)s.fd, s.seq),
                               net::MQTT_PUBLISH, echoBody, 2);
            } else {
                std::string echoBody;
                WriteU16(etl, echoBody); echoBody += echoTopic; echoBody += msg;
                SendMqttPacket(net::tagMsgShell((int32)s.fd, s.seq),
                               net::MQTT_PUBLISH, echoBody);
            }
        }
    }
}

// ===== PUBACK 处理器 =======================================================
// QoS 1 投递确认: 订阅者收到 QoS 1 PUBLISH 后回 PUBACK, Broker 清理 pending 记录。
void ModuleMqttBroker::HandlePuback(const net::tagMsgShell& sh, const MsgBody& b) {
    const std::string& r = b.body();
    if (r.size() < 4) return;  // fixed header(2) + packet_id(2)
    const uint8_t* p = reinterpret_cast<const uint8_t*>(r.data());
    uint32_t rl; uint8_t hl = ParseHdrLen(p, r.size(), rl);
    if (r.size() < hl + 2u) return;
    uint16_t pid = ReadU16(p + hl);
    {
        std::lock_guard<std::mutex> lk(m_mutexPending);
        m_mapPending.erase(pid);
    }
    LOG4_TRACE("MQTT PUBACK: fd=%d pid=%u", sh.iFd, pid);
}

// ===== PINGREQ 处理器 ======================================================
// 收到 PINGREQ → 立即回 PINGRESP。PINGREQ 无 payload, PINGRESP 也无 payload。
void ModuleMqttBroker::HandlePingreq(const net::tagMsgShell& sh) {
    SendMqttPacket(sh,net::MQTT_PINGRESP,"");
}

// ===== DISCONNECT 处理器 ===================================================
// 正常断连: 清理 fd→clientId 映射 + QoS 1 pending。Will 遗嘱不触发。
void ModuleMqttBroker::HandleDisconnect(const net::tagMsgShell& sh) {
    m_mapFdToClientId.erase(sh.iFd);
    CleanupPendingDeliveries(sh.iFd);
}

// ===== 意外断连处理 (Will 遗嘱) =============================================
// 由 Worker 在检测到连接异常断开时调用 (非正常 DISCONNECT)。
// 查找该 fd 在 CONNECT 时注册的 Will 遗嘱, 发布给匹配的订阅者。
// 如果 will.retain=true, 遗嘱消息也会存入 m_mapRetained。
void ModuleMqttBroker::HandleUnexpectedDisconnect(int32 fd) {
    CleanupPendingDeliveries(fd);
    WillInfo w;
    {
        std::lock_guard<std::mutex> lk(m_mutexWill);
        auto it=m_mapWill.find(fd);
        if(it==m_mapWill.end()) return;  // 无遗嘱
        w=it->second;
        m_mapWill.erase(it);             // 取出即删
    }

    // 构造 Will PUBLISH 包并广播
    std::string body;
    WriteU16(w.topic.size(),body);
    body+=w.topic;
    body+=w.message;
    std::vector<SubscriptionInfo> subs;
    MatchSubscribers(w.topic, subs);
    for(auto& s:subs) {
        SendMqttPacket(net::tagMsgShell((int32)s.fd,s.seq), net::MQTT_PUBLISH, body);
    }

    // 遗嘱消息同时为 Retain
    if(w.retain) {
        std::lock_guard<std::mutex> lk(m_mutexRetained);
        m_mapRetained[w.topic]={w.topic,w.message,w.qos};
    }
}

// ===== 保留消息投递 =========================================================
// 当新订阅者订阅 topic filter 时, 查询 m_mapRetained 中是否有匹配的保留消息,
// 有则立即投递给新订阅者 (MQTT 3.1.1 §3.3.1.3)。
void ModuleMqttBroker::DeliverRetained(const net::tagMsgShell& sh, const std::string& tf) {
    std::vector<RetainedMsg> m;
    {
        std::lock_guard<std::mutex> lk(m_mutexRetained);
        for(auto& p:m_mapRetained) {
            if(TopicMatches(tf, p.first))
                m.push_back(p.second);
        }
    }
    for(auto& rm:m) {
        std::string body;
        WriteU16(rm.topic.size(),body);
        body+=rm.topic;
        body+=rm.payload;
        SendMqttPacket(sh, net::MQTT_PUBLISH, body);
    }
}

// ===== 发送 MQTT 包 (底层) ==================================================
// qosFlags: 低 4 位直接写入 PUBLISH Fixed Header (bit0 retain, bit1-2 qos, bit3 dup)
//   0 = QoS 0, 2 = QoS 1, 10 = QoS 1 DUP
void ModuleMqttBroker::SendMqttPacket(const net::tagMsgShell& sh, uint8_t pt, const std::string& body, uint8_t qosFlags) {
    MsgHead h;
    h.set_cmd(net::MQTT_CMD(pt));
    h.set_seq(qosFlags);
    h.set_msgbody_len(body.size());
    MsgBody b;
    b.set_body(body);
    GetLabor()->SendTo(sh, h, b);
}

// 分配 MQTT Packet Identifier (1~65535 循环)
uint16_t ModuleMqttBroker::AllocPacketId() {
    uint16_t pid = m_uNextPacketId++;
    if (m_uNextPacketId == 0) m_uNextPacketId = 1;
    return pid;
}

// 清理某连接的 QoS 1 pending 投递
void ModuleMqttBroker::CleanupPendingDeliveries(int32 fd) {
    std::lock_guard<std::mutex> lk(m_mutexPending);
    for (auto it = m_mapPending.begin(); it != m_mapPending.end(); ) {
        if (it->second.fd == fd)
            it = m_mapPending.erase(it);
        else
            ++it;
    }
}

// 发送 PUBACK: PacketId(2 字节大端)
void ModuleMqttBroker::SendPuback(uint16_t pid, const net::tagMsgShell& sh) {
    std::string b; WriteU16(pid,b);
    SendMqttPacket(sh, net::MQTT_PUBACK, b);
}

// 主动关闭连接 (清理映射 + 通知 Worker 断开 TCP)
void ModuleMqttBroker::CloseConnection(const net::tagMsgShell& sh) {
    m_mapFdToClientId.erase(sh.iFd);
    GetLabor()->Disconnect(sh);
}

// ===== Echo Demo 接口 (保留用于外部 SO 动态开关) ====================
// Echo 已在 Init() 中默认开启, 此处保留接口供将来关闭用
extern "C" {
void MqttEchoEnable()  { g_bEchoEnabled = true; }
bool MqttEchoIsEnabled() { return g_bEchoEnabled; }
}
} // namespace mqtt
