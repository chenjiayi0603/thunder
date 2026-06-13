# Thunder

**Thunder** is a high-performance, multi-protocol gateway and distributed service framework built on C++20 coroutines.
It handles HTTP, HTTPS, and WebSocket traffic, routes requests to backend logic nodes via Protobuf RPC,
and extends behavior at runtime through Lua scripts and dynamically loaded `.so` plugins —
all within a single-threaded event loop that processes **230,000+ requests per second** on a single core.

```
┌────────────────────────────────────────────────────────────┐
│                     Thunder Gateway                        │
│                                                            │
│   HTTP ──┐                                                 │
│  HTTPS ──┼──► Worker (event loop)  ──► .so Plugin / Lua   │
│       WS ──┘        │                                      │
│                      ▼                                     │
│              etcd Service Mesh                             │
│                      │                                     │
│           ┌──────────┼──────────┐                          │
│        LOGIC       LOGIC       LOGIC   (C++20 coroutines)  │
└────────────────────────────────────────────────────────────┘
```

---

## Performance

Benchmarked against Nginx 1.x on the same hardware (i9-12900H, 1 worker, `wrk -t4 -c100 -d10s`):

### HTTP (plain text)

| Payload | Thunder (ev) | Thunder (asio io_uring) | Nginx 1w | Thunder vs Nginx |
|--------:|:------------:|:-----------------------:|:--------:|:----------------:|
| 64 B    | 232,000 RPS  | **235,000 RPS**         | 214,000  | **+9~10%**       |
| 256 B   | 230,000 RPS  | 236,000 RPS             | 212,000  | **+8~11%**       |
| 1 KB    | 229,000 RPS  | 232,000 RPS             | 191,000  | **+20~21%**      |
| 4 KB    | 216,000 RPS  | 223,000 RPS             | 184,000  | **+17~21%**      |

**Latency p50** (asio io_uring backend):

**Latency p50** (asio io_uring backend):

| Payload | Thunder ev | Thunder asio io_uring | Nginx 1w | vs Nginx (asio) |
|--------:|:----------:|:---------------------:|:--------:|:---------------:|
| 64 B    | 424 µs     | **220 µs**            | 466 µs   | **2.1× lower**  |
| 4 KB    | 457 µs     | **332 µs**            | 543 µs   | **1.6× lower**  |

### HTTPS (TLS)

**Throughput (RPS)**:

| Payload | Thunder ev | Thunder uring | Nginx SSL |
|--------:|:----------:|:-------------:|:---------:|
| 64 B    | 141,000    | 133,000       | 149,000   |
| 4 KB    | 97,000     | 92,000        | 132,000   |

> Nginx leads in HTTPS throughput — mature OpenSSL path + kernel sendfile advantage.

**Latency p50**:

| Payload | Thunder ev | Thunder uring | Nginx SSL | vs Nginx (uring) |
|--------:|:----------:|:-------------:|:---------:|:----------------:|
| 64 B    | 803 µs     | **402 µs**    | 752 µs    | **1.9× lower**   |
| 4 KB    | 1.23 ms    | **247 µs**    | 824 µs    | **3.3× lower**   |

> **Why Thunder uring beats both ev and Nginx on latency:**
>
> A single HTTPS request triggers 5–10 BIO read/write calls as OpenSSL fragments TLS records.
>
> - **Thunder ev** issues each BIO call as a separate `read`/`write` syscall — serial queue buildup → 803 µs.
> - **Nginx** also runs on epoll internally, so it has the exact same serial BIO cost → 752 µs (similar to Thunder ev).
> - **Thunder uring** submits all pending BIO ops as one batch in a single `io_uring_enter` — they drain in parallel → **402 µs**.
>
> Nginx has no io_uring path; it cannot batch BIO calls. Thunder's pluggable I/O backend is the
> only one here with that capability, which is why it undercuts Nginx latency by **1.9×** at 64 B
> and **3.3×** at 4 KB. Throughput stays flat because SSL decryption (CPU) remains the bottleneck,
> not the I/O queue depth.

