#!/usr/bin/env bash
# =============================================================================
# Thunder 一键测试入口 — 单元测试 + 端到端测试
# =============================================================================
#
# 用法（在仓库根目录）:
#   ./run_tests.sh              # 全部: unit + e2e
#   ./run_tests.sh unit         # 仅单元测试 (零外部依赖, ~14s)
#   ./run_tests.sh e2e          # 仅端到端测试 (需 Docker)
#   ./run_tests.sh bench        # 仅性能基准 (需 wrk)
#   ./run_tests.sh fast         # 快速模式: 仅单元测试
#   ./run_tests.sh build+test   # 构建 + 安装 + 全部测试
#   ./run_tests.sh build        # 仅构建 + 安装
#   ./run_tests.sh clean        # 清理构建产物
#
# 环境变量:
#   MODE=external  ./run_tests.sh e2e    # e2e external 模式 (远程栈)
#   KEEP_DOCKER=1  ./run_tests.sh e2e    # 测试后保留容器
#
# 完整文档见 tests/run_all.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "${SCRIPT_DIR}/tests/run_all.sh" "$@"
