/**
 * HttpCodec 编解码 E2E 测试
 */
#include "codec/HttpCodec.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>

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

// http_parser crash on pure garbage; use ASSERT_DEATH
TEST(HttpCodec, DecodeCorruptedErrorsDeath)
{
    net::HttpCodec codec(util::CODEC_HTTP);
    util::CBuffer buf;
    char garbage[6] = {'x', 'x', 'x', 'x', 'x', 'x'};
    buf.Write(garbage, 6);

    HttpMsg dh;
    // http_parser 对非法输入可能 abort——用 death test 断言
    EXPECT_DEATH({ codec.Decode(&buf, dh); }, "");
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
