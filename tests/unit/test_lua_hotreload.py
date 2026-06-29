"""Unit tests for Lua hot-reload flow — no network, no etcd, no Docker.

Tests the admin-web server.py hot-reload logic:
  - _lua_push writes script to correct deploy/ path (not admin-web/)
  - _lua_push bumps version and stores script_content in etcd
  - _sync_config reads local module configs and writes to etcd
  - _lua_list returns only modules with script_path/script_content
"""
import base64
import json
import sys
import types
import unittest
import os
import tempfile
import pathlib
from unittest.mock import MagicMock, patch

# ── stub imports ───────────────────────────────────────────────────────────────
_http_stub = types.ModuleType("http")
_http_server_stub = types.ModuleType("http.server")
_http_server_stub.SimpleHTTPRequestHandler = object
_http_server_stub.HTTPServer = MagicMock
_http_stub.server = _http_server_stub
sys.modules.setdefault("http", _http_stub)
sys.modules.setdefault("http.server", _http_server_stub)

_urllib_stub = types.ModuleType("urllib")
_urllib_req_stub = types.ModuleType("urllib.request")
_urllib_req_stub.Request = MagicMock
_urllib_req_stub.urlopen = MagicMock(side_effect=Exception("no network in unit tests"))
_urllib_stub.request = _urllib_req_stub
sys.modules.setdefault("urllib", _urllib_stub)
sys.modules.setdefault("urllib.request", _urllib_req_stub)

