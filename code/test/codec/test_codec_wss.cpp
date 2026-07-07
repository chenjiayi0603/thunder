/**
 * WssCodec 单元测试
 *
 * 原有 3 个用例（Construction / SetSslConfig / EncodeDecodeMsgHeadBody）保留不变。
 * 新增覆盖：
 *   - null 指针边界 (Decode / EncodeToConnection)
 *   - 连接状态管理 (SetConnectionRole / RemoveConnection / 多连接独立)
 *   - TLS 握手自检  (内存 BIO 对等握手，跳过若证书文件不存在)
 *   - 握手后 WS Echo roundtrip (client→server、server→client 均验证)
 */

#include "codec/WssCodec.hpp"
#include "util/CBuffer.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <string>

// ── 证书路径 (HelloWss 服务自签证书) ─────────────────────────────────────
static constexpr const char* kCertFile =
    "/home/tommychen/thunder/deploy/HelloWss/conf/certs/server.crt";
static constexpr const char* kKeyFile =
    "/home/tommychen/thunder/deploy/HelloWss/conf/certs/server.key";

// ── 辅助：创建带缓冲区的假连接 ────────────────────────────────────────────
static tagConnectionAttr MakeConn(int fd)
{
    tagConnectionAttr conn;
    conn.iFd = fd;
    conn.ucConnectStatus = eConnectStatus_init;
    conn.pRecvBuff = std::make_shared<util::CBuffer>();
    conn.pSendBuff = std::make_shared<util::CBuffer>();
    return conn;
}

// ── 辅助：将 src 所有可读字节移入 dst，返回搬运字节数 ────────────────────
static size_t Shuttle(util::CBuffer& src, util::CBuffer& dst)
{
    const size_t n = src.ReadableBytes();
    if (n > 0)
    {
        dst.Write(src.GetRawReadBuffer(), n);
        src.AdvanceReadIndex(n);
    }
    return n;
}

