# Thunder ⚡

**A high-performance C++20 gateway framework — 235k RPS per core, 220μs P50 latency, zero-downtime hot reload.**

[![License](https://img.shields.io/badge/license-AGPL--3.0%20%2B%20Commercial-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-%2300599C?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-orange)](https://kernel.org)

---

## What is Thunder?

Thunder is a **single-threaded event loop + multi-process + C++20 coroutine** gateway framework. Turn it into an API gateway, game server frontend, or IoT broker with `.so` plugins or Lua scripts — Thunder handles networking, protocol parsing, service discovery, and hot reloading.

```
Your code (.so / Lua) runs inside Thunder Workers.
Thunder handles high-performance I/O, distributed routing, and zero-downtime updates.
```

Think of it as: **Nginx + OpenResty + Lua, reimagined as a unified C++20 framework.**

---

## 🚀 15-Second Quick Start

```bash
git clone --recurse-submodules https://github.com/chenjiayi0603/thunder.git
cd thunder
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHUNDER_IO_ASIO_URING=ON
cmake --build build -j$(nproc) && cmake --install build

# Docker Compose one-click
./deploy.sh test compose --quick

# Test
curl http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}'
# → {"code":0,"msg":"ok"}
```

---

## 🧬 Architecture

```
                              ┌─────────────────┐
                              │   etcd ×3       │  ← Service registry · Config · Canary
                              └────────┬────────┘
                                       │ Watch (millisecond push)
              ┌────────────────────────┼────────────────────────┐
              │                        │                        │
    ┌─────────▼─────────┐    ┌────────▼────────┐    ┌─────────▼─────────┐
    │   Manager          │    │   Manager       │    │   Manager         │
    │  (supervisor)      │    │  (supervisor)   │    │  (supervisor)     │
    └──┬─────────────┬──┘    └──┬────────────┬──┘    └──┬─────────────┬──┘
       │fork         │fork      │fork         │fork     │fork         │fork
  ┌────▼───┐   ┌────▼───┐  ┌───▼────┐  ┌───▼────┐  ┌──▼────┐  ┌───▼────┐
  │Worker 0│   │Worker 1│  │Worker 0│  │Worker 1│  │Worker0│  │Worker 1│
  │epoll/  │   │epoll/  │  │epoll/  │  │epoll/  │  │epoll/ │  │epoll/  │
  │io_uring│   │io_uring│  │io_uring│  │io_uring│  │iouring│  │io_uring│
  └───┬────┘   └───┬────┘  └───┬────┘  └───┬────┘  └──┬────┘  └───┬────┘
      │            │           │           │          │           │
      └────────────┼───────────┼───────────┼──────────┼───────────┘
                   │           │           │          │
          ┌────────▼───────────▼───────────▼──────────▼────────┐
          │              Client Requests (hostNetwork)         │
          │      HTTP · HTTPS · WebSocket · WSS · Protobuf    │
          └───────────────────────────────────────────────────┘
```

**One Request, Two Paths:**

```
Client → TCP → picohttpparser(SIMD) → prefix match → Plugin AnyMessage() → yyjson → TCP
         └─ Fast Path (~4.3μs CPU) ──┘             └─ Normal Path (Protobuf codec) ─┘
```

---

## 🎯 Design Decisions

| Decision | Why | Trade-off |
|:---|:---|:---|
| **Single-threaded event loop** | Zero locks, zero contention | One core per process (multi-process scales) |
| **Multi-process (1 Manager + N Workers)** | Multi-core + fault isolation (crash kills 1 Worker) | IPC between processes |
| **C++20 coroutines (`co_await`)** | Async code reads like sync, no callback hell | Learning curve |
| **io_uring batch submission** | N I/O ops → 1 syscall, TLS latency halved | Linux 5.1+ |
| **hostNetwork (not NodePort)** | Zero kube-proxy hops | Pod cannot migrate |
| **etcd (not K8s DNS)** | Hot config push + canary weights + instant rollback | Runs a 3-node etcd cluster |
| **dlopen SO hot reload** | No process restart, connections stay alive | SO must be ABI-compatible |
| **Work-Stealing thread pool** | Per-worker queue, idle stealing | 2.53x faster than single-queue |

---

## 📊 Performance

*i9-12900H, Linux 7.0, `wrk -t4 -c100 -d10s`, single core pinned*

### HTTP — Thunder vs Nginx

| Payload | Thunder asio_uring | Nginx 1w | Gain |
|----:|:---:|:---:|:---:|
| 64 B | **235k RPS** | 214k RPS | **+10%** |
| 1 KB | **232k RPS** | 191k RPS | **+21%** |
| 4 KB | **223k RPS** | 184k RPS | **+21%** |

| Payload | Thunder | Nginx |
|----:|:---:|:---:|
| 64 B | **220 μs** | 466 μs |
| 4 KB | **332 μs** | 543 μs |

### HTTPS (TLS + io_uring batching)

| Payload | Thunder uring | Nginx SSL |
|----:|:---:|:---:|
| 64 B | **402 μs** | 752 μs |
| 4 KB | **247 μs** | 824 μs |

### Work-Stealing Thread Pool

| Config | Old single-queue | Work-Stealing | Speedup |
|:---|:---:|:---:|:--:|
| 1P-4C (typical) | 1,373 ns/op | **543 ns/op** | **2.53x** |

> 📖 Full report: [`docs/performance/`](docs/performance/)

---

## 🚀 Features

| Category | Capabilities |
|:---|:---|
| **Protocols** | HTTP/1.1 · HTTPS (TLS 1.3) · WebSocket · WSS · Protobuf RPC · MQTT 3.1.1 |
| **I/O Backends** | `ev` (epoll) · `asio_uring` (batched) · `native_uring` |
| **HTTP Parser** | picohttpparser (SSE4.2 SIMD, **+49% RPS** vs http_parser) |
| **Coroutines** | `co_await` Redis · `co_await` MySQL · `co_await` cross-node RPC · `co_await` thread-pool offload |
| **Thread Pool** | Work-Stealing (Go LRQ-style) · per-worker queue · idle stealing · 3-level dispatch |
| **Scripting** | Per-worker LuaJIT VM · Lua hot reload (<1ms, no restart) |
| **Plugins** | `.so` dynamic loading · etcd-triggered graceful restart · ABI version check |
| **Service Discovery** | etcd registry · lease heartbeat · CAS slot allocation · Watch push |
| **Canary** | Weighted routing (`v1=70% v2=30%`) · etcd weight key · instant rollback |
| **Health Check** | `GET /health` → `{"status":"ok"}` · K8s httpGet probe |
| **Admin UI** | Web console → plugin management · node topology · etcd browser |
| **Deployment** | Docker Compose · Bare metal · Kubernetes (hostNetwork + StatefulSet) |
| **Observability** | `/metrics` endpoint (Prometheus) · leveled logging (TRACE→FATAL) |

---

## 🔌 Plugin — One Macro to Register

```cpp
// code/HelloHttp/src/ModuleHello/ModuleHello.cpp
#include "cmd/Module.hpp"

class ModuleHello : public net::Module {
    bool AnyMessage(const net::tagMsgShell& shell, const HttpMsg& msg) override {
        net::SendToClient(shell, msg, R"({"code":0,"msg":"hello"})");
        return true;
    }
};
MUDULE_CREATE(core::ModuleHello);  // ← exports ABI version + create()
```

```bash
cmake --build build && cmake --install build
# .so deployed to deploy/HelloHttp/plugins/
```

**Hot Reload**: edit → rebuild `.so` → `curl PUT` → etcd version bump → Worker graceful restart → new connections use new `.so`, old connections drain without loss.

---

## 📂 Repository

```
code/Net/          Core: Manager, Worker, I/O backends, codecs, coroutines
code/HelloHttp/    HTTP gateway + plugins + Lua modules
code/HelloMqttBroker/  MQTT 3.1.1 Broker
code/Interface/    Protobuf API gateway → Logic backend
code/Logic/        Business logic node
deploy/            Build artifacts, configs, Dockerfiles
docs/architecture/ Design docs (etcd, coroutines, io_uring, work-stealing…)
docs/performance/  Reproducible benchmarks (Thunder vs Nginx, I/O backends)
k8s/               Kubernetes manifests + ops manual
tests/             pytest E2E, chaos, smoke tests
```

---

## 📖 Docs

| Section | For |
|:---|:---|
| [`QUICKSTART.md`](QUICKSTART.md) | Build, deploy, canary, hot reload |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Dev setup, code style, PR workflow |
| [`docs/architecture/00-overview.md`](docs/architecture/00-overview.md) | Architecture overview — draw the full data flow |
| [`docs/architecture/01-architecture-design.md`](docs/architecture/01-architecture-design.md) | Process model · event loop · C++20 coroutines |
| [`docs/architecture/02-etcd-designed.md`](docs/architecture/02-etcd-designed.md) | etcd service discovery · NodeID · vs CoreDNS |
| [`docs/performance/10-vs-nginx-benchmark.md`](docs/performance/10-vs-nginx-benchmark.md) | Thunder vs Nginx full benchmark |
| [`docs/api.md`](docs/api.md) | HTTP API reference |

---

## 📜 License

| Use Case | License | Cost |
|:---|:---|:---:|
| Open source, learning, internal tools | [AGPL v3](LICENSE.AGPL) | Free |
| Closed-source commercial | [Commercial](LICENSE.COMMERCIAL) | Paid |
| SaaS / cloud service | [Commercial](LICENSE.COMMERCIAL) | Paid |
