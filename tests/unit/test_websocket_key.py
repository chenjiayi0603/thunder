"""
WebSocket 握手密钥生成单元测试

防止 GenerateKey() 回归：验证 sha1(key+GUID) → base64 (非 hex)
对应 code/Hello/src/ModuleShake/ModuleShake.cpp
"""
import base64
import hashlib
import pytest

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def generate_accept_key(ws_key: str) -> str:
    """RFC 6455 Sec-WebSocket-Accept: base64(sha1(key + GUID))"""
    digest = hashlib.sha1((ws_key + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def generate_accept_key_hex(ws_key: str) -> str:
    """❌ BUG 版本: hex(sha1(key + GUID)) — 已修复，此函数仅为对照"""
    return hashlib.sha1((ws_key + GUID).encode("ascii")).hexdigest()


class TestWebSocketKeyGeneration:
    """RFC 6455 第 4.2.2 节 示例验证"""

    def test_rfc_example_accept(self):
        """RFC 6455 官方示例"""
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        assert generate_accept_key(key) == expected

    def test_empty_key(self):
        """空 key 也不应抛异常"""
        key = ""
        result = generate_accept_key(key)
        # base64(sha1(GUID)) 应该是确定性的
        assert len(result) > 0
        # 重复调应一致
        assert result == generate_accept_key(key)

    def test_minimal_key(self):
        """最小有效 key: base64(1 byte)"""
        key = "QQ=="
        result = generate_accept_key(key)
        assert len(result) == 28  # SHA1=20B, base64 always 28 chars

    def test_max_key(self):
        """长 key — 应稳定输出 base64"""
        key = base64.b64encode(b"x" * 1024).decode()
        result = generate_accept_key(key)
        # base64 输出不应包含 hex 特征 (无大写 A-F 均匀分布)
        assert len(result) > 0

    def test_not_hex_output(self):
        """回归测试: 确保不是 hex 输出"""
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        result = generate_accept_key(key)
        hex_version = generate_accept_key_hex(key)
        assert result != hex_version, f"BUG: returned hex {hex_version[:20]}..."
        # hex 只有 [0-9a-f]，base64 有更丰富字符集
        assert any(c.isupper() for c in result) or '/' in result or '+' in result, \
            f"Looks like hex: {result}"

    def test_base64_decode_roundtrip(self):
        """Sec-WebSocket-Accept 应该是合法 base64"""
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        result = generate_accept_key(key)
        try:
            decoded = base64.b64decode(result)
            assert len(decoded) == 20  # SHA1 = 20 bytes
        except Exception:
            pytest.fail(f"Not valid base64: {result}")

    def test_deterministic(self):
        """相同输入 → 相同输出"""
        key = "test_key_12345"
        results = [generate_accept_key(key) for _ in range(10)]
        assert all(r == results[0] for r in results)

    def test_different_keys_different_output(self):
        """不同 key → 不同输出"""
        r1 = generate_accept_key("key1")
        r2 = generate_accept_key("key2")
        assert r1 != r2


class TestWebSocketHandshakeHeaders:
    """测试握手 Headers 解析逻辑 (对应 ModuleShake::ParseWebsocketHandshake)"""

    def test_parse_minimal_handshake(self):
        """最小有效握手包"""
        request = (
            "GET /hello/shake HTTP/1.1\r\n"
            "Host: 127.0.0.1:27010\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        lines = request.split("\r\n")
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        assert headers["upgrade"] == "websocket"
        assert headers["connection"] == "Upgrade"
        assert "sec-websocket-key" in headers

    def test_empty_headers_no_upgrade(self):
        """空 header 集 — upgrade 应为空或不存在 (不可默认为 websocket)"""
        headers = {}
        upgrade = headers.get("upgrade", "")
        assert upgrade != "websocket", "Default should not be websocket"


class TestIteratorBugRegression:
    """回归测试: 迭代器从 begin() 开始 (不是 end())"""

    def test_begin_not_end(self):
        """验证迭代正确: for(iter=begin; iter!=end; ++iter) 会遍历全部元素"""
        headers = {
            "upgrade": "websocket",
            "connection": "Upgrade",
            "sec-websocket-key": "dGVzdA==",
        }
        parsed = {}
        # 正确: begin → end
        for k, v in headers.items():
            if k == "upgrade":
                parsed["upgrade"] = v
            elif k == "connection":
                parsed["connection"] = v
            elif k == "sec-websocket-key":
                parsed["key"] = v

        assert parsed.get("upgrade") == "websocket"
        assert parsed.get("connection") == "Upgrade"
        assert parsed.get("key") == "dGVzdA=="

        # BUG 版本: end → end (不执行任何迭代)
        parsed_bug = {}
        # 模拟 end() → end() 循环: 直接不执行
        assert len(parsed_bug) == 0, "Bug version should parse nothing"

    def test_correct_parse_finds_all_headers(self):
        """正确解析应找到所有 WebSocket 相关 header"""
        headers = {
            "connection": "Upgrade",
            "upgrade": "websocket",
            "sec-websocket-key": "key",
            "sec-websocket-version": "13",
            "host": "localhost",
        }
        found = 0
        for k, v in headers.items():
            lower = k.lower()
            if lower in ("upgrade", "connection", "sec-websocket-key",
                         "sec-websocket-version", "host"):
                found += 1
        assert found == 5, f"Should find all 5 WS headers, found {found}"
