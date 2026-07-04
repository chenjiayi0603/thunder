#!/usr/bin/env python3
"""Thunder Admin Web + SO 上传服务

用法:
  ./server.py --port 8090
"""

import argparse, base64, http.server, json, os, socket, urllib.request
from pathlib import Path

NFS_DIR = Path("/data/thunder/plugins")
ETCD_URL = os.environ.get("ETCD_URL", "http://127.0.0.1:2379")
TYPE_DIR = {
    "HELLO_HTTP": "HelloHttp", "HELLO_HTTPS": "HelloHttps",
    "HELLO_WS": "HelloWs", "LOGIC": "Logic", "INTERFACE": "Interface",
}


def _etcd_get(key: str) -> str | None:
    payload = json.dumps({"key": base64.b64encode(key.encode()).decode()}).encode()
    req = urllib.request.Request(ETCD_URL + "/v3/kv/range", data=payload,
                                 headers={"Content-Type": "application/json"})
    try:
        data = json.loads(urllib.request.urlopen(req, timeout=5).read())
        kvs = data.get("kvs", [])
        if kvs:
            return base64.b64decode(kvs[0]["value"]).decode()
    except Exception:
        pass
    return None


def _etcd_put(key: str, value: str) -> bool:
    payload = json.dumps({
        "key": base64.b64encode(key.encode()).decode(),
        "value": base64.b64encode(value.encode()).decode(),
    }).encode()
    req = urllib.request.Request(ETCD_URL + "/v3/kv/put", data=payload,
                                 headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=5)
        return True
    except Exception:
        return False


