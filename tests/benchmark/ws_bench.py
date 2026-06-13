#!/usr/bin/env python3
"""
Thunder WebSocket / WebSocket-Secure benchmark — persistent-connection echo throughput.
Usage:
    python3 ws_bench.py [--host H] [--port P] [--conns N] [--duration S] [--tls]
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import ssl
import statistics
import struct
import sys
import threading
import time
from dataclasses import dataclass, field


# ── wire helpers (同 ws_client.py) ────────────────────────────────────────────

def _mask(key: bytes, data: bytes) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        out[i] = b ^ key[i % 4]
    return bytes(out)


def _send_frame(sock: socket.socket, payload: bytes) -> None:
    key = os.urandom(4)
    masked = _mask(key, payload)
    ln = len(masked)
    b0 = 0x82  # FIN + binary
    if ln < 126:
        hdr = struct.pack("!BB", b0, 0x80 | ln)
    elif ln < 65536:
        hdr = struct.pack("!BBH", b0, 0xFE, ln)
    else:
        hdr = struct.pack("!BBQ", b0, 0xFF, ln)
    sock.sendall(hdr + key + masked)


def _recv_exactly(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def _recv_frame(sock: socket.socket) -> bytes:
    b0, b1 = _recv_exactly(sock, 2)
    ln = b1 & 0x7F
    if ln == 126:
        ln = int.from_bytes(_recv_exactly(sock, 2), "big")
    elif ln == 127:
        ln = int.from_bytes(_recv_exactly(sock, 8), "big")
    if b1 & 0x80:
        mask = _recv_exactly(sock, 4)
        return _mask(mask, _recv_exactly(sock, ln))
    return _recv_exactly(sock, ln)


def _handshake(sock: socket.socket, host: str, port: int, path: str) -> None:
    ws_key = base64.b64encode(os.urandom(16)).decode("ascii")
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
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += sock.recv(4096)
    status = buf.split(b"\r\n", 1)[0].decode()
    if " 101 " not in status:
        raise ConnectionError(f"WS handshake failed: {status}")


def _build_msg(cmd: int, seq: int, body_bytes: bytes) -> bytes:
    hdr = struct.pack("!BBHHII", 1, 0, cmd, 0, len(body_bytes), seq)
    return hdr + body_bytes


# ── worker thread ──────────────────────────────────────────────────────────────

@dataclass
class WorkerResult:
    count: int = 0
    latencies_ms: list[float] = field(default_factory=list)
    errors: int = 0


def run_worker(
    host: str, port: int, path: str,
    payload: bytes, duration: float,
    result: WorkerResult, stop: threading.Event,
    tls: bool = False,
) -> None:
    raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    raw.settimeout(5.0)
    try:
        raw.connect((host, port))
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock: socket.socket = ctx.wrap_socket(raw, server_hostname=host)
        else:
            sock = raw
        _handshake(sock, host, port, path)
        sock.settimeout(2.0)
    except Exception as e:
        result.errors += 1
        return

    seq = 0
    msg = _build_msg(20001, seq, payload)

    while not stop.is_set():
        try:
            t0 = time.perf_counter()
            _send_frame(sock, msg)
            _recv_frame(sock)
            dt = (time.perf_counter() - t0) * 1000  # ms
            result.count += 1
            result.latencies_ms.append(dt)
            seq = (seq + 1) & 0xFFFFFFFF
            msg = _build_msg(20001, seq, payload)
        except Exception:
            result.errors += 1
            break

    sock.close()


# ── bench one scenario ─────────────────────────────────────────────────────────

def bench(host: str, port: int, path: str,
          conns: int, duration: float, body_bytes: bytes, label: str,
          tls: bool = False) -> dict:
    results = [WorkerResult() for _ in range(conns)]
    stop = threading.Event()
    threads = [
        threading.Thread(target=run_worker,
                         args=(host, port, path, body_bytes, duration, results[i], stop),
                         kwargs={"tls": tls},
                         daemon=True)
        for i in range(conns)
    ]

    for t in threads:
        t.start()
    time.sleep(duration)
    stop.set()
    for t in threads:
        t.join(timeout=3)

    total = sum(r.count for r in results)
    errors = sum(r.errors for r in results)
    all_lat = []
    for r in results:
        all_lat.extend(r.latencies_ms)

    rps = total / duration
    p50 = statistics.median(all_lat) if all_lat else 0
    p99 = sorted(all_lat)[int(len(all_lat) * 0.99)] if len(all_lat) > 10 else 0

    print(f"  {label:10s}  conns={conns:3d}  "
          f"RPS={rps:8,.0f}  p50={p50*1000:6.0f}µs  p99={p99*1000:6.0f}µs  "
          f"errors={errors}")
    return {"label": label, "conns": conns, "rps": rps, "p50_us": p50 * 1000, "p99_us": p99 * 1000}


# ── main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27010)
    ap.add_argument("--path", default="/hello/shake")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--conns", type=int, default=0,
                    help="0 = run standard suite (10/100)")
    ap.add_argument("--tls", action="store_true", help="Use TLS (WSS)")
    args = ap.parse_args()

    payloads = [
        ("64B",   json.dumps({"option": "Echo", "data": "x" * 28},
                              separators=(",", ":")).encode()),
        ("256B",  json.dumps({"option": "Echo", "data": "x" * 220},
                              separators=(",", ":")).encode()),
        ("1KB",   json.dumps({"option": "Echo", "data": "x" * 990},
                              separators=(",", ":")).encode()),
        ("4KB",   json.dumps({"option": "Echo", "data": "x" * 4060},
                              separators=(",", ":")).encode()),
    ]
    conn_levels = [args.conns] if args.conns else [10, 100]
    proto = "WSS" if args.tls else "WS"

    print(f"\nThunder {proto} Benchmark — {args.host}:{args.port}{args.path}")
    print(f"duration={args.duration}s per run\n")

    rows: list[dict] = []
    for label, body in payloads:
        print(f"[{label}]  wire={len(body)+14}B")
        for c in conn_levels:
            rows.append(bench(args.host, args.port, args.path,
                               c, args.duration, body, label, tls=args.tls))
        print()

    # summary table
    print("\n── Summary ──────────────────────────────────────────────────────")
    print(f"{'Payload':>8}  {'Conns':>5}  {'RPS':>10}  {'p50 lat':>9}  {'p99 lat':>9}")
    print("-" * 52)
    for r in rows:
        print(f"{r['label']:>8}  {r['conns']:>5}  {r['rps']:>10,.0f}  "
              f"{r['p50_us']:>7.0f}µs  {r['p99_us']:>7.0f}µs")


if __name__ == "__main__":
    main()
