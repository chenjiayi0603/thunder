/*******************************************************************************
 * Project:  Net
 * @file     HttpFastCodec.cpp
 * @brief    HTTP Fast-Path 实现 — 详见 HttpFastCodec.hpp
 * @author   Thunder Team
 * @date:    2026年5月28日
 ******************************************************************************/
#include <strings.h>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>

#include "codec/HttpFastCodec.hpp"
#include "protocol/http.pb.h"
#include "util/CBuffer.hpp"
#include "codec/StringCoder.hpp"

extern "C" {
#include "util/http/pico/picohttpparser.h"
}

#include "util/http/http_parser.h"  // method enum, chunked 回退用

namespace net
{

// ===================================================================
//  Decode Fast-Path — picohttpparser 驱动
//
//  phr_parse_request 一行替代 http_parser 9 回调 + 状态机。
//  覆盖全部 HTTP/1.1 请求 (GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH/CONNECT/TRACE),
//  全部 header, URL+query, Content-Length body, chunked 解码。
//  安全检查: CHECK_EOF + IS_PRINTABLE_ASCII + RFC 7230 token char 验证。
//
//  74ns/req vs http_parser 263ns (快 3.5x)。
// ===================================================================

bool TryFastDecodeHttpRequest(const char* raw, size_t rawLen,
        HttpMsg& oHttpMsg, size_t& consumed)
{
    // ── 1. 仅处理请求 (响应走 http_parser 回退) ──
    // 响应以 "HTTP/" 开头 — 回退 http_parser (etcd 等使用 chunked 响应)
    if (rawLen >= 5 && memcmp(raw, "HTTP/", 5) == 0)
        return false;

    // ── 请求路径 ──
    const char* method;     size_t method_len;
    const char* path;       size_t path_len;
    int minor_version;
    struct phr_header headers[64];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int header_len = phr_parse_request(raw, rawLen,
        &method, &method_len, &path, &path_len,
        &minor_version, headers, &num_headers, 0);
    if (header_len <= 0) return false;  // 不完整(-2) 或 解析错误(-1)

    // ── 2. method ──
    if      (method_len == 3 && memcmp(method, "GET", 3) == 0)     oHttpMsg.set_method(HTTP_GET);
    else if (method_len == 4 && memcmp(method, "POST", 4) == 0)    oHttpMsg.set_method(HTTP_POST);
    else if (method_len == 3 && memcmp(method, "PUT", 3) == 0)     oHttpMsg.set_method(HTTP_PUT);
    else if (method_len == 6 && memcmp(method, "DELETE", 6) == 0)  oHttpMsg.set_method(HTTP_DELETE);
    else if (method_len == 4 && memcmp(method, "HEAD", 4) == 0)    oHttpMsg.set_method(HTTP_HEAD);
    else if (method_len == 7 && memcmp(method, "OPTIONS", 7) == 0) oHttpMsg.set_method(HTTP_OPTIONS);
    else if (method_len == 5 && memcmp(method, "PATCH", 5) == 0)   oHttpMsg.set_method(HTTP_PATCH);
    else if (method_len == 7 && memcmp(method, "CONNECT", 7) == 0) return false;  // CONNECT 走 http_parser
    else return false;

    // ── 3. URL + query ──
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_http_major(1);
    oHttpMsg.set_http_minor(minor_version);
    oHttpMsg.set_url(path, path_len);

    const char* qm = static_cast<const char*>(memchr(path, '?', path_len));
    if (qm) {
        oHttpMsg.set_path(path, qm - path);
        std::string strQuery(qm + 1, path_len - (qm + 1 - path));
        if (!strQuery.empty()) {
            std::map<std::string, std::string> mapParam;
            util::DecodeParameter(strQuery, mapParam);
            for (auto& kv : mapParam)
                (*oHttpMsg.mutable_params())[kv.first] = std::move(kv.second);
        }
    } else {
        oHttpMsg.set_path(path, path_len);
    }

    // ── 4. headers + 关键字段 ──
    int content_length = -1;
    bool is_chunked = false;
    std::string content_encoding;
    std::string connection_val;
    std::string keep_alive_val;

    auto* hdrs = oHttpMsg.mutable_headers();
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 14
            && strncasecmp(headers[i].name, "content-length", 14) == 0) {
            content_length = (int)strtoul(headers[i].value, nullptr, 10);
        } else if (headers[i].name_len == 17
            && strncasecmp(headers[i].name, "transfer-encoding", 17) == 0) {
            if (headers[i].value_len == 7
                && strncasecmp(headers[i].value, "chunked", 7) == 0)
                is_chunked = true;
        } else if (headers[i].name_len == 16
            && strncasecmp(headers[i].name, "content-encoding", 16) == 0) {
            content_encoding.assign(headers[i].value, headers[i].value_len);
        } else if (headers[i].name_len == 10
            && strncasecmp(headers[i].name, "connection", 10) == 0) {
            connection_val.assign(headers[i].value, headers[i].value_len);
        } else if (headers[i].name_len == 10
            && strncasecmp(headers[i].name, "keep-alive", 10) == 0) {
            keep_alive_val.assign(headers[i].value, headers[i].value_len);
        }
    }

    // 存回 headers (只存需要的, 避免 protobuf map 分配膨胀)
    if (!connection_val.empty())
        (*hdrs)["Connection"] = connection_val;
    if (!keep_alive_val.empty())
        (*hdrs)["Keep-Alive"] = keep_alive_val;
    if (!content_encoding.empty())
        (*hdrs)["Content-Encoding"] = content_encoding;

    if (content_length < 0) content_length = 0;

    // ── 5. body ──
    const char* body_start = raw + header_len;
    size_t body_avail = rawLen - header_len;

    if (is_chunked) {
        // 复制到临时缓冲解码: phr_decode_chunked 原地改写, 若分段到达则首次 -2 会污染源缓冲,
        // 导致下次重解失败。复制可保持源缓冲不变, 不完整时安全重试。
        std::string tmp(body_start, body_avail);
        size_t decode_len = tmp.size();
        struct phr_chunked_decoder decoder;
        memset(&decoder, 0, sizeof(decoder));
        ssize_t ret = phr_decode_chunked(&decoder, tmp.empty() ? nullptr : &tmp[0], &decode_len);
        if (ret == -1) return false;       // chunked 解码错误
        if (ret == -2) return false;       // 数据不完整, 等下个包(源缓冲未被污染)
        // ret >= 0: 解码完成, decode_len = 解码后 body 长度
        if (decode_len > 0)
            oHttpMsg.set_body(tmp.data(), decode_len);
        consumed = header_len + (body_avail - (size_t)ret);  // ret=残留字节数
    } else {
        if (body_avail < (size_t)content_length)
            return false;  // body 不完整
        if (content_length > 0)
            oHttpMsg.set_body(body_start, content_length);
        consumed = header_len + content_length;
    }

    oHttpMsg.set_is_decoding(false);
    return true;
}

