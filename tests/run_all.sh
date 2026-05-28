#!/usr/bin/env bash
# =============================================================================
# Thunder 测试入口 — 已迁移到 deploy.sh
# 此脚本保留向后兼容，内部转发到 ../deploy.sh test
# =============================================================================
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
echo "run_all.sh → deploy.sh test (已迁移)"
exec "${REPO_ROOT}/deploy.sh" test "$@"
