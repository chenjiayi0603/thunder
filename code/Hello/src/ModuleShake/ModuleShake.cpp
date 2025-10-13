/*******************************************************************************
 * Project:  WebServer
 * @file     ModuleHello.cpp
 * @brief 	 用于支持websocket的握手协议
 * @author   cjy
 * @date:    2016年10月31日
 * @note
 * Modify history:
 ******************************************************************************/
#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <time.h>
#include "ModuleShake.hpp"
#include "cryptopp/default.h"
#include "cryptopp/cryptlib.h"
#include "cryptopp/base64.h"
#include "cryptopp/sha.h"
#include "cryptopp/hex.h"
#include "cryptopp/aes.h"
#include "cryptopp/modes.h"
#include "cryptopp/filters.h"
#include "cryptopp/md5.h"
#include "cryptopp/des.h"
#include "ProtoError.h"

MUDULE_CREATE(im::ModuleShake);

namespace im
{

bool ModuleShake::Init()
{
    if (m_boInit)
    {
        return true;
    }
    HelloSession* pSession = GetHelloSession();
    if(!pSession)
    {
        LOG4_ERROR("failed to get GetNodeSession");
        return false;
    }
    m_boInit = true;
    return true;
}

bool ModuleShake::AnyMessage(
                const net::tagMsgShell& stMsgShell,
                const HttpMsg& oInHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    ws_req_t ws_req;
    if(!ParseWebsocketHandshake(oInHttpMsg,ws_req))
    {
        LOG4_TRACE("%s() failed to ParseWebsocketHandshake", __FUNCTION__);
        return ResponseHttp(stMsgShell,oInHttpMsg,1000,"failed to ParseWebsocketHandshake");
    }
    if(strcasecmp(ws_req.upgrade.c_str(),"websocket") == 0)//websocket升级握手消息
    {
        LOG4_TRACE("ParseWebsocketHandshake ok");
        return ResponseWebsocketResponseHandshake(stMsgShell,ws_req,oInHttpMsg);
    }
    else//普通的http消息
    {
        LOG4_TRACE("ParseWebsocketHandshake other msg");
        return ResponseHttp(stMsgShell,oInHttpMsg,1000,"ParseWebsocketHandshake other msg");
    }
    return (true);
}

bool ModuleShake::ResponseWebsocketResponseHandshake(const net::tagMsgShell& stMsgShell,const ws_req_t &ws_req,
                const HttpMsg& oInHttpMsg)
{
    LOG4_TRACE("WebsocketResponseHandshake");
    HttpMsg oOutHttpMsg;
    oOutHttpMsg.set_type(HTTP_RESPONSE);
    oOutHttpMsg.set_status_code(101);
    oOutHttpMsg.set_http_major(oInHttpMsg.http_major());
    oOutHttpMsg.set_http_minor(oInHttpMsg.http_minor());
    oOutHttpMsg.set_url("Switching Protocols");
    //Connection
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Connection", "upgrade"));
    //Upgrade
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Upgrade", "websocket"));
    //Sec-WebSocket-Accept
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Sec-WebSocket-Accept", GenerateKey(ws_req.sec_websocket_key)));
    //Access-Control-Allow-Credentials//是否允许请求带有验证信息
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Credentials", "true"));
    //Access-Control-Allow-Headers//允许自定义的头部
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Headers", "content-type"));
    //Access-Control-Allow-Origin//允许任何来自任意域的跨域请求,需要以后控制
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Access-Control-Allow-Headers", "*"));
    //Server
    oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Server", "WebSocketServer"));
    //Origin
    if (!ws_req.origin.empty())//普通的HTTP请求也会带有，在CORS中专门作为Origin信息供后端比对,表明来源域
    {
    	oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Origin", ws_req.origin));
    }
    //Sec-WebSocket-Protocol
    if (!ws_req.sec_webSocket_protocol.empty())
    {
    	oOutHttpMsg.mutable_headers()->insert(google::protobuf::MapPair<std::string, std::string>("Sec-WebSocket-Protocol", ws_req.sec_webSocket_protocol));
    }
    LOG4_TRACE("%s", ToString(oOutHttpMsg).c_str());
    return GetLabor()->SendTo(stMsgShell, oOutHttpMsg);
}

std::string ModuleShake::GenerateKey(const std::string &key)//https://blog.csdn.net/ecrisraul/article/details/88042012
{
    //sha-1
    std::string text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
//    unsigned char digest[20] = {0};
//    CryptoPP::SHA1((const unsigned char*)tmp.c_str(), tmp.length(), digest);
//    //base64 encode
//    char szToBase64[128] = {0};
//    Base64encode(szToBase64, (const char*)digest, 20);
//    return std::string(szToBase64);
	std::string hash;
	CryptoPP::SHA1 sha1;
	CryptoPP::HashFilter hashfilter(sha1);
	hashfilter.Attach(new CryptoPP::HexEncoder(new CryptoPP::StringSink(hash), false));
	hashfilter.Put(reinterpret_cast<const unsigned char*>(text.c_str()), text.length());
	hashfilter.MessageEnd();
	return hash;
}

const std::string& ModuleShake::ToString(const HttpMsg& oHttpMsg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    m_strHttpString.clear();
    char prover[16];
    sprintf(prover, "HTTP/%u.%u", oHttpMsg.http_major(), oHttpMsg.http_minor());

    if (HTTP_REQUEST == oHttpMsg.type())
    {
        if (HTTP_POST == oHttpMsg.method() || HTTP_GET == oHttpMsg.method())
        {
            m_strHttpString += http_method_str((http_method)oHttpMsg.method());
            m_strHttpString += " ";
        }
        m_strHttpString += oHttpMsg.url();
        m_strHttpString += " ";
        m_strHttpString += prover;
        m_strHttpString += "\r\n";
    }
    else
    {
        m_strHttpString += prover;
        if (oHttpMsg.status_code())
        {
            char tmp[10];
            sprintf(tmp, " %u\r\n", oHttpMsg.status_code());
            m_strHttpString += tmp;
        }
    }
    for (auto iter = oHttpMsg.headers().begin(); iter != oHttpMsg.headers().end(); ++iter)
    {
        m_strHttpString += iter->first;
        m_strHttpString += ":";
        m_strHttpString += iter->second;
        m_strHttpString += "\r\n";
    }
    m_strHttpString += "\r\n";

    if (oHttpMsg.body().size())
    {
        m_strHttpString += oHttpMsg.body();
        m_strHttpString += "\r\n\r\n";
    }
    return(m_strHttpString);
}

bool ModuleShake::ParseWebsocketHandshake(HttpMsg oInHttpMsg,ws_req_t &ws_req)
{
    /*最初的握手http包，如：
    GET /chat HTTP/1.1
    Host: server.example.com
    Upgrade: websocket
    Connection: Upgrade
    Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
    Origin: http://example.com
    Sec-WebSocket-Protocol: chat, superchat
    Sec-WebSocket-Version: 13
    * */
    for (auto iter = oInHttpMsg.headers().end(); iter != oInHttpMsg.headers().end(); ++iter)
    {
        if (std::string("Connection") == iter->first)
        {
            ws_req.connection = iter->second;
        }
        else if (std::string("Upgrade") == iter->first)
        {
            ws_req.upgrade = iter->second;
        }
        else if (std::string("Host") == iter->first)
        {
            ws_req.host = iter->second;
        }
        else if (std::string("Origin") == iter->first)
        {
            ws_req.origin = iter->second;
        }
        else if (std::string("Cookie") == iter->first)
        {
            ws_req.cookie = iter->second;
        }
        else if (std::string("Sec-WebSocket-Key") == iter->first)
        {
            ws_req.sec_websocket_key = iter->second;
        }
        else if (std::string("Sec-WebSocket-Version") == iter->first)
        {
            ws_req.sec_websocket_version = iter->second;
        }
        else if (std::string("Sec-WebSocket-Protocol") == iter->first)
        {
            ws_req.sec_webSocket_protocol = iter->second;
        }
    }
    return true;
}
bool ModuleShake::ResponseHttp(const net::tagMsgShell& stMsgShell, const HttpMsg& oInHttpMsg,
                int iCode,const std::string &msg)
{
    LOG4_TRACE("%s()", __FUNCTION__);
    HttpMsg oOutHttpMsg;
    oOutHttpMsg.set_type(HTTP_RESPONSE);
    oOutHttpMsg.set_method(HTTP_POST);
    oOutHttpMsg.set_status_code(core::server_err_code(iCode));
    oOutHttpMsg.set_http_major(oInHttpMsg.http_major());
    oOutHttpMsg.set_http_minor(oInHttpMsg.http_minor());
    oOutHttpMsg.set_body(msg);
    GetLabor()->SendTo(stMsgShell, oOutHttpMsg);
    return(true);
}





} /* namespace core */
