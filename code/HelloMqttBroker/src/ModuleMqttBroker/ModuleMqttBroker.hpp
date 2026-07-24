#ifndef HELLO_MQTT_BROKER_MODULE_HPP_
#define HELLO_MQTT_BROKER_MODULE_HPP_

#include "cmd/Module.hpp"
#include "codec/CodecMqtt.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace mqtt
{

class ModuleMqttBroker : public net::Module
{
public:
    ModuleMqttBroker(); virtual ~ModuleMqttBroker();
    virtual bool Init() override;
    virtual bool AnyMessage(const net::tagMsgShell&, const HttpMsg&) override;
    bool AnyMessage(const net::tagMsgShell&, const MsgHead&, const MsgBody&) override;

private:
    struct SubscriptionInfo { std::string topicFilter; uint32_t fd; uint32_t seq; };
    struct WillInfo { std::string topic; std::string message; uint8_t qos = 0; bool retain = false; };
    struct RetainedMsg { std::string topic; std::string payload; uint8_t qos = 0; };
    struct ConnKeepAlive { uint16_t interval = 0; double lastActivity = 0; };

    void HandleConnect(const net::tagMsgShell&, const MsgBody&);
    void HandleSubscribe(const net::tagMsgShell&, const MsgBody&);
    void HandleUnsubscribe(const net::tagMsgShell&, const MsgBody&);
    void HandlePublish(const net::tagMsgShell&, const MsgBody&);
    void HandlePuback(const net::tagMsgShell&, const MsgBody&);
    void HandlePingreq(const net::tagMsgShell&);
    void HandleDisconnect(const net::tagMsgShell&);
    void HandleUnexpectedDisconnect(int32 fd);
    void SendMqttPacket(const net::tagMsgShell&, uint8_t packetType, const std::string& body);
    void SendPuback(uint16_t packetId, const net::tagMsgShell&);
    void DeliverRetained(const net::tagMsgShell&, const std::string& topicFilter);
    void CloseConnection(const net::tagMsgShell&);
    void MatchSubscribers(const std::string& topic, std::vector<SubscriptionInfo>& outSubs);
    static bool TopicMatches(const std::string& filter, const std::string& topic);

    std::unordered_map<std::string, std::vector<SubscriptionInfo>> m_mapTopicSubscribers;
    std::mutex m_mutexSubscribers;
    std::unordered_map<int32, std::string> m_mapFdToClientId;
    std::unordered_map<int32, WillInfo> m_mapWill;
    std::mutex m_mutexWill;
    std::unordered_map<std::string, RetainedMsg> m_mapRetained;
    std::mutex m_mutexRetained;
    std::unordered_map<int32, ConnKeepAlive> m_mapKeepAlive;
    uint8_t m_ucMaxQos = 1;
};


// ========== Echo Demo 接口 (C linkage, 跨 SO 调用) ==========
extern "C" {
void MqttEchoEnable();
bool MqttEchoIsEnabled();
}
} // namespace mqtt
#endif