### WebSocket (WS) Echo

Measured with a persistent-connection Python benchmark client (`tests/benchmark/ws_bench.py`),
1 Worker, binary echo, via k8s NodePort (adds ~1 ms compared to direct native connection).

**Throughput (RPS)**:

| Payload | 10 conns | 100 conns |
|--------:|:--------:|:---------:|
| 64 B    | 46,765   | 48,736    |
| 256 B   | 33,240   | 33,846    |
| 1 KB    | 15,888   | 15,619    |
| 4 KB    | 4,881    | 5,179     |

**Latency p50 / p99** (10 connections, native path):

| Payload | p50     | p99     |
|--------:|:-------:|:-------:|
| 64 B    | 192 µs  | 549 µs  |
| 256 B   | 270 µs  | 787 µs  |
| 1 KB    | 564 µs  | 1.7 ms  |
| 4 KB    | 1.81 ms | 5.9 ms  |

> Connections are persistent — the upgrade handshake happens once, not once per message.
> The throughput gap vs HTTP is driven by per-frame overhead: every WebSocket message
> carries an 8-byte binary header plus mandatory client-side XOR masking (4-byte key,
> full payload mask loop), CPU work that HTTP keep-alive does not have.

> Hardware: i9-12900H 45 W laptop, thermal wall ~4.0 GHz steady state.
> HTTP/HTTPS methodology: INFO log, performance governor, workers pinned to P-cores, wrk pinned to E-cores.
> Full report: [`docs/reports/10-vs-nginx-benchmark-20260610.md`](docs/reports/10-vs-nginx-benchmark-20260610.md)

### WebSocket Secure (WSS) Echo

Same methodology as WS above but over TLS — `WssCodec` wraps `CodecWebSocketJson` with OpenSSL BIO
memory buffers. Measured via k8s NodePort 30012.

**Throughput (RPS)**:

| Payload | 10 conns | 100 conns |
|--------:|:--------:|:---------:|
| 64 B    | 34,278   | 38,281    |
| 256 B   | 27,550   | 27,202    |
| 1 KB    | 12,838   | 12,078    |
| 4 KB    |  4,386   |  3,762    |

**Latency p50 / p99** (10 connections):

| Payload | p50     | p99     |
|--------:|:-------:|:-------:|
| 64 B    | 253 µs  | 846 µs  |
| 256 B   | 317 µs  | 1.02 ms |
| 1 KB    | 677 µs  | 2.27 ms |
| 4 KB    | 1.97 ms | 6.82 ms |

> WSS vs WS overhead: TLS adds ~6–14% latency and ~5–13% RPS loss at 64 B / 10 conns
> (34 k vs 36 k RPS). At 100 conns and larger payloads the gap narrows — the TLS record
> layer amortises across frames and the bottleneck shifts to application processing.

---

## Why Thunder Is Fast

Thunder achieves its performance through several architectural decisions:

### 1. Zero-Copy Fast Path

Requests matching known route prefixes bypass the full Protobuf + JSON decode stack entirely.
The response is written directly from a pre-built header template with `memcpy`.

```
Normal path:  recv → http_parser → pb decode → handler → pb encode → send   (~162k RPS)
Fast path:    recv → prefix match → handler → template write → send          (~236k RPS)
```

### 2. picohttpparser

Thunder replaced the industry-standard `http_parser` with `picohttpparser` — a single-header
SIMD-accelerated HTTP/1.x parser. **Measured gain: +49% RPS** on the same handler code.

### 3. Pluggable I/O Backends

Four I/O backends compiled at build time, selected via config:

