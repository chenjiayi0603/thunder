/*******************************************************************************
 * Project:  Net
 * @file     HttpFastCodec.cpp
 * @brief    HTTP Fast-Path 实现 — 详见 HttpFastCodec.hpp
 * @author   Thunder Team
 * @date:    2026年5月28日
 ******************************************************************************/
#include <strings.h>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include "codec/HttpFastCodec.hpp"
#include "protocol/http.pb.h"
#include "util/CBuffer.hpp"
#include "util/StringCoder.hpp"
#include "util/http/http_parser.h"

namespace net
{

// ===================================================================
//  Decode Fast-Path
//
//  纯 C 库函数逐字节解析 HTTP/1.x 协议格式, 一次性顺序扫描,
//  零状态机开销, 不依赖 http_parser。
//
//  解析步骤:
//    method    → switch(methodLen) 首字母匹配 GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS
//    path      → SP1..SP2 之间, '?' 之前 (路由匹配用)
//    url       → SP1..SP2 全部 (含 query string, 日志/透传用)
//    params    → '?' 之后, util::DecodeParameter 解析 → HttpMsg.params
//    version   → p[5]-'0', p[7]-'0' 直接取值 (HTTP/{major}.{minor})
//    headers   → 只提取 Content-Length / Content-Encoding / Connection / Keep-Alive
//                (Worker 仅需这 4 个, 其余 header 跳过 — 省 protobuf map 插入开销)
//    body      → Content-Length 指定长度, protobuf set_body 拷贝
//
//  对比 http_parser 的 7 回调链 (OnMessageBegin→OnUrl→OnHeaderField→OnHeaderValue
//  →OnHeadersComplete→OnBody→OnMessageComplete), 每个字段/head/body 片段都触发
//  protobuf 操作 + 虚函数调用。Fast Path 是一次性扫描, 只在最终填充 HttpMsg。
//
//  回退 http_parser 的场景:
//    - Transfer-Encoding: chunked (分块解析复杂, http_parser 原生支持)
//    - CONNECT / TRACE method (极少使用)
//    - TCP 分包导致数据不全
//    - 未知 method
// ===================================================================

bool TryFastDecodeHttpRequest(const char* raw, size_t rawLen,
        HttpMsg& oHttpMsg, size_t& consumed)
{
    const char* p = raw;
    const char* const end = raw + rawLen;

    // ── 1. 请求行: METHOD SP PATH SP HTTP/1.x CRLF ──
    const char* sp1 = static_cast<const char*>(std::memchr(p, ' ', end - p));     // 找到 method 后的第一个空格
    if (!sp1 || sp1 >= end - 1) return false;

    size_t methodLen = sp1 - p;                                                   // method 长度 = 第一个SP - 行首
    switch (methodLen) {
    case 3:
        if      (p[0]=='G' && p[1]=='E' && p[2]=='T')    oHttpMsg.set_method(HTTP_GET);
        else if (p[0]=='P' && p[1]=='U' && p[2]=='T')    oHttpMsg.set_method(HTTP_PUT);
        else return false;
        break;
    case 4:
        if      (p[0]=='P' && p[1]=='O' && p[2]=='S' && p[3]=='T') oHttpMsg.set_method(HTTP_POST);
        else if (p[0]=='H' && p[1]=='E' && p[2]=='A' && p[3]=='D') oHttpMsg.set_method(HTTP_HEAD);
        else return false;
        break;
    case 5:
        if      (p[0]=='P' && p[1]=='A' && p[2]=='T' && p[3]=='C' && p[4]=='H') oHttpMsg.set_method(HTTP_PATCH);
        else if (p[0]=='T' && p[1]=='R' && p[2]=='A' && p[3]=='C' && p[4]=='E') return false;
        else return false;
        break;
    case 6:   if (p[0]=='D'&&p[1]=='E'&&p[2]=='L'&&p[3]=='E'&&p[4]=='T'&&p[5]=='E')
                  oHttpMsg.set_method(HTTP_DELETE); else return false; break;
    case 7:
        if      (p[0]=='O'&&p[1]=='P'&&p[2]=='T'&&p[3]=='I'&&p[4]=='O'&&p[5]=='N'&&p[6]=='S')
                  oHttpMsg.set_method(HTTP_OPTIONS);
        else if (p[0]=='C'&&p[1]=='O'&&p[2]=='N'&&p[3]=='N'&&p[4]=='E'&&p[5]=='C'&&p[6]=='T')
                  return false;
        else return false;
        break;
    default:  return false;
    }

    // path+query: url = 完整URL, path = 去掉?query
    p = sp1 + 1;                                                                  // 跳过第一个空格, p 指向 path 首字节
    const char* sp2 = static_cast<const char*>(std::memchr(p, ' ', end - p));     // 找到 path+query 后的第二个空格
    if (!sp2) return false;
    size_t urlLen = sp2 - p;                                                      // 完整 URL 长度 (含 query string)
    oHttpMsg.set_url(p, urlLen);
    const char* qm = static_cast<const char*>(std::memchr(p, '?', urlLen));       // path 中找 '?' 分隔符
    if (qm)
    {
        oHttpMsg.set_path(p, qm - p);                                             // path = 从起始到 '?' 之前
        std::string strQuery(qm + 1, sp2 - qm - 1);                               // query = '?' 之后到 SP 之前
        if (!strQuery.empty())
        {
            std::map<std::string, std::string> mapParam;
            util::DecodeParameter(strQuery, mapParam);
            for (auto& kv : mapParam)
                (*oHttpMsg.mutable_params())[kv.first] = std::move(kv.second);
        }
    }
    else
    {
        oHttpMsg.set_path(p, urlLen);
    }

    // HTTP/1.x
    p = sp2 + 1;                                                                  // 跳过第二个空格, p 指向 'H' (HTTP/x.y)
    if (p + 8 > end) return false;
    if (p[0]!='H' || p[1]!='T' || p[2]!='T' || p[3]!='P' || p[4]!='/' || p[6]!='.') return false;
    int major = p[5] - '0';                                                       // HTTP 主版本: 字符 '1' → 数字 1
    int minor = p[7] - '0';                                                       // HTTP 次版本: 字符 '1' → 数字 1

    if (major < 0 || major > 9 || minor < 0 || minor > 9) return false;           // 版本号范围校验
    oHttpMsg.set_http_major(major);
    oHttpMsg.set_http_minor(minor);

    p = static_cast<const char*>(std::memchr(p, '\n', end - p));                   // 跳过请求行剩余部分, 到 \n (行尾)
    if (!p || ++p >= end) return false;                                            // ++p 跳过 \n, 指向 headers 区域首字节

    // ── 2. headers ──
    bool   hasContentLength   = false;
    size_t contentLen         = 0;
    bool   isChunked          = false;
    bool   hasContentEncoding = false;
    std::string contentEncodingVal;
    std::string keepAliveVal, connectionVal;

    while (p + 3 < end)
    {
        if (p[0] == '\r' && p[1] == '\n') { p += 2; break; }

        if (static_cast<size_t>(end - p) >= 15
            && !strncasecmp(p, "content-length:", 15))
        {
            hasContentLength = true;
            p += 15;                                                              // 跳过 "content-length:"
            while (p < end && (*p == ' ' || *p == '\t')) ++p;                     // 跳过 : 后的空白
            contentLen = 0;
            while (p < end && *p >= '0' && *p <= '9') {                           // 逐字符累加十进制数字
                contentLen = contentLen * 10 + static_cast<size_t>(*p - '0');     // 乘10进位 + 新数字位
                ++p;
            }
            while (p < end && *p != '\n') ++p;                                    // 跳过本行剩余字符
            if (p < end) ++p;                                                     // 跳过 \n 到下一行首
        }
        else if (static_cast<size_t>(end - p) >= 18
            && !strncasecmp(p, "transfer-encoding:", 18))
        {
            const char* v = p + 18;
            while (v < end && (*v == ' ' || *v == '\t')) ++v;
            if (v + 7 < end && !strncasecmp(v, "chunked", 7)) { isChunked = true; break; }
            while (p < end && *p != '\n') ++p;
            if (p < end) ++p;
        }
        else if (static_cast<size_t>(end - p) >= 17
            && !strncasecmp(p, "content-encoding:", 17))
        {
            hasContentEncoding = true;
            p += 17;                                                              // 跳过 "content-encoding:"
            while (p < end && (*p == ' ' || *p == '\t')) ++p;                     // 跳过空白
            const char* ve = (p < end) ? static_cast<const char*>(std::memchr(p, '\r', static_cast<size_t>(end - p))) : nullptr;
            // ↑ 找 header value 末尾的 \r; p<end 防编译器有符号/无符号警告, 没找到用 end 兜底
            if (!ve) ve = end;
            contentEncodingVal.assign(p, static_cast<size_t>(ve - p));            // 复制 value 内容
            p = ve;
            while (p < end && *p != '\n') ++p;                                    // 跳过行尾, 到 \n
            if (p < end) ++p;                                                     // 跳过 \n 到下一行首
        }
        else if (static_cast<size_t>(end - p) >= 11
            && !strncasecmp(p, "connection:", 11))
        {
            p += 11;                                                              // 跳过 "connection:"
            while (p < end && (*p == ' ' || *p == '\t')) ++p;                     // 跳过空白
            const char* ve = (p < end) ? static_cast<const char*>(std::memchr(p, '\r', static_cast<size_t>(end - p))) : nullptr;
            // ↑ 找 value 末尾 \r; p<end 防编译器警告, 没找到用 end 兜底
            if (!ve) ve = end;
            connectionVal.assign(p, static_cast<size_t>(ve - p));
            p = ve;
            while (p < end && *p != '\n') ++p;                                    // 跳过行尾
            if (p < end) ++p;
        }
        else if (static_cast<size_t>(end - p) >= 11
            && !strncasecmp(p, "keep-alive:", 11))
        {
            p += 11;                                                              // 跳过 "keep-alive:"
            while (p < end && (*p == ' ' || *p == '\t')) ++p;                     // 跳过空白
            const char* ve = (p < end) ? static_cast<const char*>(std::memchr(p, '\r', static_cast<size_t>(end - p))) : nullptr;
            // ↑ 找 value 末尾 \r; p<end 防编译器警告, 没找到用 end 兜底
            if (!ve) ve = end;
            keepAliveVal.assign(p, static_cast<size_t>(ve - p));
            p = ve;
            while (p < end && *p != '\n') ++p;                                    // 跳过行尾
            if (p < end) ++p;
        }
        else
        {
            while (p < end && *p != '\n') ++p;                                    // 跳过不需要的 header 行
            if (p < end) ++p;                                                     // 跳过 \n 到下一行首
        }
    }

    if (isChunked) return false;                                                    // chunked 编码回退 http_parser

    // ── 3. body ──
    if (hasContentLength)
    {
        if (static_cast<size_t>(end - p) < contentLen) return false;               // body 数据不完整, 回退
        if (contentLen > 0) oHttpMsg.set_body(p, contentLen);                      // 拷贝 body 到 protobuf
        p += contentLen;                                                            // 推进指针经过 body
    }

    // ── 4. 填充 HttpMsg ──
    oHttpMsg.set_type(HTTP_REQUEST);
    oHttpMsg.set_is_decoding(false);
    if (!keepAliveVal.empty())
        (*oHttpMsg.mutable_headers())["Keep-Alive"] = keepAliveVal;
    if (!connectionVal.empty())
        (*oHttpMsg.mutable_headers())["Connection"] = connectionVal;
    if (hasContentEncoding && !contentEncodingVal.empty())
        (*oHttpMsg.mutable_headers())["Content-Encoding"] = contentEncodingVal;

    consumed = static_cast<size_t>(p - raw);                                        // 返回已消费字节数
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

}  // namespace net
