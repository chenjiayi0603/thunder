# pytest 集成测试

## 运行方式

- 默认 local（自动拉起 docker 栈）：
  - `pytest tests/e2e -m "integration or smoke" --mode=local`
- external（连接现有环境）：
  - `pytest tests/e2e --mode=external`
- 性能测试（wrk）：
  - `pytest tests/e2e -m perf --mode=local`

也可直接用总入口：

- `./tests/run_all.sh`
- `MODE=external ./tests/run_all.sh e2e`

## 稳定性策略

- `conftest.py` 中统一管理 local/external 模式与 docker 生命周期。
- session 级端口探测（`27006/27010/27443`，local 下额外 `6379/3306`）。
- 统一去代理环境变量，避免本机代理导致本地回环请求异常。
- 失败输出保留子命令 stdout/stderr 便于排障。

