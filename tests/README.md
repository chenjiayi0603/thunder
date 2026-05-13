# Thunder 测试

## 目录结构

```
tests/
├── run_all.sh              # 一键全部测试入口
├── build_and_test.sh       # 构建 + 测试
├── unit/                   # 单元测试 (64 cases, 零外部依赖, ~14s)
│   ├── test_websocket_key.py   # RFC 6455 / WebSocket 握手
│   ├── test_token_verify.py    # GenKey/VerifyKey permutation
│   ├── test_json_parse.py      # JSON 解析边界
│   ├── test_conhash.py         # 一致性哈希
│   └── test_iobackend_behavior.py  # IoBackend 契约回归
├── e2e/                    # 端到端集成测试 (25 cases, 需 Docker)
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

## 快速上手

```bash
# 一键全部测试 (单元 + E2E)
./tests/run_all.sh

# 仅单元测试
./tests/run_all.sh unit

# 仅 E2E
./tests/run_all.sh e2e
```
