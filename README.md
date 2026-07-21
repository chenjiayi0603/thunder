# Thunder ⚡

**A high-performance C++20 gateway framework. Asynchronous by design, extensible at runtime.**

[![License](https://img.shields.io/badge/license-AGPL--3.0%20%2B%20Commercial-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-%2300599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-orange)](https://kernel.org)

```
HTTP / HTTPS / WebSocket → picohttpparser + io_uring → Protobuf RPC → Backend Logic

  235k RPS per core · 220 μs P50 latency · work-stealing 543 ns/op (旧队列 1374 ns)
```

---

## ⚡ Quick Start

```bash
# Build
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1
cmake --build build -j1 && cmake --install build

# Run (Docker Compose)
./deploy.sh up
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo","data":"hi"}'
# → {"code":0,"msg":"ok","data":"hi"}

# Test
./deploy.sh test unit       # C++ gtest + Python pytest (~45s)
./deploy.sh test e2e        # Full integration (~3 min)
```

> 📖 **New to Thunder?** Start with [`QUICKSTART.md`](QUICKSTART.md) — covers canary deployments, Lua & SO hot-reload, and K8s setup.

---

## 🎯 Why Thunder?

Thunder was built for scenarios where **latency matters** and **downtime is unacceptable** — API gateways, game backends, real-time proxies.

| Design Choice | Trade-Off |
|:---|:---|
| **Single-threaded event loop per worker** | No locks, no races. Multi-core via multi-process. |
| **C++20 coroutines (`co_await`)** | Async code reads like sync. 1M+ concurrent coroutines per thread. |
| **io_uring batch submission** | N I/O operations → 1 `io_uring_enter` syscall. Drops TLS latency from 803μs to 402μs. |
| **HostNetwork on K8s** | Zero kube-proxy hops. Client hits worker process directly. |
| **Graceful drain + dlopen hot-swap** | Update `.so` plugins without dropping a single connection. |
| **etcd-native service mesh** | Real-time config push, canary routing, versioned rollback — no ConfigMap restarts. |

> 🧠 **Deeper dive**: performance data → [`docs/performance/`](docs/performance/) · architecture → [`docs/architecture/01-architecture-design.md`](docs/architecture/01-architecture-design.md) · FAQ → [`docs/README.md#核心设计问答`](docs/README.md#核心设计问答)

---

## 🚀 Features

| Category | Capabilities |
|:---|:---|
| **Protocols** | HTTP/1.1 · HTTPS (TLS 1.3) · WebSocket · WSS · Internal Protobuf RPC |
| **I/O Backends** | `ev` (epoll) · `asio_uring` · `native_uring` · DPDK *(planned)* |
| **Parsing** | picohttpparser (SSE4.2 SIMD, +49% RPS vs http_parser) · yyjson arena allocator |
| **Coroutines** | `co_await` Redis · `co_await` MySQL · `co_await` cross-node RPC |
| **Thread Pool** | Work-stealing (Go LRQ style) · lock-free MPMC queue · dynamic resize |
| **Scripting** | LuaJIT VM per worker · Lua hot-reload (no process restart) |
| **Plugins** | `.so` dynamic load · etcd-triggered graceful swap · NFS-shared artifacts |
| **Service Mesh** | etcd registry · lease heartbeat · CAS slot allocation |
| **Canary** | Weighted routing (`v1=70% v2=30%`) · one-line rollback · per-node-type control |
| **Admin** | Web dashboard → plugin management · node topology · etcd browser |
| **Deploy** | Docker Compose · bare-metal · Kubernetes (hostNetwork + StatefulSet etcd) |

---

## 📊 Performance

*Single-core P-pinned, `wrk -t4 -c100 -d10s`, i9-12900H. Full report → [`docs/performance/`](docs/performance/)*

### HTTP — Thunder vs Nginx

| Payload | Thunder ev | Thunder asio_uring | Nginx 1w | Delta |
|----:|:---:|:---:|:---:|:---:|
| 1 KB | 229k RPS | **232k RPS** | 191k RPS | **+21%** |
| 4 KB | 216k RPS | **223k RPS** | 184k RPS | **+21%** |
| 64 B | 232k RPS | **235k RPS** | 214k RPS | **+10%** |

| Payload | Thunder asio_uring | Nginx |
|----:|:---:|:---:|
| 64 B | **220 μs** | 466 μs |
| 4 KB | **332 μs** | 543 μs |

### HTTPS — TLS with io_uring batching

| Payload | Thunder uring | Nginx SSL |
|----:|:---:|:---:|
| 64 B | **402 μs** | 752 μs |
| 4 KB | **247 μs** | 824 μs |

### WebSocket Echo (persistent)

| Payload | Conns | RPS | P50 | P99 |
|----:|:---:|:---:|:---:|:---:|
| 64 B | 10 | 46k | 192 μs | 549 μs |
| 1 KB | 10 | 15k | 564 μs | 1.7 ms |

---

## 🧬 Architecture

```
 Manager (fork/restart workers, watch etcd config changes)
   ├── Worker 0 ──── io_uring / epoll loop
   │    ├── HTTP Fast Path (picohttpparser + prefix match → zero-copy)
   │    ├── .so Modules (dynamic dlopen, hot-swap)
   │    ├── Lua VM (LuaJIT, per-worker, script hot-reload)
   │    └── Coroutines (co_await MySQL / Redis / cross-node RPC)
   ├── Worker N ──── ...
   └── Work-stealing thread pool (offload CPU tasks)

 etcd Cluster (3 nodes)
   ├── Service registry + lease heartbeat
   ├── Config store (push to Manager → graceful worker restart)
   └── Canary weights (weighted routing, instant rollback)
```

**Principles**: Share-nothing workers · zero-copy fast path · fault isolation (plugin crash → 1 worker restarts) · runtime-extensible (Lua + .so + etcd config)

---

## 🔌 Deploying a Plugin

Thunder supports **dual-mode deployment**: Push (exec+tar) + Pull (Manager HTTP GET from admin-web). Push is the legacy path, Pull is the default since #159.

```cpp
// code/HelloHttp/src/ModuleHello/ModuleHello.cpp
#include "cmd/Module.hpp"

class ModuleHello : public net::Module {
    bool AnyMessage(const net::tagMsgShell& shell, const HttpMsg& msg) override {
        net::SendToClient(shell, msg, R"({"code":0,"msg":"hello from plugin"})");
        return true;
    }
};
MUDULE_CREATE(core::ModuleHello);
```

```bash
# 1. Build all SO modules + deploy to deploy/{type}/plugins/
./deploy.sh build

# 2. Upload .so to admin-web artifact store (→ MinIO / PVC)
curl -X PUT --data-binary @ModuleHello.so \
  http://192.168.3.61:30090/api/plugins/HelloHttp/ModuleHello.so

# 3. Deploy — bumps etcd version + so_url → Manager Pull downloads + graceful restart
curl -X POST -H "Content-Type: application/json" \
  -d '{"filename":"ModuleHello.so"}' \
  http://192.168.3.61:30090/api/plugins/HelloHttp/deploy

# → no restart. no dropped connections. new connections use updated .so.
```

| Step | Push mode (legacy, #2) | Pull mode (default, #159) |
|------|------|------|
| Upload | → local PVC | → local PVC + MinIO |
| Deploy | exec+tar to each pod | etcd bump (version + so_url) |
| Pod receives | kubectl cp via K8s API | Manager Poll → HTTP GET admin-web → download |
| Pod restart | ❌ SO lost | ✅ Manager re-downloads on startup |

---

## 📂 Repository

```
code/Net/          Core: Manager, Worker, I/O backends, codec, coroutines
code/HelloHttp/    HTTP gateway + plugins + Lua modules
code/HelloHttps/   HTTPS gateway
code/HelloWs/      WebSocket gateway
code/Interface/    Protobuf API gateway → Logic backend
code/Logic/        Business logic node
deploy/            Built binaries, configs, Dockerfiles
docs/              Design docs, benchmarks, FAQ
k8s/               Kubernetes manifests + ops manual
tests/             pytest E2E, smoke, chaos tests
```

---

## 📖 Docs

| Section | For |
|:---|:---|
| [`QUICKSTART.md`](QUICKSTART.md) | Build, deploy, canary, hot-reload |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Dev setup, code style, PR process, commit conventions |
| [`docs/README.md`](docs/README.md) | Architecture FAQ, design index, reading paths |
| [`docs/architecture/`](docs/architecture/) | Subsystem deep-dives (etcd, coroutines, io_uring, work-stealing…) |
| [`docs/performance/`](docs/performance/) | Reproducible benchmarks (Thunder vs Nginx, I/O backends comparison) |
| [`k8s/k8s-manual.md`](k8s/k8s-manual.md) | K8s cluster setup, CNI, HPA, multi-datacenter |
| [`k8s/comparison-openim.md`](k8s/comparison-openim.md) | Deployment strategy comparison with OpenIM |

---

## 📜 License

| Use Case | License | Cost |
|:---|:---|:---:|
| Open-source, learning, internal tools | [AGPL v3](LICENSE.AGPL) | Free |
| Closed-source commercial products | [Commercial](LICENSE.COMMERCIAL) | Paid |
| SaaS / cloud services | [Commercial](LICENSE.COMMERCIAL) | Paid |

> Commercial inquiries → contact the author.

---

<p align="center">
  <sub>Built with C++20, libev, io_uring, and a relentless focus on latency.</sub>
</p>
