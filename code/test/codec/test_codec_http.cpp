/**
 * HttpCodec 编解码 E2E 测试 + FastPath 单元测试
 */
#include "codec/HttpCodec.hpp"
#include "codec/HttpFastCodec.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>
#include <cstdio>

// ========== HttpMsg Encode/Decode 往返 ==========

TEST(HttpCodec, Construction)
{
    net::HttpCodec codec(util::CODEC_HTTP);
    (void)codec;
    SUCCEED();
}

TEST(HttpCodec, EncodeHttpRequestDecodeRoundTrip)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_GET);
    req.set_url("http://127.0.0.1:8080/api/test");
    req.set_http_major(1);
    req.set_http_minor(1);
    (*req.mutable_headers())["Accept"] = "application/json";

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(req, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    HttpMsg decoded;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
    EXPECT_EQ(HTTP_REQUEST, decoded.type());
    EXPECT_EQ(HTTP_GET, decoded.method());
    EXPECT_EQ(1u, decoded.http_major());
    EXPECT_EQ(1u, decoded.http_minor());
}

TEST(HttpCodec, EncodeHttpResponseDecodeRoundTrip)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg resp;
    resp.set_type(HTTP_RESPONSE);
    resp.set_status_code(200);
    resp.set_http_major(1);
    resp.set_http_minor(1);
    (*resp.mutable_headers())["Content-Type"] = "text/plain";
    resp.set_body("OK");

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(resp, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    HttpMsg decoded;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
    EXPECT_EQ(HTTP_RESPONSE, decoded.type());
    EXPECT_EQ(200u, decoded.status_code());
    EXPECT_EQ("OK", decoded.body());
}

TEST(HttpCodec, EncodeHttpPostWithJsonBody)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_POST);
    req.set_url("http://127.0.0.1:9090/data");
    req.set_http_major(1);
    req.set_http_minor(1);
    (*req.mutable_headers())["Content-Type"] = "application/json";
    req.set_body(R"({"key":"value"})");

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(req, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);

    HttpMsg decoded;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
    EXPECT_EQ(HTTP_POST, decoded.method());
    EXPECT_EQ(R"({"key":"value"})", decoded.body());
}

TEST(HttpCodec, DecodeTruncatedPauses)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_GET);
    req.set_url("http://127.0.0.1/x");
    req.set_http_major(1);
    req.set_http_minor(1);

    util::CBuffer buf;
    codec.Encode(req, &buf);

    util::CBuffer partial;
    buf.Read(&partial, 5);

    HttpMsg decoded;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&partial, decoded));
}

// pico: 非法输入安全返回 PAUSE, 不崩溃
TEST(HttpCodec, DecodeCorruptedReturnsPause)
{
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    char garbage[6] = {'x', 'x', 'x', 'x', 'x', 'x'};
    buf.Write(garbage, 6);

    HttpMsg dh;
    EXPECT_EQ(codec.Decode(&buf, dh), net::CODEC_STATUS_PAUSE);
}

// Encode 缺版本号：HttpCodec 内部没有校验，可能 crash——用 death test
TEST(HttpCodec, EncodeMissingHttpVersionDeath)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_GET);
    req.set_url("http://127.0.0.1/x");

    util::CBuffer buf;
    EXPECT_DEATH({ codec.Encode(req, &buf); }, "");
}

TEST(HttpCodec, AddHttpHeader)
{
    net::HttpCodec codec(util::CODEC_HTTP);
    codec.AddHttpHeader("X-Custom", "thunder");

    HttpMsg req;
    req.set_type(HTTP_REQUEST);
    req.set_method(HTTP_GET);
    req.set_url("http://127.0.0.1/x");
    req.set_http_major(1);
    req.set_http_minor(1);

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(req, &buf));
    EXPECT_GT(buf.ReadableBytes(), 0u);
}

TEST(HttpCodec, LargeResponseBody)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    HttpMsg resp;
    resp.set_type(HTTP_RESPONSE);
    resp.set_status_code(200);
    resp.set_http_major(1);
    resp.set_http_minor(1);
    resp.set_body(std::string(1000, 'A'));

    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Encode(resp, &buf));

    HttpMsg decoded;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
    EXPECT_EQ(std::string(1000, 'A'), decoded.body());
}

TEST(HttpCodec, ResponseStatusCodeVariants)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    for (int code : {200, 301, 404, 500})
    {
        HttpMsg resp;
        resp.set_type(HTTP_RESPONSE);
        resp.set_status_code(code);
        resp.set_http_major(1);
        resp.set_http_minor(1);

        util::CBuffer buf;
        ASSERT_EQ(net::CODEC_STATUS_OK, codec.Encode(resp, &buf));
        HttpMsg decoded;
        ASSERT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
        EXPECT_EQ(static_cast<unsigned int>(code), decoded.status_code());
    }
}