| Backend | Best for | Notes |
|---------|----------|-------|
| `ev` (epoll) | Small–medium payloads, default | Lowest overhead at 100 connections |
| `asio_uring` | Large payloads, latency-sensitive | Batch submission: 2.1× lower p50 latency |
| `native_uring` | HTTPS with many I/Os | Raw io_uring, no ASIO overhead |
| `dpdk` (planned) | Line-rate packet processing | Kernel bypass |

### 4. Multi-Process Worker Model

Each worker is an independent OS process with its own event loop. Processes share nothing;
a crashing plugin `.so` takes down only one worker and is transparently restarted by the Manager.

```
Manager ──fork──► Worker 0  (event loop, plugins, coroutines)
         ──fork──► Worker 1
         ──fork──► Worker N
```

Graceful restart: Manager drains in-flight connections, starts replacement workers,
and only kills old workers after all connections close or timeout.

### 5. C++20 Coroutines Without Callbacks

Business logic is written as straight-line `co_await` code.
The event loop handles all scheduling transparently:

```cpp
net::AsyncTask HandleRequest(net::StepCo20& step) {
    // DB, Redis, cross-node RPC — all look synchronous
    auto rows = co_await db.Query("SELECT * FROM orders WHERE user_id=?", userId);
    bool cached = co_await cache.Set("orders:" + id, rows.toJson(), 300);
    bool notified = co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
    step.Response(200, buildResponse(rows));
}
```

### 6. yyjson — Fastest JSON Library

