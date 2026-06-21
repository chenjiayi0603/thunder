# Thunder 测试状态快照

> 这个文件是**活文档**，每次测试后由 `tests/save_status.sh` 更新。
> 不保留历史，只保留**当前状态**。

---

## 最近一次更新

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-21 |
| 分支 | `feat/etcd-watch-registry` |
| 操作人 | Claude |
| 阶段 | #119 gRPC v1.81.1 + etcd-cpp-apiv3 from-source 构建验证 |

---

## 构建

| 项 | 状态 | 备注 |
|----|:---:|------|
| `./deploy.sh build` | ✅ 0 error | 分支 feat/etcd-watch-registry |
| protobuf 降级分支构建 | ✅ 0 error | protobuf 7.35 → 6.33.5，ProtoCodec 编解码验证通过 |

---

## C++ 单元测试（ctest）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 |
| 结果 | ✅ **356/356**（100%） | 0 failed |
| 命令 | `ctest --test-dir build/code/test -j$(nproc)` | |

---

## Python 单元测试（pytest unit）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 |
| 结果 | ✅ **141/141** | 0 skipped |
| 命令 | `python3 -m pytest tests/unit/ -q` | |

---

## E2E 集成测试（Docker）

| 项 | 状态 | 备注 |
|----|:---:|------|
| 上次运行 | 2026-06-21 | 分支 `feat/etcd-watch-registry` |
| 结果 | ✅ **38 passed, 2 skipped** | 和上次基准一致 |
| 命令 | `./deploy.sh test e2e` | |

---

## 分支专项状态

### `chore/protobuf-6.33-downgrade`（#107 A~E 全完成）

| 子项 | 状态 | 备注 |
|------|:---:|------|
| 构建 0 error | ✅ | EtcdGrpcConnector + Manager 工厂 |
| ctest 355/355 | ✅ | Phase E 后；旧 EtcdParse/EtcdHttpConn 21 tests 已移除 |
| Python unit | ✅ | |
| B1 注册+KeepAlive | ✅ | gRPC 专属线程，SlotTxn，ev_async 事件桥 |
| B2 路由发现 | ✅ | ls 初始快照 + Watch 流，NodeNotice 组装 |
| B3 配置下发 | ✅ | config Watch + PutConfig 命令队列 |
| Manager 工厂接入 | ✅ | `connector=etcd-grpc` → EtcdGrpcConnector |
| deploy config 切换 | ✅ | 全部节点配置已改为 `etcd-grpc`，3 端点 |
| Docker lib | ✅ | libetcd-cpp-api-core.so 在 code/3party/lib |
| Phase C：3 节点 Raft | ✅ | docker-compose 3 节点 + YAML anchor |
| Phase E：旧代码清理 | ✅ | EtcdCenterConnector/HttpConn/Watcher/Parse 全删 |
| E2E 全通过 | ✅ | 30 passed，2 skipped（单节点 etcd 查询限制） |

---

## 待测 / 阻塞

| 项 | 阻塞原因 | 关联 |
|----|---------|------|
| Phase D：Admin 脚本更新 | ✅ 完成 | #107 Phase D |
| TSan 验证 | ✅ 完成 | 37 races 全在 gRPC/abseil 内部，Thunder 源码 0 race；报告：`docs/reports/tsan-and-raft-failover-2026-06-18.md` |
| EtcdGrpcConnector 多端点 failover | 🟡 待做 | 目前只连第一个端点，gRPC channel 内置但未验证 |
| #119 Step 4：删除 vendored 文件 | 🟡 待做 | ep_grpc+ep_etcd from-source ✅，待干净机器跑 thirdparty_deploy 后删 code/3party/include/etcd/ 和 lib/libetcd-cpp-api-core.so |
| Smoke | ✅ 完成 | 18/18 全通（2026-06-21） |

---

## 快捷命令

```bash
# 构建
./deploy.sh build

# 单元测试（快，约 10s）
cd build && ctest -j4 --output-on-failure

# Python 单元测试（极快，约 0.1s）
python3 -m pytest tests/unit/ -q

# E2E（慢，约 3-5min）
./deploy.sh test e2e

# 保存测试结果到本文件
tests/save_status.sh
```