class UploadServer(http.server.SimpleHTTPRequestHandler):
    """静态文件服务器 + PUT 上传"""

    upload_base = None  # 上传目标根目录

    def do_GET(self):
        """GET /api/lua-scripts?node_type=HELLO_HTTP — 列出 Lua 脚本及版本"""
        if self.path.startswith("/api/lua-scripts"):
            from urllib.parse import parse_qs, unquote
            qs = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = {k: unquote(v[0]) for k, v in parse_qs(qs).items()}
            node_type = params.get("node_type", "HELLO_HTTP")
            scripts = self._lua_list(node_type)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"scripts": scripts}).encode())
            return
        """GET /api/so-images — 列出可用 SO 镜像"""
        if self.path == "/api/so-images":
            images = []
            try:
                import docker
                c = docker.from_env()
                for img in c.images.list():
                    for tag in img.tags:
                        if tag and "so" in tag:
                            images.append({
                                "tag": tag,
                                "size": img.attrs.get("Size", 0),
                                "created": img.attrs.get("Created", ""),
                            })
                # 按创建时间倒序 (最新在前)
                images.sort(key=lambda x: x.get("created", ""), reverse=True)
            except Exception:
                pass
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"images": images}).encode())
            return
        # GET /api/so-files?image=xxx → 列出镜像内 .so 文件
        if self.path.startswith("/api/so-files"):
            from urllib.parse import parse_qs, unquote
            qs = self.path.split("?", 1)[1] if "?" in self.path else ""
            params = {k: unquote(v[0]) for k, v in parse_qs(qs).items()}
            image = params.get("image", "")
            files = []
            if image:
                try:
                    import docker, tarfile, io
                    c = docker.from_env()
                    ctr = c.containers.create(image)
                    try:
                        bits, _ = ctr.get_archive("/app/so")
                        buf = io.BytesIO()
                        for chunk in bits: buf.write(chunk)
                        buf.seek(0)
                        tf = tarfile.open(fileobj=buf)
                        for member in tf:
                            name = member.name.split("/")[-1]
                            if name.endswith(".so"):
                                files.append(name)
                    finally:
                        ctr.remove()
                except Exception as e:
                    print(f"so-files error: {e}")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"files": files}).encode())
            return
        return super().do_GET()

    def do_PUT(self):
        """PUT /plugins/{TypeDir}/{filename} → deploy/{TypeDir}/plugins/{filename} + NFS"""
        parts = self.path.strip("/").split("/")
        if len(parts) < 3 or parts[0] != "plugins":
            self.send_error(400, "path must be /plugins/{TypeDir}/{filename}")
            return
        type_dir = parts[1]
        filename = "/".join(parts[2:])
        base = UploadServer.upload_base or os.getcwd()
        # 写本地：deploy/{TypeDir}/plugins/{filename}
        path = os.path.join(base, type_dir, "plugins", filename)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        cl = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(cl)
        with open(path, "wb") as f:
            f.write(data)
        # 同步写 NFS (k8s 多节点共享): NFS/{TypeDir}/{filename}
        if NFS_DIR.exists():
            nfs_path = os.path.join(str(NFS_DIR), type_dir, filename)
            try:
                os.makedirs(os.path.dirname(nfs_path), exist_ok=True)
                with open(nfs_path, "wb") as f:
                    f.write(data)
            except (PermissionError, Exception):
                pass
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({
            "ok": True,
            "path": self.path,
            "size": len(data),
        }).encode())

    def do_POST(self):
        """POST /api/lua-scripts — 上传 Lua 脚本并推送到 etcd"""
        if self.path == "/api/lua-scripts":
            cl = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(cl))
            node_type = body.get("node_type", "HELLO_HTTP")
            name = body.get("name", "").strip()   # e.g. "echo.lua"
            content = body.get("content", "")
            url_path = body.get("url_path", "")   # e.g. "/hello/lua_echo"
            if not name or not content or not url_path:
                self.send_error(400, "need name, content, url_path")
                return
            result = self._lua_push(node_type, name, content, url_path)
            self.send_response(200 if result["ok"] else 502)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(result).encode())
            return
        """POST /api/sync-config — 从本地配置文件初始化 etcd module 配置"""
        if self.path == "/api/sync-config":
            result = self._sync_config()
            self.send_response(200 if result["ok"] else 502)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(result).encode())
            return
        """POST /api/so-extract — 从 SO 镜像提取文件
           POST /api/fetch      — 从远端 URL 拉取 SO 文件"""
        if self.path == "/api/so-extract":
            self._handle_so_extract()
            return
        if self.path == "/api/fetch":
            cl = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(cl))
            url = body.get("url", "")
            rel = body.get("path", "")
            if not url or not rel:
                self.send_error(400, "need url and path")
                return
            # 下载
            try:
                req = urllib.request.Request(url)
                data = urllib.request.urlopen(req, timeout=30).read()
            except Exception as e:
                self.send_error(502, f"download failed: {e}")
                return
            # 写本地
            dst = self.translate_path("/" + rel.lstrip("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
            # 写 NFS
            if NFS_DIR.exists():
                parts = rel.strip("/").split("/")
                if len(parts) >= 3 and parts[0] == "plugins":
                    nfs_path = os.path.join(str(NFS_DIR), *parts[1:])
                    os.makedirs(os.path.dirname(nfs_path), exist_ok=True)
                    with open(nfs_path, "wb") as f:
                        f.write(data)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps({"ok": True, "path": "/" + rel, "size": len(data)}).encode())
            return
        self.send_error(404)

    def do_OPTIONS(self):
        """CORS preflight"""
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "PUT, GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def _handle_so_extract(self):
        """POST /api/so-extract {image: "reg/so:v3", file: "xxx.so", type: "HELLO"}"""
        cl = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(cl))
        image = body.get("image", "").strip()
        so_file = body.get("file", "").strip()
        node_type = body.get("type", "").strip()
        if not image or not so_file:
            self.send_error(400, "need image and file")
            return
        type_dir = {"HELLO_HTTP":"HelloHttp","HELLO_HTTPS":"HelloHttps","HELLO_WS":"HelloWs",
                    "LOGIC":"Logic","INTERFACE":"Interface"}.get(node_type, node_type)
        rel = f"{type_dir}/plugins/{so_file}"
        try:
            import docker, tarfile, io
            client = docker.from_env()
            # registry 镜像先拉取
            if "/" in image:
                try:
                    client.images.get(image)
                except docker.errors.ImageNotFound:
                    client.images.pull(image)
            ctr = client.containers.create(image)
            try:
                bits, _ = ctr.get_archive(f"/app/so/{so_file}")
                buf = io.BytesIO()
                for chunk in bits: buf.write(chunk)
                buf.seek(0)
                tf = tarfile.open(fileobj=buf)
                data = tf.extractfile(so_file).read()
            finally:
                ctr.remove()
        except Exception as e:
            self.send_error(502, f"extract failed: {e}")
            return
        self._save_so(data, rel)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps({"ok": True, "path": "/" + rel, "size": len(data)}).encode())

    def _save_so(self, data, rel):
        """写本地 + NFS。rel = {TypeDir}/plugins/{so_file}"""
        base = UploadServer.upload_base or os.getcwd()
        dst = os.path.join(base, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        # NFS: NFS_DIR/{TypeDir}/{filename}（跳过 "plugins" 路径段，与 do_PUT 一致）
        if NFS_DIR.exists():
            parts = rel.strip("/").split("/")  # ["HelloHttp", "plugins", "xxx.so"]
            if len(parts) >= 3:
                nfs_path = os.path.join(str(NFS_DIR), parts[0], *parts[2:])
                os.makedirs(os.path.dirname(nfs_path), exist_ok=True)
                with open(nfs_path, "wb") as f:
                    f.write(data)


    def _sync_config(self) -> dict:
        """从本地配置文件同步 module 配置到 etcd（新服务器初始化时自动调用）"""
        import json as _json, os, glob
        deploy_dir = os.path.join(os.path.dirname(__file__), "..")
        # 每个类型只读一个主配置文件：{TypeDir}/conf/{TypeDir}.json
        # e.g. HelloHttp/conf/HelloHttp.json, Logic/conf/Logic.json
        synced = []; errors = []
        cfg_map = {}  # node_type -> modules
        for node_type, type_dir in TYPE_DIR.items():
            conf_file = os.path.join(deploy_dir, type_dir, "conf", f"{type_dir}.json")
            if not os.path.exists(conf_file):
                # 降级：配目录下第一个有 module 段的 json
                for f in sorted(glob.glob(os.path.join(deploy_dir, type_dir, "conf", "*.json"))):
                    conf_file = f; break
            try:
                with open(conf_file) as f:
                    cfg = _json.load(f)
                mods = cfg.get("module", [])
                if mods:
                    cfg_map[node_type] = mods
            except Exception as e:
                errors.append({"type": node_type, "file": conf_file, "error": str(e)})

        for node_type, modules in cfg_map.items():
            try:
                cfg_key = f"/thunder/config/module/{node_type}"
                val = _json.dumps({"module": modules})
                _etcd_put(cfg_key, val)
                synced.append({"type": node_type, "modules": len(modules)})
            except Exception as e:
                errors.append({"type": node_type, "error": str(e)})

        return {"ok": len(errors) == 0, "synced": synced, "errors": errors}


    def _lua_list(self, node_type: str) -> list:
        """返回 node_type 的 Lua 脚本信息列表（从 etcd 模块配置读取）"""
        cfg_key = f"/thunder/config/module/{node_type}"
        raw = _etcd_get(cfg_key)
        if not raw:
            return []
        try:
            mods = json.loads(raw).get("module", [])
        except Exception:
            return []
        result = []
        for m in mods:
            if "script_path" in m or "script_content" in m:
                result.append({
                    "url_path": m.get("url_path", ""),
                    "script_path": m.get("script_path", ""),
                    "version": m.get("version", 0),
                    "has_inline": bool(m.get("script_content")),
                })
        return result

    def _lua_push(self, node_type: str, name: str, content: str, url_path: str) -> dict:
        """将 Lua 脚本内容写入本地 + 推送到 etcd 模块配置（自动版本递增）"""
        # 1. 写本地脚本文件
        type_dir = TYPE_DIR.get(node_type, node_type)
        serve_dir = Path(UploadServer.upload_base or os.getcwd())
        scripts_dir = serve_dir / type_dir / "scripts"
        scripts_dir.mkdir(parents=True, exist_ok=True)
        script_file = scripts_dir / name
        script_file.write_text(content, encoding="utf-8")

        # 2. 读当前 etcd 模块配置
        cfg_key = f"/thunder/config/module/{node_type}"
        raw = _etcd_get(cfg_key)
        mods = []
        if raw:
            try:
                mods = json.loads(raw).get("module", [])
            except Exception:
                pass

        # 3. 找匹配 url_path 的模块条目并更新
        updated = False
        for m in mods:
            if m.get("url_path") == url_path:
                m["script_content"] = content
                m["version"] = m.get("version", 0) + 1
                updated = True
                break
        if not updated:
            return {"ok": False, "error": f"url_path {url_path!r} not found in etcd module config"}

        # 4. 写回 etcd
        new_val = json.dumps({"module": mods})
        ok = _etcd_put(cfg_key, new_val)
        return {
            "ok": ok,
            "node_type": node_type,
            "url_path": url_path,
            "script": name,
            "size": len(content),
            "version": next(m["version"] for m in mods if m.get("url_path") == url_path),
        }


def main():
    p = argparse.ArgumentParser(description="Thunder Admin Web Server")
    p.add_argument("--port", type=int, default=8090)
    args = p.parse_args()

    serve_dir = str(Path(__file__).resolve().parent.parent)  # deploy/
    os.chdir(serve_dir)
    UploadServer.upload_base = serve_dir  # 下发目标：deploy/{TypeDir}/plugins/ 和 deploy/{TypeDir}/scripts/

    import socket
    ip = socket.gethostbyname(socket.gethostname())

    print(f"╔══════════════════════════════════════════╗")
    print(f"║   Thunder Admin Web Server              ║")
    print(f"╠══════════════════════════════════════════╣")
    print(f"║  本地访问: http://127.0.0.1:{args.port}/")
    print(f"║  远程访问: http://{ip}:{args.port}/")
    print(f"║  Lua 下发: POST /api/lua-scripts → {serve_dir}/{{Type}}/scripts/")
    print(f"║  SO 下发:  POST /api/so-extract  → {serve_dir}/{{Type}}/plugins/")
    if NFS_DIR.exists():
        print(f"║  NFS 目录: {NFS_DIR}/ (k8s 多节点共享)")
    print(f"╚══════════════════════════════════════════╝")
    print()

    http.server.HTTPServer(("0.0.0.0", args.port), UploadServer).serve_forever()


if __name__ == "__main__":
    main()
