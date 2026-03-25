#!/usr/bin/env bash
# Hello WebSocket 接入冒烟（Docker Compose / 宿主机直连）
#
# 假定 WebSocket 节点已监听（与 deploy/Hello/conf/HelloWs.json 一致：access_codec=5，默认 127.0.0.1:27010）。
# 注意：docker compose 的 hello 服务默认只执行 start.sh（HTTP Hello）；要测 WS 请在容器内额外执行:
#   docker compose exec hello bash -lc 'cd /thunder/deploy/Hello && ./start_ws.sh'
# 或在宿主机 deploy/Hello 下先 ./start_ws.sh（与 compose 同为 host 网络时端口一致）。
#
# 本脚本只在宿主机用 python3 发 WebSocket 二进制帧，不要求 Hello 可执行文件在宿主机。
# 卷挂载 ../../:/thunder 时建议存在:
#   deploy/Hello/plugins/CmdHello.so
#   deploy/Hello/plugins/ModuleShake.so
#
# 用法（在 deploy/docker 下）:
#   ./test_helloserver_ws_smoke.sh
#
# 可选环境变量:
#   HELLO_HOST HELLO_PORT HELLO_SHAKE_PATH — 默认 127.0.0.1:27010 /hello/shake
#   CURL_MAXTIME_WS              — socket 超时秒数（默认 60，与 deploy/tests/test_helloserver_ws.sh 一致）
#   PRE_CURL_SEC                 — 连接前额外 sleep 秒数（默认 0）
#   REQUIRE_PORTS                — 为 1 时先检查 WS 端口已 LISTEN（默认 0）
#   SKIP_PLUGIN_CHECK            — 为 1 时跳过 .so 存在性检查（默认 0）
#   HELLO_WS_CMD                 — 业务帧命令字（默认 20001）
#   HELLO_WS_EXPECT_HELLO_JSON   — 默认 1：校验 JSON 应答（需 CmdHello 已加载）
#   HELLO_TEST_REDIS_MYSQL       — 为 1 时额外测 TestHelloCoRedis / TestHelloCoMysql（默认 0）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27010}"
HELLO_SHAKE_PATH="${HELLO_SHAKE_PATH:-/hello/shake}"

CURL_MAXTIME_WS="${CURL_MAXTIME_WS:-60}"
PRE_CURL_SEC="${PRE_CURL_SEC:-0}"
REQUIRE_PORTS="${REQUIRE_PORTS:-0}"
SKIP_PLUGIN_CHECK="${SKIP_PLUGIN_CHECK:-0}"
HELLO_WS_CMD="${HELLO_WS_CMD:-20001}"
HELLO_WS_EXPECT_HELLO_JSON="${HELLO_WS_EXPECT_HELLO_JSON:-1}"
HELLO_TEST_REDIS_MYSQL="${HELLO_TEST_REDIS_MYSQL:-0}"

PLUGIN_CMD_HELLO="${DEPLOY_ROOT}/Hello/plugins/CmdHello.so"
PLUGIN_SHAKE="${DEPLOY_ROOT}/Hello/plugins/ModuleShake.so"

_tcp_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -tln 2>/dev/null | grep -q ":${port} " && return 0
  fi
  if command -v nc >/dev/null 2>&1; then
    nc -z 127.0.0.1 "${port}" 2>/dev/null && return 0
  fi
  return 1
}

command -v python3 >/dev/null 2>&1 || {
  echo "错误: 需要 python3（与 deploy/tests/test_helloserver_ws.sh 一致）" >&2
  exit 1
}

