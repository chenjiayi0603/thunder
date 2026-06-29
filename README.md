# Thunder

Thunder is a high-performance C++20 gateway and distributed service framework.
It handles HTTP, HTTPS, and WebSocket traffic, routes requests to backend Logic nodes via Protobuf RPC,
and extends behavior at runtime through hot-reloadable Lua scripts and `.so` plugins —
all within a single-threaded event loop, **230,000+ requests/second per core**.

```
Client (HTTP / HTTPS / WS)
        │
        ▼
   Worker (event loop + .so plugins + Lua VM)
        │  io_uring / epoll
        ▼
   etcd Service Mesh
        │
   ┌────┴────┐
 LOGIC     LOGIC     (C++20 coroutines, horizontal scale)
```

---

## Building Thunder

**Requirements**: CMake ≥ 3.20, GCC 12+ or Clang 15+, OpenSSL headers, Docker + Compose

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1   # third-party libs, ~10–20 min first time
cmake --build build -j1
cmake --install build                                 # installs to deploy/
```

> `-j1` is required — parallel builds stall on disk I/O during third-party compilation.

Subsequent builds (third-party already built):

```bash
cmake --build build -j1 && cmake --install build
```

Optional flags:

```bash
-DTHUNDER_IO_ASIO_URING=ON    # enable asio io_uring backend (Linux 5.1+)
-DTHUNDER_LUAJIT=ON           # enable LuaJIT scripting
```

---

## Running Thunder

```bash
./deploy.sh up          # start Docker cluster (etcd + MySQL + Redis + all nodes)
./deploy.sh status      # container status + listening ports
./deploy.sh down        # stop and clean up
```

Wait ~15 seconds for all services to become healthy, then:

```bash
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo","data":"hi"}'
# → {"code":0,"msg":"ok","data":"hi"}
```

**Service ports:**

| Service     | Protocol  | Port  |
|-------------|-----------|-------|
| HelloHttp   | HTTP      | 27006 |
| HelloHttps  | HTTPS     | 27443 |
| HelloWs     | WebSocket | 27010 |
| Interface   | HTTP      | 27008 |
| Logic       | Internal  | 16068 |
| etcd        | HTTP      | 2379  |
| Redis       | TCP       | 6379  |
| MySQL       | TCP       | 3306  |

---

## Testing

```bash
./deploy.sh test unit      # C++ gtest (359 cases) + Python pytest, no external deps, ~45s
./deploy.sh test e2e       # Docker E2E: compose up → 25+ pytest cases → compose down, ~3 min
./deploy.sh test           # unit + e2e
./deploy.sh clean          # remove build artifacts + Docker state
```

Smoke test (requires cluster already running):

```bash
./tests/test_smoke.sh      # HTTP / HTTPS / WS / Interface→Logic / etcd, 9 checks
```

---

## Performance

Benchmarked on i9-12900H, 1 worker, `wrk -t4 -c100 -d10s`, INFO log, P-cores pinned.
Full report: [`docs/performance/10-vs-nginx-benchmark-20260610.md`](docs/performance/10-vs-nginx-benchmark-20260610.md)

### HTTP throughput vs Nginx 1.x

| Payload | Thunder ev | Thunder asio_uring | Nginx 1w  | Delta       |
|--------:|:----------:|:------------------:|:---------:|:-----------:|
| 64 B    | 232k RPS   | **235k RPS**       | 214k RPS  | **+9–10%**  |
| 1 KB    | 229k RPS   | 232k RPS           | 191k RPS  | **+20–21%** |
| 4 KB    | 216k RPS   | 223k RPS           | 184k RPS  | **+17–21%** |

### HTTP latency (asio_uring backend)

| Payload | Thunder ev | Thunder asio_uring | Nginx 1w | vs Nginx    |
|--------:|:----------:|:------------------:|:--------:|:-----------:|
| 64 B    | 424 µs     | **220 µs**         | 466 µs   | **2.1× lower** |
| 4 KB    | 457 µs     | **332 µs**         | 543 µs   | **1.6× lower** |

### HTTPS latency (TLS)

| Payload | Thunder ev | Thunder uring | Nginx SSL | vs Nginx       |
|--------:|:----------:|:-------------:|:---------:|:--------------:|
| 64 B    | 803 µs     | **402 µs**    | 752 µs    | **1.9× lower** |
| 4 KB    | 1.23 ms    | **247 µs**    | 824 µs    | **3.3× lower** |

> HTTPS throughput is SSL-CPU bound; Thunder uring wins on latency because `io_uring` batches all
> OpenSSL BIO calls into a single `io_uring_enter` instead of one syscall per BIO read/write.

### WebSocket echo (persistent connections)

| Payload | 10 conns | p50    | p99    |
|--------:|:--------:|:------:|:------:|
| 64 B    | 46,765   | 192 µs | 549 µs |
| 1 KB    | 15,888   | 564 µs | 1.7 ms |
| 4 KB    |  4,881   | 1.8 ms | 5.9 ms |

---

## Why Thunder Is Fast

### Zero-Copy Fast Path

Requests matching known route prefixes skip Protobuf + JSON decode entirely:

```
Normal path:  recv → parse → pb decode → handler → pb encode → send   (~162k RPS)
Fast path:    recv → prefix match → handler → memcpy template → send   (~236k RPS)
```

### picohttpparser — SIMD HTTP Parsing

Replaced `http_parser` with `picohttpparser`, a single-header SSE4.2-accelerated parser
that scans 16 bytes per cycle instead of one. **Measured gain: +49% RPS.**

### Pluggable I/O Backends

| Backend        | Strength                        | Notes                              |
|----------------|---------------------------------|------------------------------------|
| `ev` (epoll)   | Default, lowest overhead        | Best for small-medium payloads     |
| `asio_uring`   | Latency-sensitive, large payload| Batch submit: N I/Os → 1 syscall   |
| `native_uring` | Raw io_uring                    | Demo backend; no batch advantage   |
| `dpdk`         | Planned                         | Kernel bypass                      |

### Multi-Process Worker Model

Each Worker is an independent OS process. A crashing plugin takes down only that worker;
Manager restarts it transparently. Graceful restart drains in-flight connections before
killing the old worker.

### C++20 Coroutines

All async I/O — MySQL, Redis, cross-node RPC — written as `co_await`:

```cpp
net::AsyncTask HandleRequest(net::StepCo20& step) {
    auto rows = co_await db.Query("SELECT * FROM orders WHERE user_id=?", userId);
    bool cached = co_await cache.Set("orders:" + id, rows.toJson(), 300);
    co_await step.SendToInternalByNodeTypeAsync("LOGIC", head, body);
    step.Response(200, buildResponse(rows));
}
```

---

## Features

| Feature                    | Notes                                                     |
|----------------------------|-----------------------------------------------------------|
| HTTP/1.1                   | picohttpparser, keep-alive, chunked transfer              |
| HTTPS / TLS                | OpenSSL, SNI                                              |
| WebSocket (WS / WSS)       | Upgrade, ping/pong, fragmentation, TLS variant            |
| Internal Protobuf RPC      | Node-to-node binary transport                             |
| MySQL client               | `co_await db.Query(...)` — non-blocking, event loop safe  |
| Redis client               | `co_await cache.Get/Set/HSet(...)` — async hiredis        |
| Lua scripting              | LuaJIT, hot-reload, per-worker VM                         |
| `.so` plugin hot-swap      | Zero-downtime deploy via etcd watch + graceful restart    |
| etcd service mesh          | Registration, discovery, config push, TTL health          |
| Raft consensus (built-in)  | 3-node Center cluster, no external Zookeeper              |
| Admin web UI               | Plugin management, node topology, etcd browser            |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Gateway Nodes                           │
│                                                                 │
│  Manager                                                        │
│    ├── fork / restart Workers                                   │
│    ├── receive plugin updates from etcd                         │
│    └── graceful drain + hot-swap                                │
│                                                                 │
│  Worker 0..N  (one event loop each)                             │
│    ├── I/O Backend: ev / asio_uring / native_uring              │
│    ├── HTTP Fast Path (picohttpparser + prefix match)           │
│    ├── Codec chain: HTTP → Protobuf → response                  │
│    ├── .so Modules (dynamically loaded)                         │
│    ├── Lua VM (LuaJIT, per worker)                              │
│    └── C++20 Coroutine Steps                                    │
│         ├── co_await MySQL / Redis                              │
│         ├── co_await HTTP upstream                              │
│         └── co_await cross-node PB RPC                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │ Internal Protobuf (TCP)
┌───────────────────────────▼─────────────────────────────────────┐
│                        Logic Nodes                              │
│  Same Worker architecture; scaled horizontally via etcd.        │
└───────────────────────────┬─────────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                       Center Cluster                            │
│  3-node Raft — leader election, service registry, config store  │
└─────────────────────────────────────────────────────────────────┘
```

