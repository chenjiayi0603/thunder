from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import time
from dataclasses import dataclass


@dataclass
class WsResponse:
    opcode: int
    seq: int
    body_text: str


def _mask_payload(key: bytes, payload: bytes) -> bytes:
    out = bytearray(len(payload))
    for i, b in enumerate(payload):
        out[i] = b ^ key[i % 4]
    return bytes(out)


def _recv_http_headers(sock: socket.socket) -> str:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, _ = data.partition(b"\r\n\r\n")
    return head.decode("iso-8859-1", errors="replace")


def _parse_headers(block: str) -> dict[str, str]:
    headers: dict[str, str] = {}
    for ln in block.split("\r\n")[1:]:
        if ":" not in ln:
            continue
        k, v = ln.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    return headers


def _send_ws_binary_masked(sock: socket.socket, payload: bytes) -> None:
    key = os.urandom(4)
    masked = _mask_payload(key, payload)
    ln = len(masked)
    b0 = 0x80 | 0x02
    if ln < 126:
        header = struct.pack("!BB", b0, 0x80 | ln)
    elif ln < 65536:
        header = struct.pack("!BBH", b0, 0x80 | 126, ln)
    else:
        header = struct.pack("!BBQ", b0, 0x80 | 127, ln)
    sock.sendall(header + key + masked)


def _recv_ws_frame(sock: socket.socket) -> tuple[int, bytes]:
    hdr = sock.recv(2)
    if len(hdr) < 2:
        raise AssertionError("websocket frame header too short")
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    ln = b1 & 0x7F
    if ln == 126:
        ln = int.from_bytes(sock.recv(2), "big")
    elif ln == 127:
        ln = int.from_bytes(sock.recv(8), "big")
    mask = sock.recv(4) if masked else b""
    body = bytearray()
    while len(body) < ln:
        chunk = sock.recv(ln - len(body))
        if not chunk:
            raise AssertionError("websocket payload truncated")
        body.extend(chunk)
    data = bytes(body)
    if masked:
        data = _mask_payload(mask, data)
    return opcode, data


class RawWsClient:
    def __init__(self, host: str, port: int, path: str, timeout_s: float = 20.0) -> None:
        self.host = host
        self.port = port
        self.path = path
        self.timeout_s = timeout_s
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout_s)

    def connect(self) -> None:
        deadline = time.time() + self.timeout_s
        last_status = ""
        while time.time() < deadline:
            try:
                self.sock.connect((self.host, self.port))
                ws_key = "dGhlIHNhbXBsZSBub25jZQ=="
                req = (
                    f"GET {self.path} HTTP/1.1\r\n"
                    f"Host: {self.host}:{self.port}\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: websocket\r\n"
                    f"Sec-WebSocket-Key: {ws_key}\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n"
                ).encode("ascii")
                self.sock.sendall(req)
                block = _recv_http_headers(self.sock)
                status = block.split("\r\n", 1)[0]
                last_status = status
                if " 101 " in status:
                    headers = _parse_headers(block)
                    assert headers.get("upgrade", "").lower() == "websocket"
                    expected = base64.b64encode(
                        hashlib.sha1((ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
                    ).decode("ascii")
                    assert headers.get("sec-websocket-accept", "") == expected
                    return
            except OSError:
                pass
            # 连接失败或返回 404 时，短暂重试（服务启动/插件装载窗口）。
            try:
                self.sock.close()
            finally:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(self.timeout_s)
            time.sleep(0.5)
        raise AssertionError(f"handshake failed before timeout: {last_status or 'connect error'}")

    def send_json_body(self, cmd: int, seq: int, body: dict[str, str]) -> WsResponse:
        body_b = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        head = struct.pack("!BBHHII", 1, 0, cmd, 0, len(body_b), seq)
        _send_ws_binary_masked(self.sock, head + body_b)
        opcode, payload = _recv_ws_frame(self.sock)
        assert len(payload) >= 14, "response payload too short"
        _, _, _, _, blen, rseq = struct.unpack("!BBHHII", payload[:14])
        assert rseq == seq, f"seq mismatch: {rseq}!={seq}"
        text = payload[14 : 14 + blen].decode("utf-8", errors="replace")
        return WsResponse(opcode=opcode, seq=rseq, body_text=text)

    def close(self) -> None:
        self.sock.close()

