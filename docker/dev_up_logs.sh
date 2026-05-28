#!/usr/bin/env bash
# =============================================================================
# Thunder Docker 开发环境 — 已迁移到 deploy.sh
# 此脚本保留向后兼容，内部转发到 ../deploy.sh up
# =============================================================================
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo "dev_up_logs.sh → deploy.sh up (已迁移)"
case "${1:-}" in
    down|stop)
        exec "${REPO_ROOT}/deploy.sh" down ;;
    status|ps)
        exec "${REPO_ROOT}/deploy.sh" status ;;
    restart)
        exec "${REPO_ROOT}/deploy.sh" restart ;;
    *)
        exec "${REPO_ROOT}/deploy.sh" up ;;
esac
