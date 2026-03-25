#!/usr/bin/env bash
# 启动 WebSocket 接入 Hello 节点（deploy/Hello/bin/Hello + conf/HelloWs.json，access_codec=5）
# 若已有 Hello_ws_robot 进程则先结束；握手 /hello/shake 后对 CodecWebSocketJson 二进制帧跑与 test_helloserver.sh 同构的 JSON 业务体（option 字段）；
# 脚本退出时 pkill Hello_ws_robot，避免残留。
#
# 用法: ./test_helloserver_ws.sh
#       CONF=conf/HelloWs.json ./test_helloserver_ws.sh
#
# 环境变量:
#   HELLO_HOST HELLO_PORT HELLO_SHAKE_PATH — 默认 127.0.0.1 27010 /hello/shake（与 deploy/Hello/conf/HelloWs.json 一致）
#   CURL_MAXTIME_WS           — 单次 socket 超时秒（默认 60）
#   STARTUP_WAIT_SEC          — 启动后等待秒（默认 2）
#   HELLO_WS_CMD              — WS 业务帧命令字（默认 20001，与 Hello.json 中 CmdHello 一致）
#   HELLO_WS_EXPECT_HELLO_JSON — 默认 1：按 HTTP 冒烟方式校验应答 JSON（依赖 CmdHello.so，配置 load:true）
#                               置 0 时仅校验 CMD_RSP_SYS_ERROR(1000)（无 Cmd 处理时）
#
# 依赖: python3

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/HelloWs.json}"

HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27010}"
HELLO_SHAKE_PATH="${HELLO_SHAKE_PATH:-/hello/shake}"
CURL_MAXTIME_WS="${CURL_MAXTIME_WS:-60}"
HELLO_WS_CMD="${HELLO_WS_CMD:-20001}"
HELLO_WS_EXPECT_HELLO_JSON="${HELLO_WS_EXPECT_HELLO_JSON:-1}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

_stop_hello_ws_robot() {
  echo "=== 测试结束，停止 Hello WebSocket 进程（匹配 HelloWs.json）===" >&2
  pkill -f "HelloWs.json" 2>/dev/null || true
  sleep 1 || true
}
trap '_stop_hello_ws_robot' EXIT INT TERM

command -v python3 >/dev/null 2>&1 || {
  echo "错误: 需要 python3" >&2
  exit 1
}

echo "=== 若已有 Hello WebSocket（HelloWs.json）则先停止 ==="
pkill -f "HelloWs.json" 2>/dev/null || true
sleep 1

cd "${DEPLOY_ROOT}/Hello"
mkdir -p log

if [[ ! -x "${DEPLOY_ROOT}/Hello/bin/Hello" ]]; then
  echo "错误: 缺少可执行文件 ${DEPLOY_ROOT}/Hello/bin/Hello" >&2
  exit 1
fi

echo "=== 后台启动 Hello WebSocket 节点 (${CONF}) ==="
nohup "${DEPLOY_ROOT}/Hello/bin/Hello" "${CONF}" >> log/test_helloserver_ws.log 2>&1 &
echo "PID=$! 日志: ${DEPLOY_ROOT}/Hello/log/test_helloserver_ws.log"

sleep "${STARTUP_WAIT_SEC:-2}"

export HELLO_HOST HELLO_PORT HELLO_SHAKE_PATH CURL_MAXTIME_WS HELLO_WS_CMD HELLO_WS_EXPECT_HELLO_JSON

python3 <<'PY'
import base64
import hashlib
import json
import os
import socket
import struct
import sys

# 与 code/Net/include/cmd/CW.hpp 一致（仅用于默认模式校验）
CMD_RSP_SYS_ERROR = 1000

host = os.environ.get("HELLO_HOST", "127.0.0.1")
port = int(os.environ.get("HELLO_PORT", "27010"))
path = os.environ.get("HELLO_SHAKE_PATH", "/hello/shake")
timeout_s = float(os.environ.get("CURL_MAXTIME_WS", "60"))
ws_cmd = int(os.environ.get("HELLO_WS_CMD", "20001"))
expect_hello_json = os.environ.get("HELLO_WS_EXPECT_HELLO_JSON", "0") == "1"

guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ws_key = "dGhlIHNhbXBsZSBub25jZQ=="  # RFC 示例 key
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
    # RFC 7230: Connection 可为逗号分隔 token，须含 Upgrade
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
    b0 = 0x80 | 0x02  # FIN + binary
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
    # 帧载荷 = MsgBody.body 的 UTF-8 业务 JSON 原文（与 CodecWebSocketJson 约定一致）
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
            # 仍允许带空 JSON 等，以服务端实现为准；默认 OrdinaryResponse 走 body 时 WS 多为空
            print(f"--- {name}: OK (cmd={cmd_full}, body_len={blen}) ---")
            return
        print(f"--- {name}: OK (cmd={cmd_full}, empty body) ---")


sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(timeout_s)
try:
    sock.connect((host, port))
    do_handshake(sock)

    checks = [
        ("Echo", 101, {"option": "Echo"}, ('"code"', '"msg"')),
        ("TestHelloPoolCpu", 102, {"option": "TestHelloPoolCpu"}, ("TestHelloPoolCpu", "786432")),
        ("TestHelloPoolBlock", 103, {"option": "TestHelloPoolBlock"}, ("TestHelloPoolBlock", '"slept_ms":80')),
        ("UnknownOption", 104, {"option": "NoSuchOption"}, ('"code"',)),
    ]

    for name, seq, inner, needles in checks:
        print(f"=== {name}: WS cmd={ws_cmd} seq={seq} ===")
        send_business(sock, seq, inner)
        if expect_hello_json:
            check_response(name, seq, needles)
        else:
            check_response(name, seq, ())
finally:
    sock.close()

print("=== WebSocket Hello 用例完成；日志见 deploy/Hello/log/test_helloserver_ws.log ===")
PY

echo "提示: 未使用 wrk（WebSocket 与 test_helloserver.sh 的 HTTP wrk 场景不同）。" >&2
echo "=== 全部完成 ==="