| Principle              | Implementation                                           |
|------------------------|----------------------------------------------------------|
| Share nothing          | One process per worker, no inter-worker locks            |
| No blocking            | `co_await` for all I/O; thread pool for CPU-bound tasks  |
| Zero-copy where possible | Fast Path + yyjson arena allocator                     |
| Fault isolation        | Plugin crash kills one worker; Manager restarts it       |
| Runtime extensibility  | Lua for logic; `.so` for hot paths; etcd for config      |

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

Build and deploy:

```bash
./deploy.sh build-so HelloHttp_ModuleOrder

# Extract to workers via Admin API (triggers graceful hot-swap)
curl -X POST http://localhost:8090/api/so-extract -F "file=ModuleOrder.so"
```

---

## Kubernetes

```bash
kubectl apply -f k8s/
kubectl -n thunder rollout status deployment --timeout=120s

# NodePorts: HTTP=30006  Interface=30008  HTTPS=30043  WS=30010  Admin=30090
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
├── Interface/    # Protobuf gateway node
├── Proto/        # .proto definitions
└── Util/         # JSON (yyjson), logging, DB helpers

deploy/           # Built binaries, node configs, start scripts
docs/
├── architecture/ # Design docs and deep-dives
├── performance/  # Benchmark reports
├── quality/      # ASan/TSan, sanitizer playbooks
└── reports/      # Benchmark reports, incident postmortems
k8s/              # Kubernetes manifests
tests/            # pytest E2E, smoke scripts, benchmark scripts
```

---

## License

See [`LICENSE`](LICENSE).