TEST(HttpCodec, HttpMethodGETandPOST)
{
    net::HttpCodec codec(util::CODEC_HTTP);

    for (int method : {HTTP_GET, HTTP_POST})
    {
        HttpMsg req;
        req.set_type(HTTP_REQUEST);
        req.set_method(method);
        req.set_url("http://127.0.0.1/x");
        req.set_http_major(1);
        req.set_http_minor(1);

        util::CBuffer buf;
        ASSERT_EQ(net::CODEC_STATUS_OK, codec.Encode(req, &buf));
        HttpMsg decoded;
        ASSERT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, decoded));
        EXPECT_EQ(method, decoded.method());
    }
}

TEST(HttpCodec, DecodeEmptyBufferReturnsPause)
{
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    HttpMsg msg;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&buf, msg));
}

// ========== TryFastDecodeHttpRequest 单元测试 ==========
// 用原始 HTTP 字节直接测 Fast Path，绕过 Encode/Decode 往返。
// 通过 Decode(CBuffer,HttpMsg) 调用以覆盖完整路径（含 gzip 检查等）。

namespace {

// 辅助：构造原始 HTTP 请求字节 → 走 HttpCodec::Decode → 验证
struct FastPathResult {
    bool ok;
    int method;
    std::string path;
    std::string url;
    std::string body;
    int major;
    int minor;
    size_t params_count;
    bool has_connection;
    bool has_keepalive;
};

FastPathResult RunDecode(const std::string& raw) {
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    buf.Write(raw.data(), raw.size());
    HttpMsg msg;
    auto status = codec.Decode(&buf, msg);
    return {
        status == net::CODEC_STATUS_OK,
        msg.method(),
        msg.path(),
        msg.url(),
        msg.body(),
        static_cast<int>(msg.http_major()),
        static_cast<int>(msg.http_minor()),
        static_cast<size_t>(msg.params_size()),
        msg.headers().find("Connection") != msg.headers().end(),
        msg.headers().find("Keep-Alive") != msg.headers().end()
    };
}

// 构造 Transfer-Encoding: chunked 响应字节 (单 chunk + 终止块, 无残留 → phr ret==0)
std::string MakeChunkedResponse(const std::string& body) {
    char sz[32];
    std::snprintf(sz, sizeof(sz), "%zx", body.size());
    return std::string("HTTP/1.1 200 OK\r\n")
         + "Content-Type: application/json\r\n"
         + "Transfer-Encoding: chunked\r\n\r\n"
         + sz + "\r\n" + body + "\r\n0\r\n\r\n";
}

}  // namespace

// ── 1. 全部 7 种 HTTP method ──

