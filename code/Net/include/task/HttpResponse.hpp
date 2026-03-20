#pragma once

#include "protocol/http.pb.h"
#include <string>

namespace net {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    HttpMsg rawMsg;

    bool Ok() const { return statusCode >= 200 && statusCode < 300; }

    static HttpResponse FromHttpMsg(const HttpMsg& msg) {
        HttpResponse resp;
        resp.statusCode = msg.status_code();
        resp.body = msg.body();
        resp.rawMsg = msg;
        return resp;
    }
};

} // namespace net