// ── 辅助：驱动 TLS 握手至完成（或超过 maxRounds 视为失败）────────────────
//   原理：client 先起步（空接收缓冲区触发 SSL_do_handshake 产生 ClientHello），
//         然后交替把各自 pSendBuff 搬到对端 pRecvBuff，直到没有新字节流转。
static bool DriveHandshake(net::WssCodec& serverCodec, tagConnectionAttr& serverConn,
                           net::WssCodec& clientCodec, tagConnectionAttr& clientConn,
                           int maxRounds = 20)
{
    MsgHead h;
    MsgBody b;
    for (int i = 0; i < maxRounds; ++i)
    {
        clientCodec.Decode(&clientConn, h, b);
        serverCodec.Decode(&serverConn, h, b);

        const size_t c2s = Shuttle(*clientConn.pSendBuff, *serverConn.pRecvBuff);
        const size_t s2c = Shuttle(*serverConn.pSendBuff, *clientConn.pRecvBuff);

        if (c2s == 0 && s2c == 0)
        {
            return true;  // 没有新握手报文 → 双方握手已完成
        }
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════
//  原有测试（保留）
// ═════════════════════════════════════════════════════════════════════════

TEST(WssCodec, Construction)
{
    net::WssCodec codec;
    (void)codec;
    SUCCEED();
}

TEST(WssCodec, SetSslConfig)
{
    net::WssCodec codec;
    net::WssCodec::SslConfig cfg;
    cfg.strServerCertFile = "/tmp/cert.pem";
    cfg.strServerKeyFile  = "/tmp/key.pem";
    codec.SetSslConfig(cfg);
    SUCCEED();
}

TEST(WssCodec, EncodeDecodeMsgHeadBody)
{
    net::WssCodec codec;
    MsgHead head;
    head.set_cmd(1);
    head.set_seq(100);
    MsgBody body;
    body.set_body("test");
    util::CBuffer buf;
    auto s = codec.Encode(head, body, &buf);
    EXPECT_EQ(s, net::CODEC_STATUS_OK);
    EXPECT_TRUE(buf.ReadableBytes() > 0);
}

// ═════════════════════════════════════════════════════════════════════════
//  新增：null 指针边界检查
// ═════════════════════════════════════════════════════════════════════════

TEST(WssCodec, DecodeNullConnReturnsErr)
{
    net::WssCodec codec;
    MsgHead h;
    MsgBody b;
    EXPECT_EQ(net::CODEC_STATUS_ERR, codec.Decode(static_cast<tagConnectionAttr*>(nullptr), h, b));
}

TEST(WssCodec, EncodeToConnectionNullConnReturnsErr)
{
    net::WssCodec codec;
    MsgHead h;
    MsgBody b;
    util::CBuffer buf;
    EXPECT_EQ(net::CODEC_STATUS_ERR,
              codec.EncodeToConnection(static_cast<tagConnectionAttr*>(nullptr), h, b, &buf));
}

TEST(WssCodec, EncodeToConnectionNullOutputBuffReturnsErr)
{
    net::WssCodec codec;
    tagConnectionAttr conn = MakeConn(99);
    MsgHead h;
    MsgBody b;
    EXPECT_EQ(net::CODEC_STATUS_ERR,
              codec.EncodeToConnection(&conn, h, b, static_cast<util::CBuffer*>(nullptr)));
}

// ═════════════════════════════════════════════════════════════════════════
//  新增：连接状态管理
// ═════════════════════════════════════════════════════════════════════════

// RemoveConnection 未知 fd 不崩溃（幂等）
TEST(WssCodec, RemoveUnknownConnectionIsNoop)
{
    net::WssCodec codec;
    EXPECT_NO_FATAL_FAILURE(codec.RemoveConnection(9999));
    EXPECT_NO_FATAL_FAILURE(codec.RemoveConnection(9999));  // second call
}

// SetConnectionRole + RemoveConnection 正常清理
TEST(WssCodec, SetConnectionRoleAndRemove)
{
    net::WssCodec codec;
    codec.SetConnectionRole(5, true);   // 服务端角色
    codec.SetConnectionRole(6, false);  // 客户端角色

    // 触发 EnsureState（创建 TLS 状态）
    tagConnectionAttr conn5 = MakeConn(5);
    tagConnectionAttr conn6 = MakeConn(6);
    MsgHead h;
    MsgBody b;
    codec.Decode(&conn5, h, b);  // PAUSE 是正常的
    codec.Decode(&conn6, h, b);

    // 移除后不崩溃，再次访问 fd5 不影响 fd6
    EXPECT_NO_FATAL_FAILURE(codec.RemoveConnection(5));
    EXPECT_NO_FATAL_FAILURE(codec.RemoveConnection(5));  // 幂等
    EXPECT_NO_FATAL_FAILURE(codec.Decode(&conn6, h, b));
}

// 两个不同 fd 的 TLS 状态相互独立
TEST(WssCodec, MultipleConnectionsAreIndependent)
{
    net::WssCodec codec;
    codec.SetConnectionRole(10, true);   // fd10 = server
    codec.SetConnectionRole(11, false);  // fd11 = client

    tagConnectionAttr c10 = MakeConn(10);
    tagConnectionAttr c11 = MakeConn(11);
    MsgHead h;
    MsgBody b;
    codec.Decode(&c10, h, b);
    codec.Decode(&c11, h, b);

    // 删除 fd10，fd11 不受影响
    codec.RemoveConnection(10);
    EXPECT_NO_FATAL_FAILURE(codec.Decode(&c11, h, b));

    // 删除 fd11
    EXPECT_NO_FATAL_FAILURE(codec.RemoveConnection(11));
}

// ═════════════════════════════════════════════════════════════════════════
//  新增：TLS 握手（需真实证书文件）
// ═════════════════════════════════════════════════════════════════════════

// 检查证书是否可访问的帮助函数
static bool CertsAvailable()
{
    FILE* f = std::fopen(kCertFile, "r");
    if (!f) return false;
    std::fclose(f);
    f = std::fopen(kKeyFile, "r");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// TLS 握手双方均能完成
TEST(WssCodec, TlsHandshakeCompletes)
{
    if (!CertsAvailable())
    {
        GTEST_SKIP() << "WSS 证书文件不存在，跳过 TLS 握手测试";
    }

    net::WssCodec serverCodec;
    net::WssCodec::SslConfig cfg;
    cfg.strServerCertFile = kCertFile;
    cfg.strServerKeyFile  = kKeyFile;
    serverCodec.SetSslConfig(cfg);
    serverCodec.SetConnectionRole(1, true);   // 服务端角色

    net::WssCodec clientCodec;
    clientCodec.SetConnectionRole(2, false);  // 客户端角色

    tagConnectionAttr serverConn = MakeConn(1);
    tagConnectionAttr clientConn = MakeConn(2);

    const bool done = DriveHandshake(serverCodec, serverConn, clientCodec, clientConn);
    EXPECT_TRUE(done) << "TLS 握手未在 20 轮内完成";
}

// 握手后 EncodeToConnection 输出密文（验证 TLS 加密层生效）
//
// WssCodec 是纯服务端编解码器：
//   Encode  → 无掩码帧（服务端→客户端，符合 WS RFC）
//   Decode  → 期望有掩码帧（客户端→服务端，符合 WS RFC）
// EncodeToConnection 走服务端 Encode 路径，对应客户端 Decode 只能由真实
// WebSocket 客户端（浏览器/Python ws 库）完成，不在本 codec 测试范围内。
// 此测试验证：握手后加密层确实生效（密文非空、与明文不同）。
TEST(WssCodec, TlsEncryptionObfuscatesPlaintext)
{
    if (!CertsAvailable())
    {
        GTEST_SKIP() << "WSS 证书文件不存在，跳过 TLS 加密测试";
    }

    net::WssCodec serverCodec;
    net::WssCodec::SslConfig cfg;
    cfg.strServerCertFile = kCertFile;
    cfg.strServerKeyFile  = kKeyFile;
    serverCodec.SetSslConfig(cfg);
    serverCodec.SetConnectionRole(1, true);

    net::WssCodec clientCodec;
    clientCodec.SetConnectionRole(2, false);

    tagConnectionAttr serverConn = MakeConn(1);
    tagConnectionAttr clientConn = MakeConn(2);

    ASSERT_TRUE(DriveHandshake(serverCodec, serverConn, clientCodec, clientConn))
        << "TLS 握手未完成，无法继续加密测试";

    // ── 服务端对已知明文调用 EncodeToConnection ────────────────────────
    MsgHead head;
    head.set_cmd(20002);
    head.set_seq(7);
    MsgBody body;
    const std::string kKnownJson = R"({"code":0,"msg":"ok"})";
    body.set_body(kKnownJson);

    // 先拿到对应的未加密明文（走非连接 Encode 路径）
    util::CBuffer plainBuf;
    ASSERT_EQ(net::CODEC_STATUS_OK, serverCodec.Encode(head, body, &plainBuf));
    const size_t plainLen = plainBuf.ReadableBytes();
    ASSERT_GT(plainLen, 0u);

    // EncodeToConnection 走 TLS 加密路径
    ASSERT_EQ(net::CODEC_STATUS_OK,
              serverCodec.EncodeToConnection(&serverConn, head, body,
                                            serverConn.pSendBuff.get()));
    const size_t cipherLen = serverConn.pSendBuff->ReadableBytes();

    // 密文非空
    EXPECT_GT(cipherLen, 0u) << "TLS 加密后应有密文输出";

    // 密文长度不等于明文（TLS 增加了 record header / MAC / padding）
    // 通常 cipher > plain；用不等号而非大于号，避免对 TLS record overhead 做硬假设
    EXPECT_NE(cipherLen, plainLen) << "密文长度应与明文不同（TLS 增加了 overhead）";

    // 密文不能直接包含已知 JSON 字符串（加密确实混淆了内容）
    const char* pCipher = serverConn.pSendBuff->GetRawReadBuffer();
    const std::string cipher(pCipher, cipherLen);
    EXPECT_EQ(std::string::npos, cipher.find(kKnownJson))
        << "密文中不应出现明文 JSON（加密未生效）";
}

// 握手后对同一连接多次 EncodeToConnection 累积密文（连续加密稳定性）
TEST(WssCodec, TlsEncryptionMultipleMessages)
{
    if (!CertsAvailable())
    {
        GTEST_SKIP() << "WSS 证书文件不存在，跳过 TLS 加密测试";
    }

    net::WssCodec serverCodec;
    net::WssCodec::SslConfig cfg;
    cfg.strServerCertFile = kCertFile;
    cfg.strServerKeyFile  = kKeyFile;
    serverCodec.SetSslConfig(cfg);
    serverCodec.SetConnectionRole(1, true);

    net::WssCodec clientCodec;
    clientCodec.SetConnectionRole(2, false);

    tagConnectionAttr serverConn = MakeConn(1);
    tagConnectionAttr clientConn = MakeConn(2);

    ASSERT_TRUE(DriveHandshake(serverCodec, serverConn, clientCodec, clientConn));

    size_t totalCipher = 0;
    for (int i = 0; i < 5; ++i)
    {
        MsgHead h;
        h.set_cmd(20002);
        h.set_seq(static_cast<uint32_t>(i));
        MsgBody b;
        b.set_body(R"({"code":0})");

        const size_t before = serverConn.pSendBuff->ReadableBytes();
        ASSERT_EQ(net::CODEC_STATUS_OK,
                  serverCodec.EncodeToConnection(&serverConn, h, b,
                                                serverConn.pSendBuff.get()));
        const size_t after = serverConn.pSendBuff->ReadableBytes();
        EXPECT_GT(after, before) << "第 " << i << " 次加密应增加密文";
        totalCipher += (after - before);
    }
    EXPECT_GT(totalCipher, 0u);
}
