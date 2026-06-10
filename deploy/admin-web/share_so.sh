#!/bin/bash
# 编译机运行: ./share_so.sh ModuleHello_v2.so
# 启动临时 HTTP, 传输一次后自动退出
if [ -z "$1" ] || [ ! -f "$1" ]; then
  echo "用法: ./share_so.sh <so文件> [端口]"
  exit 1
fi
PORT=${2:-9999}
python3 -c "
import http.server, sys, socket
f = sys.argv[1]
size = __import__('os').path.getsize(f)
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', str(size))
        self.send_header('Content-Type', 'application/octet-stream')
        self.end_headers()
        with open(f, 'rb') as fh:
            self.wfile.write(fh.read())
    def log_message(self, *a): pass
s = http.server.HTTPServer(('0.0.0.0', int(sys.argv[2])), H)
ip = socket.gethostbyname(socket.gethostname())
print(f'分享中: http://{ip}:{sys.argv[2]}/')
print('在 Admin 页面填入此地址 → 拉取 (传输一次后自动退出)')
s.handle_request()
" "$1" "$PORT"
