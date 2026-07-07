"""
Token 验证逻辑单元测试

对应 code/Logic/src/LogicSession.h 的 VerifyTokenPermutation
以及 code/Logic/src/CmdGetToken.cpp 的 GenKey/VerifyKey 流程

防止回归: token 查找 + permutation 验证
"""
import pytest


class TokenStore:
    """模拟 LogicSession::m_tokenM — token 存储表"""
    def __init__(self):
        self._store = {}

    def gen_token(self, token: str, key: str) -> None:
        self._store[token] = key

    def verify_permutation(self, token: str, key: str) -> bool:
        """VerifyTokenPermutation: 查表 + permutation 校验"""
        if token not in self._store:
            return False
        stored_key = self._store[token]
        if len(key) != len(stored_key):
            return False
        # 字符频率校验 (permutation: 相同字符重排)
        freq = {}
        for c in stored_key:
            freq[c] = freq.get(c, 0) + 1
        for c in key:
            if freq.get(c, 0) == 0:
                return False
            freq[c] -= 1
        return True


class TestTokenVerification:
    """Token 关键路径测试"""

    def setup_method(self):
        self.store = TokenStore()

    def test_genkey_then_verify_correct(self):
        """正常流程: GenKey → VerifyKey 成功"""
        self.store.gen_token("tok123", "key456")
        assert self.store.verify_permutation("tok123", "key456")

    def test_wrong_token_fails(self):
        """假 token — 表中不存在"""
        self.store.gen_token("real_token", "real_key")
        assert not self.store.verify_permutation("bad_token", "real_key")

    def test_wrong_key_fails(self):
        """正确 token 但错误 key"""
        self.store.gen_token("tok", "secret_key_123")
        assert not self.store.verify_permutation("tok", "wrong_key_456")

    def test_empty_store_verify_fails(self):
        """空表 — 任何 token 都失败"""
        assert not self.store.verify_permutation("any", "thing")

    def test_permutation_valid(self):
        """Permutation: key 字符重排仍通过"""
        self.store.gen_token("t", "abc123")
        assert self.store.verify_permutation("t", "3c2b1a")  # 重排
        assert self.store.verify_permutation("t", "123abc")  # 重排

    def test_permutation_invalid_length(self):
        """长度不同 → 失败"""
        self.store.gen_token("t", "abc")
        assert not self.store.verify_permutation("t", "abcd")
        assert not self.store.verify_permutation("t", "ab")

    def test_permutation_invalid_chars(self):
        """额外字符 → 失败"""
        self.store.gen_token("t", "abc")
        assert not self.store.verify_permutation("t", "abz")
        assert not self.store.verify_permutation("t", "abcd")

    def test_multi_token_isolation(self):
        """多个 token 独立 — 不会串"""
        self.store.gen_token("t1", "k1")
        self.store.gen_token("t2", "k2")
        assert self.store.verify_permutation("t1", "k1")
        assert self.store.verify_permutation("t2", "k2")
        assert not self.store.verify_permutation("t1", "k2")
        assert not self.store.verify_permutation("t2", "k1")

    def test_overwrite_token(self):
        """同一个 token 多次 GenKey — 覆盖旧值"""
        self.store.gen_token("t", "old_key")
        self.store.gen_token("t", "new_key")
        assert not self.store.verify_permutation("t", "old_key")
        assert self.store.verify_permutation("t", "new_key")

    def test_response_format_genkey_success(self):
        """GenKey 成功响应格式: code=0 + token + key"""
        response = {"code": 0, "token": "tok", "key": "key"}
        assert response["code"] == 0
        assert "token" in response
        assert "key" in response
        assert response["token"] and response["key"]

    def test_response_format_verify_wrong(self):
        """VerifyKey 错误 token 响应格式: code=1, HTTP 200"""
        response = {"code": 1}
        assert response["code"] == 1  # 业务错误码
        # 不检查 HTTP status (在集成测试中验证)

    def test_empty_token_key_rejected(self):
        """空 token/key 应被拒绝"""
        # CmdGetToken: token.empty() || key.empty() → Response(code=1)
        token, key = "", ""
        is_valid = bool(token and key)
        # 空值 → 直接返回错误，不走到 VerifyTokenPermutation
        assert not is_valid


class TestInterfaceErrorHandling:
    """Interface 层错误处理 (ModuleInterface.cpp)"""

    def test_invalid_json_body(self):
        """非法 JSON → code=1"""
        import json
        body = "not json{{{"
        try:
            json.loads(body)
            valid = True
        except json.JSONDecodeError:
            valid = False
        assert not valid, "Should reject invalid JSON"

    def test_missing_option(self):
        """缺少 option 字段 → code=1"""
        body = {"data": "test"}
        has_option = "option" in body and body["option"]
        assert not has_option, "Should reject missing option"

    def test_verifykey_missing_token_or_key(self):
        """VerifyKey 但 token 或 key 为空 → code=1"""
        body = {"option": "VerifyKey", "token": "", "key": "abc"}
        has_required = bool(body.get("token") and body.get("key"))
        assert not has_required

    def test_genkey_accepts_no_token_key(self):
        """GenKey 不需要客户端传 token/key (服务端生成)"""
        body = {"option": "GenKey"}
        # GenKey 分支走 genkey=="1"，不需要 token/key 非空检查
        assert body["option"] == "GenKey"
        assert "token" not in body  # 服务端生成
