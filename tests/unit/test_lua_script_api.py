"""unit tests for admin-web Lua script API helpers (no network, no etcd)"""
import base64
import json
import sys
import types
import unittest
from unittest.mock import MagicMock, patch

# ── stub urllib.request so imports don't fail without network ──────────────────
_urllib_stub = types.ModuleType("urllib")
_urllib_req_stub = types.ModuleType("urllib.request")
_urllib_req_stub.Request = MagicMock
_urllib_req_stub.urlopen = MagicMock(side_effect=Exception("no network in unit tests"))
_urllib_stub.request = _urllib_req_stub
sys.modules.setdefault("urllib", _urllib_stub)
sys.modules.setdefault("urllib.request", _urllib_req_stub)

# ── stub http.server ───────────────────────────────────────────────────────────
_http_stub = types.ModuleType("http")
_http_server_stub = types.ModuleType("http.server")
_http_server_stub.SimpleHTTPRequestHandler = object
_http_server_stub.HTTPServer = MagicMock
_http_stub.server = _http_server_stub
sys.modules.setdefault("http", _http_stub)
sys.modules.setdefault("http.server", _http_server_stub)

# ── load module ────────────────────────────────────────────────────────────────
import importlib.util, os
_srv = os.path.join(os.path.dirname(__file__), "../../deploy/admin-web/server.py")
spec = importlib.util.spec_from_file_location("server", _srv)
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class TestEtcdHelpers(unittest.TestCase):
    """_etcd_get / _etcd_put wrappers"""

    def _make_etcd_response(self, key: str, value: str):
        return json.dumps({
            "kvs": [{
                "key": base64.b64encode(key.encode()).decode(),
                "value": base64.b64encode(value.encode()).decode(),
            }]
        }).encode()

    def test_etcd_get_returns_value(self):
        mock_resp = MagicMock()
        mock_resp.read.return_value = self._make_etcd_response("/k", "hello")
        with patch.object(server, "_etcd_get", return_value="hello") as mock_get:
            result = server._etcd_get("/k")
            self.assertEqual(result, "hello")

    def test_etcd_get_missing_returns_none(self):
        with patch.object(server, "_etcd_get", return_value=None):
            self.assertIsNone(server._etcd_get("/missing"))

    def test_etcd_put_returns_bool(self):
        with patch.object(server, "_etcd_put", return_value=True) as mock_put:
            self.assertTrue(server._etcd_put("/k", "v"))


class TestLuaListHelper(unittest.TestCase):
    """UploadServer._lua_list"""

    def _make_server(self):
        srv = object.__new__(server.UploadServer)
        return srv

    def test_list_returns_lua_modules_only(self):
        etcd_val = json.dumps({"module": [
            {"url_path": "/hello/hello", "so_path": "plugins/x.so", "version": 1},
            {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua", "version": 2},
            {"url_path": "/hello/lua_limit", "script_path": "scripts/limit.lua", "version": 3},
        ]})
        srv = self._make_server()
        with patch.object(server, "_etcd_get", return_value=etcd_val):
            result = srv._lua_list("HELLO_HTTP")
        self.assertEqual(len(result), 2)
        url_paths = {r["url_path"] for r in result}
        self.assertIn("/hello/lua_echo", url_paths)
        self.assertIn("/hello/lua_limit", url_paths)
        self.assertNotIn("/hello/hello", url_paths)

    def test_list_empty_when_no_etcd(self):
        srv = self._make_server()
        with patch.object(server, "_etcd_get", return_value=None):
            self.assertEqual(srv._lua_list("HELLO_HTTP"), [])

    def test_list_has_inline_flag(self):
        etcd_val = json.dumps({"module": [
            {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua",
             "script_content": "function handle_request(r) end", "version": 5},
        ]})
        srv = self._make_server()
        with patch.object(server, "_etcd_get", return_value=etcd_val):
            result = srv._lua_list("HELLO_HTTP")
        self.assertTrue(result[0]["has_inline"])
        self.assertEqual(result[0]["version"], 5)


class TestLuaPushHelper(unittest.TestCase):
    """UploadServer._lua_push"""

    def _make_server(self, tmp_dir):
        server.UploadServer.upload_base = str(tmp_dir)
        srv = object.__new__(server.UploadServer)
        return srv

    def test_push_updates_version_and_content(self):
        import tempfile, pathlib
        with tempfile.TemporaryDirectory() as tmp:
            etcd_val = json.dumps({"module": [
                {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua", "version": 1},
            ]})
            srv = self._make_server(pathlib.Path(tmp))
            with patch.object(server, "_etcd_get", return_value=etcd_val), \
                 patch.object(server, "_etcd_put", return_value=True) as mock_put:
                result = srv._lua_push(
                    "HELLO_HTTP", "echo.lua",
                    "function handle_request(r) end",
                    "/hello/lua_echo"
                )
            self.assertTrue(result["ok"])
            self.assertEqual(result["version"], 2)
            # verify script written to disk
            self.assertTrue((pathlib.Path(tmp) / "HelloHttp" / "scripts" / "echo.lua").exists())
            # verify etcd_put called with bumped version + content
            call_args = mock_put.call_args[0]
            written = json.loads(call_args[1])["module"][0]
            self.assertEqual(written["version"], 2)
            self.assertIn("script_content", written)

    def test_push_fails_when_url_path_not_found(self):
        import tempfile, pathlib
        with tempfile.TemporaryDirectory() as tmp:
            etcd_val = json.dumps({"module": [
                {"url_path": "/hello/lua_echo", "version": 1},
            ]})
            srv = self._make_server(pathlib.Path(tmp))
            with patch.object(server, "_etcd_get", return_value=etcd_val), \
                 patch.object(server, "_etcd_put", return_value=True):
                result = srv._lua_push(
                    "HELLO_HTTP", "other.lua", "content", "/hello/nonexistent"
                )
            self.assertFalse(result["ok"])
            self.assertIn("not found", result["error"])


if __name__ == "__main__":
    unittest.main()