if [[ "${SKIP_PLUGIN_CHECK}" != "1" ]]; then
  missing=()
  [[ -f "${PLUGIN_CMD_HELLO}" ]] || missing+=("${PLUGIN_CMD_HELLO}")
  [[ -f "${PLUGIN_SHAKE}" ]] || missing+=("${PLUGIN_SHAKE}")
  if ((${#missing[@]} > 0)); then
    echo "错误: 缺少 WebSocket Hello 依赖插件:" >&2
    for f in "${missing[@]}"; do echo "  - ${f}" >&2; done
    echo "请编译安装 CmdHello、ModuleShake，或 SKIP_PLUGIN_CHECK=1 ./test_helloserver_ws_smoke.sh" >&2
    exit 1
  fi
fi

if [[ "${REQUIRE_PORTS}" == "1" ]]; then
  if ! _tcp_listening "${HELLO_PORT}"; then
    echo "错误: ${HELLO_HOST}:${HELLO_PORT} 未监听（请先启动 WS 节点：deploy/Hello/start_ws.sh 或容器内同等操作）" >&2
    exit 1
  fi
  echo "已检测到端口监听: ${HELLO_PORT}"
fi

if [[ "${PRE_CURL_SEC}" != "0" ]]; then
  echo "=== PRE_CURL_SEC=${PRE_CURL_SEC}s，等待后再连 WebSocket ==="
  sleep "${PRE_CURL_SEC}"
fi

export HELLO_HOST HELLO_PORT HELLO_SHAKE_PATH CURL_MAXTIME_WS HELLO_WS_CMD HELLO_WS_EXPECT_HELLO_JSON HELLO_TEST_REDIS_MYSQL

python3 <<'PY'
import base64
import hashlib
import json
import os
import socket
import struct
import sys

CMD_RSP_SYS_ERROR = 1000

host = os.environ.get("HELLO_HOST", "127.0.0.1")
port = int(os.environ.get("HELLO_PORT", "27010"))
path = os.environ.get("HELLO_SHAKE_PATH", "/hello/shake")
timeout_s = float(os.environ.get("CURL_MAXTIME_WS", "60"))
ws_cmd = int(os.environ.get("HELLO_WS_CMD", "20001"))
expect_hello_json = os.environ.get("HELLO_WS_EXPECT_HELLO_JSON", "1") == "1"
extra_redis_mysql = os.environ.get("HELLO_TEST_REDIS_MYSQL", "0") == "1"

guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ws_key = "dGhlIHNhbXBsZSBub25jZQ=="
expected_accept = base64.b64encode(hashlib.sha1((ws_key + guid).encode("ascii")).digest()).decode("ascii")


def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(code)


def recv_http_headers(sock: socket.socket) -> str:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, _ = data.partition(b"\r\n\r\n")
    return head.decode("iso-8859-1", errors="replace")


def parse_headers(block: str) -> dict:
    h = {}
    for ln in block.split("\r\n")[1:]:
        if not ln or ":" not in ln:
            continue
        k, v = ln.split(":", 1)
        h[k.strip().lower()] = v.strip()
    return h


def connection_has_upgrade_token(conn_val: str) -> bool:
    parts = [p.strip().lower() for p in conn_val.replace(",", " ").split() if p.strip()]
    return "upgrade" in parts


def build_client_msg_head(version: int, encript: int, cmd: int, checksum: int, body_len: int, seq: int) -> bytes:
    return struct.pack("!BBHHII", version, encript, cmd, checksum, body_len, seq)


def mask_payload(key: bytes, payload: bytes) -> bytes:
    out = bytearray(len(payload))
    for i, b in enumerate(payload):
        out[i] = b ^ key[i % 4]
    return bytes(out)


def send_ws_binary_masked(sock: socket.socket, payload: bytes) -> None:
    key = os.urandom(4)
    masked = mask_payload(key, payload)
    ln = len(masked)
    b0 = 0x80 | 0x02
    if ln < 126:
        header = struct.pack("!BB", b0, 0x80 | ln)
    elif ln < 65536:
        header = struct.pack("!BBH", b0, 0x80 | 126, ln)
    else:
        header = struct.pack("!BBQ", b0, 0x80 | 127, ln)
    sock.sendall(header + key + masked)


def recv_ws_frame(sock: socket.socket) -> tuple[int, bytes]:
    hdr = sock.recv(2)
    if len(hdr) < 2:
        die("对端关闭或数据不足（帧头）")
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    ln = b1 & 0x7F
    if ln == 126:
        e = sock.recv(2)
        if len(e) < 2:
            die("对端关闭（扩展长度）")
        ln = int.from_bytes(e, "big")
    elif ln == 127:
        e = sock.recv(8)
        if len(e) < 8:
            die("对端关闭（64 位长度）")
        ln = int.from_bytes(e, "big")
    mask = sock.recv(4) if masked else b""
    if masked and len(mask) < 4:
        die("对端关闭（mask key）")
    buf = bytearray()
    while len(buf) < ln:
        chunk = sock.recv(ln - len(buf))
        if not chunk:
            die("对端关闭（payload）")
        buf.extend(chunk)
    data = bytes(buf)
    if masked:
        data = mask_payload(mask, data)
    return opcode, data


def do_handshake(sock: socket.socket) -> None:
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        f"Sec-WebSocket-Key: {ws_key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    ).encode("ascii")
    sock.sendall(req)
    block = recv_http_headers(sock)
    print(block)
    print("")
    lines = block.split("\r\n")
    if not lines or not lines[0].startswith("HTTP/"):
        die("错误: 无效 HTTP 响应行")
    parts = lines[0].split()
    code = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else 0
    if code != 101:
        die(f"错误: 期望 101，实际 {code}（检查 ModuleShake 与 {path}）")
    headers = parse_headers(block)
    if headers.get("upgrade", "").lower() != "websocket":
        die("错误: 缺少 Upgrade: websocket")
    conn = headers.get("connection", "")
    if not conn or not connection_has_upgrade_token(conn):
        die(f"错误: Connection 须包含 Upgrade token，当前: {conn!r}")
    if headers.get("sec-websocket-accept", "") != expected_accept:
        die(f"错误: Sec-WebSocket-Accept 不匹配，期望 {expected_accept}")
    cl = headers.get("content-length")
    if cl is not None and cl.strip() not in ("0", ""):
        die(f"错误: 101 响应不应带非空 Content-Length: {cl!r}")


def send_business(sock: socket.socket, seq: int, inner: dict) -> None:
    body_b = json.dumps(inner, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    head = build_client_msg_head(1, 0, ws_cmd, 0, len(body_b), seq)
    send_ws_binary_masked(sock, head + body_b)


def check_response(name: str, seq: int, needles: tuple[str, ...]) -> None:
    opcode, payload = recv_ws_frame(sock)
    if opcode != 2:
        die(f"错误: {name} 期望二进制帧 opcode=2，实际 {opcode}")
    if len(payload) < 14:
        die(f"错误: {name} 应答过短 len={len(payload)}")
    ver, enc, cmd16, csum, blen, rseq = struct.unpack("!BBHHII", payload[:14])
    body = payload[14 : 14 + blen]
    if rseq != seq:
        die(f"错误: {name} seq 不匹配 期望 {seq} 实际 {rseq}")
    cmd_full = (enc << 24) | cmd16
    if expect_hello_json:
        if not body:
            die(f"错误: {name} HELLO_WS_EXPECT_HELLO_JSON=1 但包体为空")
        try:
            text = body.decode("utf-8")
        except UnicodeDecodeError:
            die(f"错误: {name} 包体非 UTF-8")
        print(text)
        for nd in needles:
            if nd not in text:
                die(f"错误: {name} 应答未包含片段: {nd!r}")
    else:
        if cmd_full != CMD_RSP_SYS_ERROR:
            die(
                f"错误: {name} 期望框架 CMD_RSP_SYS_ERROR({CMD_RSP_SYS_ERROR})，"
                f"实际 cmd={cmd_full}（若已加载 CmdHello 可设 HELLO_WS_EXPECT_HELLO_JSON=1）"
            )
        if blen != 0:
            print(f"--- {name}: OK (cmd={cmd_full}, body_len={blen}) ---")
            return
        print(f"--- {name}: OK (cmd={cmd_full}, empty body) ---")


checks = [
    ("Echo", 101, {"option": "Echo"}, ('"code"', '"msg"')),
    ("TestHelloPoolCpu", 102, {"option": "TestHelloPoolCpu"}, ("TestHelloPoolCpu", "786432")),
    ("TestHelloPoolBlock", 103, {"option": "TestHelloPoolBlock"}, ("TestHelloPoolBlock", '"slept_ms":80')),
]

if extra_redis_mysql:
    checks.extend(
        [
            (
                "TestHelloCoRedis",
                104,
                {"option": "TestHelloCoRedis"},
                ('"option":"TestHelloCoRedis"', '"get_ok":1'),
            ),
            (
                "TestHelloCoMysql",
                105,
                {"option": "TestHelloCoMysql"},
                ('"option":"TestHelloCoMysql"', '"create_ok":1', '"insert_ok":1', '"select_ok":1'),
            ),
        ]
    )

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(timeout_s)
try:
    print(f"=== 连接 WebSocket ws://{host}:{port}{path}（业务 cmd={ws_cmd}）===")
    sock.connect((host, port))
    do_handshake(sock)

    for name, seq, inner, needles in checks:
        print(f"=== {name}: WS cmd={ws_cmd} seq={seq} ===")
        send_business(sock, seq, inner)
        if expect_hello_json:
            check_response(name, seq, needles)
        else:
            check_response(name, seq, ())
finally:
    sock.close()

if not extra_redis_mysql:
    print("=== 跳过 Redis/MySQL 协程用例；如需启用请设置 HELLO_TEST_REDIS_MYSQL=1 ===")
print("=== Hello WebSocket 冒烟通过 ===")
PY
