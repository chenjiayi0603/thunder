#include "task/HttpTask.hpp"
#include "codec/HttpCodec.hpp"
#include "util/http/http_parser.h"

namespace net {

HttpTask::HttpTask(Step* pNextStep) : BaseTask(pNextStep) {}

HttpTask::HttpTask(const tagMsgShell& shell, const HttpMsg& httpMsg, Step* pNextStep)
    : BaseTask(shell, pNextStep) {
    m_oInHttpMsg = httpMsg;
}

HttpTask::HttpTask(const tagMsgShell& shell, const MsgHead& head, Step* pNextStep)
    : BaseTask(shell, head, pNextStep) {}

HttpTask::HttpTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body, Step* pNextStep)
    : BaseTask(shell, head, body, pNextStep) {}

bool HttpTask::HttpPost(const std::string& strUrl, const std::string& strBody,
                        const std::unordered_map<std::string, std::string>& mapHeaders) {
    HttpMsg oHttpMsg;
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(1);
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_method(HTTP_POST);
    oHttpMsg.set_url(strUrl);
    for (auto c_iter = mapHeaders.begin(); c_iter != mapHeaders.end(); ++c_iter) {
        oHttpMsg.mutable_headers()->insert(
            google::protobuf::MapPair<std::string, std::string>(c_iter->first, c_iter->second));
    }
    oHttpMsg.set_body(strBody);
    return HttpRequest(oHttpMsg);
}

bool HttpTask::HttpPost(const std::string& strUrl, const std::string& strBody) {
    HttpMsg oHttpMsg;
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(1);
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_method(HTTP_POST);
    oHttpMsg.set_url(strUrl);
    oHttpMsg.set_body(strBody);
    return HttpRequest(oHttpMsg);
}

bool HttpTask::HttpGet(const std::string& strUrl) {
    HttpMsg oHttpMsg;
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(1);
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_method(HTTP_GET);
    oHttpMsg.set_url(strUrl);
    return HttpRequest(oHttpMsg);
}

bool HttpTask::HttpRequest(const HttpMsg& oHttpMsg) {
    std::string strHost;
    std::string strPath;
    struct http_parser_url stUrl;
    if (0 == http_parser_parse_url(oHttpMsg.url().c_str(), oHttpMsg.url().length(), 0, &stUrl)) {
        int iPort = 0;
        if (stUrl.field_set & (1 << UF_PORT)) {
            iPort = stUrl.port;
        } else {
            iPort = 80;
        }

        if (stUrl.field_set & (1 << UF_HOST)) {
            strHost = oHttpMsg.url().substr(stUrl.field_data[UF_HOST].off, stUrl.field_data[UF_HOST].len);
        }

        if (stUrl.field_set & (1 << UF_PATH)) {
            strPath = oHttpMsg.url().substr(stUrl.field_data[UF_PATH].off, stUrl.field_data[UF_PATH].len);
        }

        return GetLabor()->SentTo(strHost, iPort, strPath, oHttpMsg, this);
    }
    LOG4_ERROR("http_parser_parse_url \"%s\" error!", oHttpMsg.url().c_str());
    return false;
}

bool HttpTask::SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg) {
    return GetLabor()->SendTo(stMsgShell, oHttpMsg, this);
}

HttpGetAwaiter HttpTask::HttpGetAsync(const std::string& url) { return HttpGetAwaiter(this, url); }

HttpPostAwaiter HttpTask::HttpPostAsync(const std::string& url, const std::string& body) {
    return HttpPostAwaiter(this, url, body, {});
}

HttpPostAwaiter HttpTask::HttpPostAsync(const std::string& url, const std::string& body,
                                        const std::unordered_map<std::string, std::string>& headers) {
    return HttpPostAwaiter(this, url, body, headers);
}

} // namespace net
