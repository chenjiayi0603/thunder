#!/usr/bin/env python3
"""Thunder Admin Web + SO 上传服务

用法:
  ./server.py --port 8090
"""

import argparse, http.server, json, os, socket
from pathlib import Path

NFS_DIR = Path("/data/thunder/plugins")

TYPE_DIRS = {
    "HELLO": "HelloHttp",
    "LOGIC": "Logic",
    "INTERFACE": "Interface",
    "HELLO_WS": "HelloWs",
    "HELLO_HTTPS": "HelloHttps",
}


class UploadServer(http.server.SimpleHTTPRequestHandler):
    """静态文件服务器 + PUT 上传"""

    upload_base = None  # 上传目标根目录

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

    def do_OPTIONS(self):
        """CORS preflight"""
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "PUT, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()


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