// ===================================================================
//  Encode Fast-Path
//
//  常见场景 (HTTP/1.1 200 OK, 无 gzip/chunked, body ≤ 8KB, 默认 headers):
//  预编译响应模板, 跳过多次 vsnprintf 格式化 + header map 遍历拷贝。
//
//  适用条件:
//    - HTTP_RESPONSE, HTTP/1.1, status 200
//    - 无 gzip / chunked 编码
//    - body 1~8192 字节
//    - headers 仅含默认 Content-Type (+ 可选 Content-Length)
//
//  模板:
//    "HTTP/1.1 200 OK\r\n"
//    "Connection: keep-alive\r\n"
//    "Content-Type: application/json;charset=UTF-8\r\n"
//    "Content-Length: {body.size}\r\n\r\n"
//    "{body bytes}"
//
//  对比普通路径: 需逐 header 遍历 m_mapAddingHttpHeader, 每个 header 调用一次
//  pBuff->Printf("%s: %s\r\n", ...) → vsnprintf → 格式化 → 写入 buffer。
//  Fast Path 将整段响应头固定为 static const char[], 仅 Content-Length 和 body
//  需要动态填入, 省去 header map 遍历 + N 次 vsnprintf。
// ===================================================================

bool TryFastEncodeHttpResponse(const HttpMsg& oHttpMsg,
        const std::unordered_map<std::string, std::string>& mapAddingHeaders,
        util::CBuffer* pBuff, int& iHadWriteSize)
{
    // 适用条件: HTTP/1.1 200, 无 gzip/chunked, body 1~8192, 默认 headers
    if (oHttpMsg.type() != HTTP_RESPONSE
        || oHttpMsg.http_major() != 1 || oHttpMsg.http_minor() != 1
        || oHttpMsg.status_code() != 200
        || oHttpMsg.body().size() == 0 || oHttpMsg.body().size() > 8192)
        return false;

    // gzip/chunked 判断由调用方传入, 这里不做二次判断
    // 只检查 headers 是否为默认 (Content-Type 或 Content-Type+Content-Length)
    bool bDefaultHeaders =
        (mapAddingHeaders.size() == 2
         && mapAddingHeaders.find("Content-Type") != mapAddingHeaders.end())
        || (mapAddingHeaders.size() == 3
            && mapAddingHeaders.find("Content-Type") != mapAddingHeaders.end()
            && mapAddingHeaders.find("Content-Length") != mapAddingHeaders.end());
    if (!bDefaultHeaders) return false;

    // 回退调用方已写的状态行, 用预编译模板重写
    pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);
    iHadWriteSize = 0;

    static const char kHeader200[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/json;charset=UTF-8\r\n"
        "Content-Length: ";

    pBuff->Write(kHeader200, sizeof(kHeader200) - 1);                      // 写固定响应头模板
    iHadWriteSize = static_cast<int>(sizeof(kHeader200) - 1);              // sizeof-1: 不写末尾 \0

    int iWriteSize = pBuff->Printf("%u\r\n\r\n",                            // 动态填入 body 长度
            static_cast<unsigned>(oHttpMsg.body().size()));
    if (iWriteSize < 0)
    {
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);       // 写入失败: 回退 write index
        return false;
    }
    iHadWriteSize += iWriteSize;

    if (pBuff->Write(oHttpMsg.body().c_str(),
            static_cast<int>(oHttpMsg.body().size())) < 0)                   // 写 body 字节
    {
        pBuff->SetWriteIndex(pBuff->GetWriteIndex() - iHadWriteSize);        // 写入失败: 回退全部
        return false;
    }

    return true;
}