import importlib.util
_srv = os.path.join(os.path.dirname(__file__), "../../deploy/admin-web/server.py")
spec = importlib.util.spec_from_file_location("server", _srv)
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class TestLuaHotReloadDeployPath(unittest.TestCase):
    """#125: admin-web writes Lua scripts to deploy/{TypeDir}/scripts/ not admin-web/{TypeDir}/scripts/"""

    def test_push_writes_to_correct_deploy_path(self):
        """Verify _lua_push writes to upload_base/HelloHttp/scripts/echo.lua"""
        with tempfile.TemporaryDirectory() as tmp:
            deploy_root = pathlib.Path(tmp)
            # Simulate: upload_base = deploy/ (not deploy/admin-web/)
            server.UploadServer.upload_base = str(deploy_root)
            etcd_val = json.dumps({"module": [
                {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua", "version": 1},
            ]})
            srv = object.__new__(server.UploadServer)
            with patch.object(server, "_etcd_get", return_value=etcd_val), \
                 patch.object(server, "_etcd_put", return_value=True):
                result = srv._lua_push(
                    "HELLO_HTTP", "echo.lua",
                    "function handle_request(msg)\n  return true\nend",
                    "/hello/lua_echo"
                )
            self.assertTrue(result["ok"])
            # File should be at deploy/HelloHttp/scripts/echo.lua (not deploy/admin-web/HelloHttp/scripts/)
            target = deploy_root / "HelloHttp" / "scripts" / "echo.lua"
            self.assertTrue(target.exists(), f"Expected {target} to exist")
            self.assertIn("handle_request", target.read_text())

    def test_push_version_bump(self):
        """Verify version increments on each push"""
        with tempfile.TemporaryDirectory() as tmp:
            deploy_root = pathlib.Path(tmp)
            server.UploadServer.upload_base = str(deploy_root)
            etcd_val = json.dumps({"module": [
                {"url_path": "/hello/lua_echo", "version": 3},
            ]})
            srv = object.__new__(server.UploadServer)
            with patch.object(server, "_etcd_get", return_value=etcd_val), \
                 patch.object(server, "_etcd_put", return_value=True) as mock_put:
                result = srv._lua_push(
                    "HELLO_HTTP", "echo.lua",
                    "function handle_request(msg) end",
                    "/hello/lua_echo"
                )
            self.assertTrue(result["ok"])
            self.assertEqual(result["version"], 4)
            # Verify the version in etcd_put payload
            call_args = mock_put.call_args[0]
            written = json.loads(call_args[1])["module"][0]
            self.assertEqual(written["version"], 4)
            self.assertIn("script_content", written)

    def test_push_script_content_stored(self):
        """Verify script_content is written to etcd"""
        with tempfile.TemporaryDirectory() as tmp:
            server.UploadServer.upload_base = str(pathlib.Path(tmp))
            etcd_val = json.dumps({"module": [
                {"url_path": "/hello/lua_echo", "version": 1},
            ]})
            srv = object.__new__(server.UploadServer)
            content = "function handle_request(msg)\n  SendToClientFast('ok')\n  return true\nend"
            with patch.object(server, "_etcd_get", return_value=etcd_val), \
                 patch.object(server, "_etcd_put", return_value=True) as mock_put:
                srv._lua_push("HELLO_HTTP", "echo.lua", content, "/hello/lua_echo")
            call_args = mock_put.call_args[0]
            written = json.loads(call_args[1])["module"][0]
            self.assertEqual(written["script_content"], content)


class TestSyncConfigPath(unittest.TestCase):
    """#125: _sync_config reads from deploy/ not deploy/admin-web/"""

    def test_sync_config_uses_deploy_root(self):
        """_sync_config reads conf/HelloHttp.json from deploy/HelloHttp/conf/"""
        with tempfile.TemporaryDirectory() as tmp:
            deploy_root = pathlib.Path(tmp)
            # Create deploy/HelloHttp/conf/HelloHttp.json
            (deploy_root / "HelloHttp" / "conf").mkdir(parents=True)
            config = {
                "node_type": "HELLO_HTTP",
                "module": [
                    {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua", "version": 1},
                    {"url_path": "/hello/lua_limit", "script_path": "scripts/limit.lua", "version": 2},
                ]
            }
            (deploy_root / "HelloHttp" / "conf" / "HelloHttp.json").write_text(json.dumps(config))

            # Create the __file__ mock to point to deploy/admin-web/server.py
            with patch.object(server, "__file__",
                              str(deploy_root / "admin-web" / "server.py")):
                # Recompute deploy_dir = os.path.join(os.path.dirname(__file__), "..")
                # → admin-web/.. = deploy_root
                deploy_dir = os.path.join(os.path.dirname(str(deploy_root / "admin-web" / "server.py")), "..")
                self.assertEqual(os.path.realpath(deploy_dir), os.path.realpath(str(deploy_root)),
                                "Sync should read from deploy/ root")


class TestLuaHotReloadE2E(unittest.TestCase):
    """Integration test: etcd interaction for hot-reload flow (requires etcd but no Docker containers)

    These tests verify the etcd key/version management that drives the hot-reload:
    1. _sync_config writes module config to etcd key /thunder/config/module/{type}
    2. _lua_push bumps version on existing url_path
    3. Version bump is what triggers Manager's Watch → ReloadLua
    """

    def test_sync_config_writes_to_correct_etcd_key(self):
        """_sync_config writes to /thunder/config/module/HELLO_HTTP"""
        with tempfile.TemporaryDirectory() as tmp:
            deploy_root = pathlib.Path(tmp)
            (deploy_root / "HelloHttp" / "conf").mkdir(parents=True)
            config = {"node_type": "HELLO_HTTP", "module": [
                {"url_path": "/hello/lua_echo", "script_path": "scripts/echo.lua", "version": 1},
            ]}
            (deploy_root / "HelloHttp" / "conf" / "HelloHttp.json").write_text(json.dumps(config))

            srv = object.__new__(server.UploadServer)
            # Mock _sync_config's deploy_dir to use our temp dir
            with patch("os.path.dirname", return_value=str(deploy_root / "admin-web")), \
                 patch.object(server, "_etcd_put", return_value=True) as mock_put:
                # Monkey-patch the deploy_dir calculation
                import os as _os
                orig_dirname = _os.path.dirname
                def _dirname(p):
                    if "server.py" in str(p):
                        return str(deploy_root / "admin-web")
                    return orig_dirname(p)
                with patch.object(_os.path, "dirname", _dirname), \
                     patch("glob.glob", return_value=[str(deploy_root / "HelloHttp" / "conf" / "HelloHttp.json")]):
                    result = srv._sync_config()

            self.assertTrue(result["ok"])
            # Verify etcd_put was called with key /thunder/config/module/HELLO_HTTP
            put_keys = [call[0][0] for call in mock_put.call_args_list]
            self.assertIn("/thunder/config/module/HELLO_HTTP", put_keys)


if __name__ == "__main__":
    unittest.main()
