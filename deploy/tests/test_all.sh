#!/usr/bin/env bash
# pytest 总入口（替代原 deploy/tests/test_all.sh）
#
# 用法:
#   ./deploy/tests/test_all.sh                   # 默认本地模式，运行全部 integration 或 smoke 测试
#   MODE=external ./deploy/tests/test_all.sh     # 切换为 external 模式
#   KEEP_DOCKER=1 ./deploy/tests/test_all.sh     # 测试完成后保留 docker 容器
#   PYTEST_EXPR="integration and not perf" ./deploy/tests/test_all.sh    # 只运行 integration 且不包含 perf 的测试
#
# pytest 默认会捕获 stdout，会话 fixture 里长时间 cmake/docker 时终端会像「卡住」；
# 此处使用 -v -s 以便即时看到用例名与 conftest 里的 [pytest integration] 阶段日志。
#
set -euo pipefail

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
PYTEST_DIR="${TESTS_DIR}/pytest"
MODE="${MODE:-local}"
KEEP_DOCKER="${KEEP_DOCKER:-0}"
PYTEST_EXPR="${PYTEST_EXPR:-integration or smoke}"

command -v pytest >/dev/null 2>&1 || {
  echo "错误: 未找到 pytest，请先安装（例如: pip install pytest requests）" >&2
  exit 1
}

cd "${PYTEST_DIR}"

ARGS=( "-v" "-s" "-m" "${PYTEST_EXPR}" "--mode=${MODE}" )
if [[ "${KEEP_DOCKER}" == "1" ]]; then
  ARGS+=( "--keep-docker" )
fi

echo "=== pytest 集成测试入口 ==="
echo "mode=${MODE} keep_docker=${KEEP_DOCKER} expr=${PYTEST_EXPR}"
pytest "${ARGS[@]}"