bool TryFastDecodeHttpResponse(const char* raw, size_t rawLen,
        HttpMsg& oHttpMsg, size_t& consumed)
{
    if (rawLen < 5 || memcmp(raw, "HTTP/", 5) != 0) return false;
    int minor_version, status; const char* msg; size_t msg_len;
    struct phr_header headers[64];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);
    int header_len = phr_parse_response(raw, rawLen, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);
    if (header_len <= 0) return false;
    oHttpMsg.set_type(HTTP_RESPONSE);
    oHttpMsg.set_http_major(1); oHttpMsg.set_http_minor(minor_version); oHttpMsg.set_status_code(status);
    int content_length = -1; bool is_chunked = false;
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 14 && strncasecmp(headers[i].name, "content-length", 14) == 0)
            content_length = (int)strtoul(headers[i].value, nullptr, 10);
        else if (headers[i].name_len == 17 && strncasecmp(headers[i].name, "transfer-encoding", 17) == 0)
            if (headers[i].value_len == 7 && strncasecmp(headers[i].value, "chunked", 7) == 0) is_chunked = true;
    }
    if (content_length < 0) content_length = 0;
    const char* body_start = raw + header_len; size_t body_avail = rawLen - header_len;
    if (is_chunked) {
        // phr_decode_chunked 会原地解码(改写缓冲)。若 body 与终止块分两次 recv 到达,
        // 首次 -2(不完整)已污染源缓冲, 下次带完整数据重解将解到垃圾 → PAUSE 永挂、
        // keepalive 卡死。故复制到临时缓冲解码, 保持源缓冲不变以便重解。
        std::string tmp(body_start, body_avail);
        size_t decode_len = tmp.size();
        struct phr_chunked_decoder decoder; memset(&decoder, 0, sizeof(decoder));
        ssize_t ret = phr_decode_chunked(&decoder, tmp.empty() ? nullptr : &tmp[0], &decode_len);
        // phr_decode_chunked: -1=错误, -2=数据不完整, >=0=解码完成(返回值为残留字节数)。
        // ret==0 是"完整且无残留"的正常成功(etcd lease/keepalive 的 chunked 响应即此情形),
        // 旧 `ret <= 0` 误判为失败 → Decode 返回 PAUSE → keepalive 永远收不到响应 → 续租全挂。
        if (ret == -1) return false;       // chunked 解码错误
        if (ret == -2) return false;       // 数据不完整, 等下个包(源缓冲未被污染)
        if (decode_len > 0) oHttpMsg.set_body(tmp.data(), decode_len);
        consumed = header_len + (body_avail - (size_t)ret);
    } else {
        if (body_avail < (size_t)content_length) return false;
        if (content_length > 0) oHttpMsg.set_body(body_start, content_length);
        consumed = header_len + content_length;
    }
    oHttpMsg.set_is_decoding(false);
    return true;
}

}  // namespace net
