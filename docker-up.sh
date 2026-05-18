#!/usr/bin/env bash
# =============================================================================
# Thunder 一键 Docker Compose 部署
# =============================================================================
#
# 用法（在仓库根目录）:
#   ./docker-up.sh              # 完整流程: cmake构建 + docker compose up
#   ./docker-up.sh down         # 停止并清理容器
#   ./docker-up.sh status       # 查看容器状态
#   ./docker-up.sh restart      # 重启栈
#   ./docker-up.sh logs         # 查看日志
#
# 快捷方式（跳过 cmake 构建）:
#   SKIP_BUILD=1 ./docker-up.sh
#
# 持续跟随日志:
#   FOLLOW=1 ./docker-up.sh
#
# 指定服务:
#   SERVICES="center logic" ./docker-up.sh
#
# 完整选项见 docker/dev_up_logs.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "${SCRIPT_DIR}/docker/dev_up_logs.sh" "$@"
