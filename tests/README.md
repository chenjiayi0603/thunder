# Thunder 测试

## 统一入口

所有测试/构建/部署操作统一走 `./deploy.sh`：

```bash
./deploy.sh test unit   # C++ + Python 单元测试 (~45s, 零外部依赖)
./deploy.sh test e2e    # Docker 集成测试 (~3min, 需 Docker)
./deploy.sh test        # 全部: unit + e2e
./deploy.sh test bench  # wrk 性能测试
./deploy.sh build       # cmake configure + build + install
```

旧命令 `./tests/run_all.sh` 仍可用（内部转发到 deploy.sh）。

## 目录结构

```
tests/
├── run_all.sh              # → ../deploy.sh test (向后兼容)
├── unit/                   # Python 单元测试 (64 cases, 零外部依赖, ~0.03s)
│   ├── test_websocket_key.py   # RFC 6455 / WebSocket 握手
│   ├── test_token_verify.py    # GenKey/VerifyKey permutation
│   ├── test_json_parse.py      # JSON 解析边界
│   ├── test_conhash.py         # 一致性哈希
│   └── test_iobackend_behavior.py  # IoBackend 契约回归
├── e2e/                    # Python E2E 集成测试 (25 cases, 需 Docker)
│   ├── test_interface_chain.py # Interface→Logic 全链路
│   ├── test_http_hello.py      # HTTP Echo/CPU/Block
│   ├── test_https_hello.py     # HTTPS (TLS)
│   ├── test_ws_hello.py        # WebSocket
│   ├── test_center_admin.py    # Center 管理
│   ├── test_multicenter_raft.py # Raft 选举
│   └── ...
└── benchmark/              # 性能基准测试
    ├── run_bench.sh        # 全量三档横向对比
    ├── run_quick_bench.sh  # 快速冒烟
    └── wrk_*.lua           # wrk 压测脚本
```
code/test/                  # C++ gtest 单元测试 (20+ targets, ~250 cases)
  ├── connector/           # TcpCenterConnector 插件测试
  ├── coroutine/           # C++20 协程
  ├── codec/               # 编解码器 (proto/http/client)
  ├── session/             # 会话管理
  └── ...
```
