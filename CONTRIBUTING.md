# Contributing to Thunder

## 环境搭建

```bash
git clone --recurse-submodules https://github.com/chenjiayi0603/thunder.git
cd thunder
docker compose -f docker-compose.dev.yml up -d
```

## 编码规范

- C++20, `.clang-format` 统一风格
- `MUDULE_CREATE(ClassName)` 宏注册插件（自动带 ABI 版本校验）
- 注释用中文

## 测试

```bash
# 单元测试
./deploy.sh test unit

# K8s 回归测试 (需要 K8s 集群)
./deploy.sh test k8s

# etcd 混沌测试
ETCD_URL=http://10.107.65.253:2379 bash tests/chaos_etcd_k8s.sh
```

## 提交规范

- 分支: `feat/xxx`, `fix/xxx`, `docs/xxx`
- Commit: `type(scope): message` (如 `fix(#164): io_backend 配置修复`)
- PR 合入 dev, CI 通过后方可合入

## 设计文档

见 [docs/architecture/](docs/architecture/)