TEST(FastPath, Method_GET) {
    auto r = RunDecode("GET /api/v1 HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_GET, r.method);
    EXPECT_EQ("/api/v1", r.path);
}

TEST(FastPath, Method_POST) {
    auto r = RunDecode("POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_POST, r.method);
    EXPECT_EQ("/submit", r.path);
    EXPECT_EQ("hello", r.body);
}

TEST(FastPath, Method_PUT) {
    auto r = RunDecode("PUT /update HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_PUT, r.method);
    EXPECT_EQ("abc", r.body);
}

TEST(FastPath, Method_DELETE) {
    auto r = RunDecode("DELETE /remove HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_DELETE, r.method);
    EXPECT_EQ("/remove", r.path);
}

TEST(FastPath, Method_PATCH) {
    auto r = RunDecode("PATCH /modify HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_PATCH, r.method);
    EXPECT_EQ("/modify", r.path);
}

TEST(FastPath, Method_HEAD) {
    auto r = RunDecode("HEAD /health HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_HEAD, r.method);
    EXPECT_EQ("/health", r.path);
}

TEST(FastPath, Method_OPTIONS) {
    auto r = RunDecode("OPTIONS /cors HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_OPTIONS, r.method);
    EXPECT_EQ("/cors", r.path);
}

// ── 2. Query string 解析 ──

TEST(FastPath, QueryString_SingleParam) {
    auto r = RunDecode("GET /search?q=thunder HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("/search", r.path);
    EXPECT_EQ("/search?q=thunder", r.url);
    EXPECT_EQ(1u, r.params_count);
}

TEST(FastPath, QueryString_MultiParam) {
    auto r = RunDecode("GET /api?a=1&b=2&c=3 HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("/api", r.path);
    EXPECT_EQ(3u, r.params_count);
}

TEST(FastPath, QueryString_NoQuery) {
    auto r = RunDecode("GET /plain HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("/plain", r.path);
    EXPECT_EQ("/plain", r.url);
    EXPECT_EQ(0u, r.params_count);
}

// ── 3. Content-Length 大小写不敏感 ──

TEST(FastPath, ContentLength_LowerCase) {
    auto r = RunDecode("POST /x HTTP/1.1\r\ncontent-length: 4\r\n\r\nbody");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("body", r.body);
}

TEST(FastPath, ContentLength_UpperCase) {
    auto r = RunDecode("POST /x HTTP/1.1\r\nContent-Length: 4\r\n\r\nbody");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("body", r.body);
}

TEST(FastPath, ContentLength_MixedCase) {
    auto r = RunDecode("POST /x HTTP/1.1\r\nContent-length: 4\r\n\r\nbody");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("body", r.body);
}

TEST(FastPath, ContentLength_Zero) {
    auto r = RunDecode("GET /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("", r.body);
}

// ── 4. Connection / Keep-Alive 提取 ──

TEST(FastPath, Connection_KeepAlive) {
    auto r = RunDecode("GET /x HTTP/1.1\r\nConnection: keep-alive\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.has_connection);
}

TEST(FastPath, KeepAlive_Timeout) {
    auto r = RunDecode("GET /x HTTP/1.1\r\nKeep-Alive: timeout=5\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.has_keepalive);
}

TEST(FastPath, Connection_Close) {
    auto r = RunDecode("GET /x HTTP/1.1\r\nconnection: close\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.has_connection);
}

// ── 5. Content-Encoding 提取 ──

TEST(FastPath, ContentEncoding_Gzip) {
    // 只测 header 提取: Fast Path 不支持 gzip body，返回 false→回退 http_parser
    // 这里直接测 header 字段是否正确写入
    size_t consumed = 0;
    HttpMsg msg;
    std::string raw = "GET /x HTTP/1.1\r\nContent-Encoding: gzip\r\n\r\n";
    bool ok = net::TryFastDecodeHttpRequest(raw.data(), raw.size(), msg, consumed);
    EXPECT_TRUE(ok);
    auto it = msg.headers().find("Content-Encoding");
    ASSERT_NE(msg.headers().end(), it);
    EXPECT_EQ("gzip", it->second);
}

// ── 6. 回退场景: chunked → http_parser ──

TEST(FastPath, Fallback_ChunkedEncoding) {
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    // chunked 请求: http_parser 原生处理
    std::string raw = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "5\r\nhello\r\n0\r\n\r\n";
    buf.Write(raw.data(), raw.size());
    HttpMsg msg;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, msg));
    EXPECT_EQ(HTTP_POST, msg.method());
    EXPECT_EQ("hello", msg.body());
}

TEST(FastPath, Fallback_ChunkedMixedCase) {
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    std::string raw = "POST /x HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n"
                      "3\r\nabc\r\n0\r\n\r\n";
    buf.Write(raw.data(), raw.size());
    HttpMsg msg;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, msg));
    EXPECT_EQ("abc", msg.body());
}

// ── 7. 回退场景: CONNECT / TRACE → http_parser ──

TEST(FastPath, Fallback_CONNECT) {
    // CONNECT: Fast Path 返回 false，回退 http_parser。
    // http_parser 能解析 CONNECT 请求行 (无 body)，返回 OK。
    size_t consumed = 0;
    HttpMsg msg;
    bool ok = net::TryFastDecodeHttpRequest("CONNECT proxy:8080 HTTP/1.1\r\n\r\n",
            32, msg, consumed);
    EXPECT_FALSE(ok);  // Fast Path 拒绝 CONNECT，回退 http_parser
}

TEST(FastPath, Fallback_TRACE) {
    // TRACE: Fast Path 返回 false，回退 http_parser。
    // http_parser 可能 crash 在 TRACE 上 → 用 death test。
    size_t consumed = 0;
    HttpMsg msg;
    bool ok = net::TryFastDecodeHttpRequest("TRACE /debug HTTP/1.1\r\n\r\n",
            26, msg, consumed);
    EXPECT_FALSE(ok);  // Fast Path 拒绝 TRACE，回退 http_parser
}

// ── 8. 数据不全 → Fast Path 回退，http_parser PAUSE ──

TEST(FastPath, Fallback_IncompleteRequest) {
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    buf.Write("GET /x HTTP/1.1", 13);
    HttpMsg msg;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&buf, msg));
}

TEST(FastPath, Fallback_IncompleteBody) {
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    // Content-Length: 5 但只有 3 字节 body
    buf.Write("POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nabc", 43);
    HttpMsg msg;
    // Fast Path 检测数据不全返回 false → http_parser 也发现不全 → PAUSE
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&buf, msg));
}

// ── 9. HTTP 版本解析 ──

TEST(FastPath, HttpVersion_1_0) {
    auto r = RunDecode("GET /old HTTP/1.0\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(1, r.major);
    EXPECT_EQ(0, r.minor);
}

TEST(FastPath, HttpVersion_1_1) {
    auto r = RunDecode("GET /new HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(1, r.major);
    EXPECT_EQ(1, r.minor);
}

// ── 10. 多 header 混合 ──

TEST(FastPath, MultiHeader_Mixed) {
    auto r = RunDecode(
        "POST /api/data?page=1 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test\r\n"
        "Accept: application/json\r\n"
        "Content-Length: 15\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=30\r\n"
        "\r\n"
        "{\"key\":\"value\"}"
    );
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_POST, r.method);
    EXPECT_EQ("/api/data", r.path);
    EXPECT_EQ("/api/data?page=1", r.url);
    EXPECT_EQ("{\"key\":\"value\"}", r.body);
    EXPECT_EQ(1u, r.params_count);
    EXPECT_TRUE(r.has_connection);
    EXPECT_TRUE(r.has_keepalive);
}

// ── 11. 空 body POST ──

TEST(FastPath, EmptyBody_NoContentLength) {
    auto r = RunDecode("POST /empty HTTP/1.1\r\n\r\n");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(HTTP_POST, r.method);
    EXPECT_EQ("", r.body);
}

// ── 12. 大 body ──

TEST(FastPath, LargeBody_10KB) {
    std::string big(10240, 'X');
    std::string raw = "POST /big HTTP/1.1\r\nContent-Length: 10240\r\n\r\n" + big;
    auto r = RunDecode(raw);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(big, r.body);
}

// ── 13. chunked 响应解码 (etcd lease/keepalive 回归, issus #24) ──
// 修复前 TryFastDecodeHttpResponse 把 phr_decode_chunked 的 ret==0 (完整无残留)
// 误判为失败 → Decode 返回 PAUSE → keepalive 永远收不到响应 → 续租全挂、注册表崩塌。

TEST(FastPath, ChunkedResponse_SingleChunkNoTrailing) {
    // etcd /v3/lease/keepalive 的真实响应形态: 单 chunk, 终止块后无残留字节 (ret==0)
    const std::string body = R"({"result":{"ID":"123","TTL":"60"}})";
    auto r = RunDecode(MakeChunkedResponse(body));
    EXPECT_TRUE(r.ok);            // 修复前: status==PAUSE → ok=false
    EXPECT_EQ(body, r.body);
}

TEST(FastPath, ChunkedResponse_MultiChunk) {
    std::string raw =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n5\r\ndefgh\r\n0\r\n\r\n";
    auto r = RunDecode(raw);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("abcdefgh", r.body);
}

TEST(FastPath, ChunkedResponse_EmptyBody) {
    // 仅终止块的 chunked 响应 (body 为空) 也应解码成功
    std::string raw =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    auto r = RunDecode(raw);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("", r.body);
}

TEST(FastPath, ChunkedResponse_SplitAcrossReads) {
    // body chunk 与终止块分两次 recv 到达 (etcd keepalive 实况):
    // 首次解码 PAUSE 不得污染源缓冲, 补齐后必须正确解码。
    // 修复前 phr_decode_chunked 原地改写缓冲 → 重解到垃圾 → PAUSE 永挂 → 续租卡死。
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    const std::string body = R"({"result":{"ID":"7","TTL":"60"}})";
    char sz[16]; std::snprintf(sz, sizeof(sz), "%zx", body.size());
    const std::string part1 =
        std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
        + sz + "\r\n" + body + "\r\n";
    const std::string part2 = "0\r\n\r\n";

    buf.Write(part1.data(), part1.size());
    HttpMsg m1;
    EXPECT_EQ(net::CODEC_STATUS_PAUSE, codec.Decode(&buf, m1));  // 不完整 → PAUSE

    buf.Write(part2.data(), part2.size());
    HttpMsg m2;
    EXPECT_EQ(net::CODEC_STATUS_OK, codec.Decode(&buf, m2));     // 补齐 → OK
    EXPECT_EQ(body, m2.body());
}