Thunder uses [yyjson](https://github.com/ibireme/yyjson) (v0.12.0) as its JSON backend:

| Operation | yyjson | cJSON | Speedup |
|-----------|-------:|------:|:-------:|
| Parse (small JSON) | 1,944 MB/s | 474 MB/s | **4.1×** |
| Build (small JSON) | 1,567 MB/s | 134 MB/s | **11.7×** |
| Parse (large JSON) | 3,726 MB/s (On-Demand) | — | — |

---

## Features

### Multi-Protocol Gateway

| Protocol | Status | Notes |
|----------|:------:|-------|
| HTTP/1.1 | ✅ | picohttpparser, keep-alive, chunked |
| HTTPS/TLS | ✅ | OpenSSL, SNI |
| WebSocket (WS) | ✅ | Upgrade, ping/pong, fragmentation |
| WebSocket Secure (WSS) | ✅ | WS over TLS, same OpenSSL stack |
| Internal Protobuf | ✅ | Node-to-node binary RPC |

### Async MySQL & Redis Clients (C++20 Coroutines)

Thunder provides first-class coroutine clients for MySQL and Redis.
No threads blocked, no callbacks — plain `co_await` syntax on the event loop thread:

```cpp
net::AsyncTask HandleOrder(net::StepCo20& step) {
    // MySQL — non-blocking query, event loop continues serving other connections
    MySqlCoHelper db(step, dbConf);
    auto reply = co_await db.Query("SELECT id, name FROM orders WHERE user_id=1");
    if (reply.ok) {
        for (auto& row : reply.rows) { /* row["id"], row["name"] */ }
    }
    co_await db.Exec("UPDATE orders SET status=1 WHERE id=?", orderId);

    // Redis — same co_await pattern
    RedisCoHelper cache(step, "127.0.0.1", 6379);
    co_await cache.Set("order:" + orderId, payload, /*TTL=*/300);
    auto hit = co_await cache.Get("order:" + orderId);
    co_await cache.HSet("user:1", "lastOrder", orderId);
}
```

| Client | Operations | Underlying I/O |
|--------|-----------|----------------|
| `MySqlCoHelper` | `Query`, `Exec` | Thread pool offload → `PostToEventLoop` resume |
| `RedisCoHelper` | `Get`, `Set`, `Del`, `HGet`, `HSet`, `Expire` | Async hiredis → event loop resume |

Both clients suspend the coroutine without blocking the event loop.
A 100-connection benchmark with mixed DB + cache calls shows the same RPS as pure HTTP —
the event loop fills idle wait time with other requests.

### Lua Scripting

Write request handlers in Lua without recompiling. LuaJIT is embedded;
scripts are hot-reloaded at runtime.

```lua
-- route.lua: custom routing logic
function OnRequest(req)
    if req.path:match("^/api/v2/") then
        return "LOGIC_V2"
    end
    return "LOGIC"
end
```

```lua
-- limit.lua: rate limiting
function OnRequest(req)
    local key = "rate:" .. req.remote_ip
    local count = redis_incr(key, 60)   -- async, non-blocking
    if count > 1000 then
        return 429, '{"code":429,"msg":"rate limit exceeded"}'
    end
end
```

Lua modules load alongside C++ `.so` plugins with **zero measured performance overhead**
(confirmed by single-variable benchmark).

### Dynamic `.so` Plugin System

Deploy new business logic without restarting the gateway. The Manager pushes plugin
updates to workers; each worker gracefully drains, hot-swaps the `.so`, and resumes:

```bash
# Upload a new plugin via the Admin API
curl -X POST http://gateway:8090/api/so-extract \
  -F "image=HelloHttp-v1.2" \
  -F "file=ModuleOrder.so"

# Workers reload automatically via etcd watch notification
```

Plugin conventions:
- Entry: `MUDULE_CREATE(PluginName)` macro
- Handler: `bool DoMsg(MsgHead&, MsgBody&)` or HTTP variant
- Thread model: called on the worker's event loop thread — must not block

### etcd Distributed Service Mesh

Thunder uses etcd for service registration, discovery, and configuration:

```
Center nodes (Raft cluster)
  │
  ├── Service registration  (workers announce themselves)
  ├── Service discovery     (route requests by node type)
  ├── Config push           (live config changes, no restart)
  └── Health monitoring     (TTL-based liveness)
```

Cluster topology is fully dynamic: add or remove Logic/Interface/custom node types
at runtime. The routing table converges in milliseconds via etcd watch.

### Admin Web UI

A built-in web dashboard (React + nginx) provides:

- Plugin image management (build, list, extract)
- Live node topology view
- etcd key browser
- Per-worker stats

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                          Client Layer                            │
│              HTTP / HTTPS / WebSocket / Protobuf RPC             │
└───────────────────────────┬──────────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────────┐
│                       Gateway Nodes                              │
│                                                                  │
│  Manager (supervisor)                                            │
│    ├── fork/restart Workers                                      │
│    ├── receive plugin .so updates from etcd                      │
│    └── graceful drain + hot-swap                                 │
│                                                                  │
│  Worker 0..N  (one event loop each)                              │
│    ├── I/O Backend: ev / asio_uring / native_uring               │
│    ├── HTTP Fast Path (picohttpparser + prefix match)            │
│    ├── Codec chain: HTTP → Protobuf → response                   │
│    ├── .so Modules (dynamically loaded)                          │
│    ├── Lua VM (LuaJIT per worker)                                │
│    └── C++20 Coroutine Steps (StepCo20)                          │
│         ├── co_await MySQL / Redis                               │
│         ├── co_await HTTP upstream                               │
│         └── co_await cross-node PB RPC                           │
└───────────────────────────┬──────────────────────────────────────┘
                            │ Internal Protobuf (TCP)
┌───────────────────────────▼──────────────────────────────────────┐
│                        Logic Nodes                               │
│                                                                  │
│  Same Worker architecture; receives routed requests,             │
│  applies business logic, returns Protobuf responses.             │
│  Scaled horizontally; registered in etcd.                        │
└──────────────────────────────────────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────────┐
│                       Center Cluster                             │
│                                                                  │
│  3-node Raft consensus (built-in, no external Zookeeper)         │
│  Responsibilities:                                               │
│    ├── Leader election + log replication                         │
│    ├── Service registry (node type → IP:port:worker)             │
│    ├── etcd-backed config store                                  │
│    └── Plugin distribution (SO images → NFS / node local)       │
└──────────────────────────────────────────────────────────────────┘
```

### Design Principles

| Principle | Implementation |
|-----------|----------------|
| **Share nothing** | One process per worker; no inter-worker locks |
| **No blocking** | `co_await` for all I/O; thread pool for CPU-bound tasks |
| **Zero-copy where possible** | Fast Path bypasses serialize/deserialize; yyjson arena allocator |
| **Fault isolation** | Plugin crash → one worker dies and is restarted; traffic shifts |
| **Runtime extensibility** | Lua for logic; `.so` for performance-critical paths; etcd for config |

---

## Quick Start

### Requirements

- CMake ≥ 3.20
- GCC 12+ or Clang 15+ (C++20)
- OpenSSL dev headers
- Docker + Docker Compose (for E2E tests)

### Build

```bash
git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j$(nproc)
cmake --build build -j$(nproc)
cmake --install build          # installs to deploy/
```

Optional build flags:

```bash
# Enable asio io_uring backend (requires Linux 5.1+ kernel)
cmake -S . -B build -DTHUNDER_IO_ASIO_URING=ON

# Enable LuaJIT module support
cmake -S . -B build -DTHUNDER_LUAJIT=ON
```

### Run

```bash
cd deploy
./nodes.sh restart all         # starts Center + Gateway + Logic nodes

# Smoke test
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo","data":"hi"}'
# → {"code":0,"msg":"ok","data":"hi"}
```

### Docker Compose (E2E)

```bash
./deploy.sh test e2e
# Runs: build → compose up → pytest E2E (25+ cases) → compose down
```

Covers: HTTP/HTTPS/WS endpoints, Raft election, plugin load/unload,
cross-service routing, stress QPS/P99 targets.

### Kubernetes

```bash
kubectl apply -f k8s/
kubectl -n thunder rollout status deployment --timeout=120s

# NodePorts: HTTP=30006, Interface=30008, HTTPS=30043, WS=30010, Admin=30090
```

---

## Writing a Plugin

```cpp
// code/HelloHttp/src/ModuleHello/ModuleOrder.cpp
#include "Module.hpp"

class ModuleOrder : public net::Module {
public:
    bool DoMsg(net::MsgHead& head, net::MsgBody& body) override {
        util::CJsonObject req(body.data());
        std::string action;
        req.Get("action", action);

        util::CJsonObject rsp;
        rsp.Add("code", 0);
        rsp.Add("orderId", generateId());
        body.set_data(rsp.ToString());
        return true;
    }
};

MUDULE_CREATE(ModuleOrder);
```

Upload and activate:

```bash
./deploy.sh build-so HelloHttp_ModuleOrder
curl -X POST http://localhost:8090/api/so-extract -F "file=ModuleOrder.so"
```

---

## Testing

```bash
./deploy.sh test unit          # C++ gtest (359 cases) + Python pytest
./deploy.sh test e2e           # Docker E2E (25+ integration cases)
./deploy.sh clean              # remove build artifacts + Docker state
```

---

## Repository Layout

```
code/
├── Net/          # Core: Manager, Worker, I/O backends, codec, coroutines
├── Center/       # Raft consensus + service registry
├── Logic/        # Example logic node
├── HelloHttp/    # HTTP gateway node + example plugins + Lua modules
├── HelloHttps/   # HTTPS gateway node
├── HelloWs/      # WebSocket gateway node
├── Interface/    # Interface node (Protobuf gateway)
├── Proto/        # .proto definitions
└── Util/         # JSON (yyjson), logging, DB helpers

deploy/           # Built binaries, node configs, start scripts
docs/
├── architecture/ # Design docs, benchmarks, feature deep-dives
├── reports/      # Benchmark reports, incident postmortems
└── io/           # I/O backend comparison and tuning guides
k8s/              # Kubernetes manifests
tests/            # pytest E2E, smoke scripts, benchmark scripts
```

---

## License

See [`LICENSE`](LICENSE).
