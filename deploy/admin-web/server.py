#!/usr/bin/env python3
"""Thunder Admin Web + SO 上传服务

用法:
  ./server.py --port 8090
"""

import argparse, http.server, json, os, socket, urllib.request
from pathlib import Path

NFS_DIR = Path("/data/thunder/plugins")


class UploadServer(http.server.SimpleHTTPRequestHandler):
    """静态文件服务器 + PUT 上传"""

    upload_base = None  # 上传目标根目录

    def do_GET(self):
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
        """PUT /plugins/{TypeDir}/{filename} → 保存到本地 + NFS"""
        path = self.translate_path(self.path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        cl = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(cl)
        # 写本地
        with open(path, "wb") as f:
            f.write(data)
        # 同步写 NFS (k8s 多节点共享), 路径: /plugins/{TypeDir}/{file} → NFS/{TypeDir}/{file}
        if NFS_DIR.exists():
            parts = self.path.strip("/").split("/")
            if len(parts) >= 3 and parts[0] == "plugins":
                nfs_path = os.path.join(str(NFS_DIR), *parts[1:])  # 跳过 "plugins/" 前缀
                try:
                    os.makedirs(os.path.dirname(nfs_path), exist_ok=True)
                    with open(nfs_path, "wb") as f:
                        f.write(data)
                except PermissionError:
                    pass  # NFS 只读, 仅写本地
                except Exception:
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
        rel = f"plugins/{type_dir}/{so_file}"
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
        """写本地 + NFS"""
        dst = self.translate_path("/" + rel.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        if NFS_DIR.exists():
            parts = rel.strip("/").split("/")
            if len(parts) >= 3 and parts[0] == "plugins":
                nfs_path = os.path.join(str(NFS_DIR), *parts[1:])
                os.makedirs(os.path.dirname(nfs_path), exist_ok=True)
                with open(nfs_path, "wb") as f:
                    f.write(data)


def main():
    p = argparse.ArgumentParser(description="Thunder Admin Web Server")
    p.add_argument("--port", type=int, default=8090)
    args = p.parse_args()

    serve_dir = str(Path(__file__).resolve().parent)
    os.chdir(serve_dir)
    UploadServer.upload_base = serve_dir

    import socket
    ip = socket.gethostbyname(socket.gethostname())

    print(f"╔══════════════════════════════════════════╗")
    print(f"║   Thunder SO Upload Server              ║")
    print(f"╠══════════════════════════════════════════╣")
    print(f"║  本地访问: http://127.0.0.1:{args.port}/")
    print(f"║  远程访问: http://{ip}:{args.port}/")
    print(f"║  上传接口: PUT /plugins/{{Type}}/{{file}}.so")
    print(f"║  本地目录: {serve_dir}/plugins/")
    if NFS_DIR.exists():
        print(f"║  NFS 目录: {NFS_DIR}/ (k8s 多节点共享)")
    print(f"╚══════════════════════════════════════════╝")
    print()

    http.server.HTTPServer(("0.0.0.0", args.port), UploadServer).serve_forever()


if __name__ == "__main__":
    main()
