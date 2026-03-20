#pragma once

#include "task/BaseTask.hpp"
#include "protocol/http.pb.h"
#include "codec/HttpCodec.hpp"
#include "task/HttpResponse.hpp"
#include "task/HttpAwaiter.hpp"
#include <string>
#include <unordered_map>

namespace net {

class HttpGetAwaiter;
class HttpPostAwaiter;

class HttpTask : public BaseTask {
public:
    HttpTask(Step* pNextStep = nullptr);
    HttpTask(const tagMsgShell& shell, const HttpMsg& httpMsg, Step* pNextStep = nullptr);
    HttpTask(const tagMsgShell& shell, const MsgHead& head, Step* pNextStep = nullptr);
    HttpTask(const tagMsgShell& shell, const MsgHead& head, const MsgBody& body, Step* pNextStep = nullptr);
    ~HttpTask() override = default;

    bool HttpGet(const std::string& strUrl);
    bool HttpPost(const std::string& strUrl, const std::string& strBody);
    bool HttpPost(const std::string& strUrl, const std::string& strBody,
                  const std::unordered_map<std::string, std::string>& mapHeaders);
    bool HttpRequest(const HttpMsg& oHttpMsg);
    bool SendTo(const tagMsgShell& stMsgShell, const HttpMsg& oHttpMsg);

    HttpGetAwaiter HttpGetAsync(const std::string& url);
    HttpPostAwaiter HttpPostAsync(const std::string& url, const std::string& body);
    HttpPostAwaiter HttpPostAsync(const std::string& url, const std::string& body,
                                  const std::unordered_map<std::string, std::string>& headers);
};

} // namespace net
