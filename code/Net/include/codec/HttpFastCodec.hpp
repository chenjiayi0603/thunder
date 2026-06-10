/*******************************************************************************
 * Project:  Net
 * @file     HttpFastCodec.hpp
 * @brief    HTTP Fast-Path (Decode + Encode) — 跳过 http_parser/vsnprintf 开销
 * @author   Thunder Team
 * @date:    2026年5月28日
 ******************************************************************************/
#ifndef SRC_CODEC_HTTPFASTCODEC_HPP_
#define SRC_CODEC_HTTPFASTCODEC_HPP_

#include <cstddef>
#include <string>
#include <unordered_map>

class HttpMsg;
namespace util { class CBuffer; }

namespace net {

// ========== Decode ==========

/**
 * @brief HTTP 请求快速解码
 *
 * 直接解析 METHOD /path HTTP/1.x + Content-Length + body,
 * 跳过 http_parser 的 7 回调链, 覆盖所有常规 GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS 请求。
 *
 * @return true  解析成功
 * @return false 需回退 http_parser (chunked, CONNECT, TRACE, 数据不全等)
 */
bool TryFastDecodeHttpRequest(const char* raw, size_t rawLen,
        HttpMsg& oHttpMsg, size_t& consumed);

bool TryFastDecodeHttpResponse(const char* raw, size_t rawLen,
        HttpMsg& oHttpMsg, size_t& consumed);

// ========== Encode ==========

/**
 * @brief HTTP 响应快速编码
 *
 * 对 HTTP/1.1 200 OK 场景, 用预编译模板直接写入响应头+body,
 * 跳过多次 vsnprintf 格式化 + header map 遍历拷贝。
 *
 * 适用条件: 无 gzip/chunked, body 1~8192 字节, headers 仅含默认 Content-Type。
 *
 * @return true  编码成功，pBuff 已写入完整响应
 * @return false 不满足条件，调用方继续走普通路径
 */
bool TryFastEncodeHttpResponse(const HttpMsg& oHttpMsg,
        const std::unordered_map<std::string, std::string>& mapAddingHeaders,
        util::CBuffer* pBuff, int& iHadWriteSize);

}  // namespace net

#endif /* SRC_CODEC_HTTPFASTCODEC_HPP_ */
