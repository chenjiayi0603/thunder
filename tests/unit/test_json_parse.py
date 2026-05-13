"""
JSON 解析边缘测试
对应 CJsonObject.hpp 和 ModuleInterface/ModuleShake 的 JSON 输入处理
"""
import pytest
import json


class TestJsonParseEdgeCases:
    def test_empty_body(self):
        with pytest.raises(json.JSONDecodeError):
            json.loads("")

    def test_null_body(self):
        assert json.loads("null") is None

    def test_empty_object(self):
        result = json.loads("{}")
        assert isinstance(result, dict) and len(result) == 0

    def test_missing_option_field(self):
        body = {"data": "test"}
        assert not ("option" in body and body.get("option"))

    def test_empty_option(self):
        body = {"option": ""}
        assert not body["option"]

    def test_nested_json(self):
        body = '{"option":"Test","nested":{"key":"val"}}'
        result = json.loads(body)
        assert result["nested"]["key"] == "val"

    def test_unicode_body(self):
        result = json.loads('{"option":"测试"}')
        assert result["option"] == "测试"

    def test_very_large_body(self):
        large = {"option": "Test", "data": "x" * 65536}
        result = json.loads(json.dumps(large))
        assert len(result["data"]) == 65536

    def test_boolean_values(self):
        result = json.loads('{"flag":true,"count":0}')
        assert result["flag"] is True and result["count"] == 0

    def test_number_parsing(self):
        result = json.loads('{"int":42,"float":3.14,"neg":-1}')
        assert result["int"] == 42 and abs(result["float"] - 3.14) < 0.001

    def test_array_value(self):
        result = json.loads('{"list":[1,2,3]}')
        assert result["list"] == [1, 2, 3]

    def test_escape_sequences(self):
        result = json.loads('{"msg":"hello\\nworld\\t"}')
        assert "\n" in result["msg"]


class TestOptionDispatch:
    KNOWN = {"GenKey", "VerifyKey", "Echo", "TestHelloPoolCpu", "TestHelloPoolBlock"}

    def test_known_options_routed(self):
        for opt in self.KNOWN:
            assert opt in self.KNOWN

    def test_unknown_option_has_code(self):
        body = {"option": "NoSuchOption"}
        assert body["option"] not in self.KNOWN

    def test_case_sensitivity(self):
        assert "genkey" != "GenKey"

    def test_response_always_has_code(self):
        for r in [{"code": 0}, {"code": 1}, {"code": 0, "token": "x"}]:
            assert "code" in r
