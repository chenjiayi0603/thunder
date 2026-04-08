#!/usr/bin/env bash
# Thunder 集成测试总入口（参考 Redis runtest 体验：单入口 + 前置探测 + 分组执行）
#
# 用法（仓库任意路径）:
#   ./deploy/docker/test_all_integration.sh
#   ./deploy/docker/test_all_integration.sh --local
#   ./deploy/docker/test_all_integration.sh --external --group integration
#   ./deploy/docker/test_all_integration.sh --group docker-smoke --keep-docker
#
# 模式:
#   --local     自动拉起 docker compose（默认）
#   --external  仅连接已有环境，不做 docker compose up/down
#
# 分组:
#   --group all|unit|integration|docker-smoke   默认 all
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKER_DIR="${REPO_ROOT}/deploy/docker"
TEST_BUILD_DIR="${REPO_ROOT}/build/code/test"
MODE="local"
GROUP="all"
KEEP_DOCKER=0
POST_UP_WAIT_SEC="${POST_UP_WAIT_SEC:-15}"

failures=0
runs=0

usage() {
  cat <<'EOF'
Usage:
  ./deploy/docker/test_all_integration.sh [options]

Options:
  --local                 Auto start/stop docker compose (default)
  --external              Use existing environment, no compose up/down
  --group <name>          all|unit|integration|docker-smoke (default: all)
  --keep-docker           In local mode, skip compose down on exit
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --local) MODE="local" ;;
    --external) MODE="external" ;;
    --group)
      GROUP="${2:-}"
      shift
      ;;
    --keep-docker) KEEP_DOCKER=1 ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "错误: 未知参数: $1" >&2
      usage
      exit 2
      ;;
  esac
  shift
done

if [[ "${GROUP}" != "all" && "${GROUP}" != "unit" && "${GROUP}" != "integration" && "${GROUP}" != "docker-smoke" ]]; then
  echo "错误: --group 仅支持 all|unit|integration|docker-smoke" >&2
  exit 2
fi

_title() {
  echo ""
  echo "###############################################################################"
  echo "# $1"
  echo "###############################################################################"
}

_run() {
  local title="$1"
  shift
  runs=$((runs + 1))
  _title "${title}"
  if "$@"; then
    echo "# OK: ${title}"
  else
    echo "# FAIL: ${title}" >&2
    failures=$((failures + 1))
  fi
}

_require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "错误: 缺少命令 ${cmd}" >&2
    return 1
  fi
}

_check_port() {
  local host="$1"
  local port="$2"
  if command -v ss >/dev/null 2>&1 && ss -tln 2>/dev/null | grep -q ":${port} "; then
    return 0
  fi
  if command -v nc >/dev/null 2>&1 && nc -z "${host}" "${port}" 2>/dev/null; then
    return 0
  fi
  return 1
}

_prepare_mysql_schema() {
  local sql
  sql="CREATE TABLE IF NOT EXISTS thunder_orm_ut_users (
id INT AUTO_INCREMENT PRIMARY KEY,
user_id VARCHAR(64),
user_name VARCHAR(128),
password VARCHAR(128),
org_name VARCHAR(128)
);"

  if [[ "${MODE}" == "local" ]]; then
    (cd "${DOCKER_DIR}" && docker compose exec -T mysql mariadb -uroot -pthunder thunder_test -e "${sql}") >/dev/null
  else
    if command -v mariadb >/dev/null 2>&1; then
      mariadb -h "${THUNDER_ORM_MYSQL_HOST:-127.0.0.1}" -P "${THUNDER_ORM_MYSQL_PORT:-3306}" \
        -u "${THUNDER_ORM_MYSQL_USER:-root}" -p"${THUNDER_ORM_MYSQL_PASSWORD:-thunder}" \
        "${THUNDER_ORM_MYSQL_DB:-thunder_test}" -e "${sql}" >/dev/null
    else
      echo "警告: external 模式未找到 mariadb 客户端，跳过自动建表。"
    fi
  fi
}

_cleanup() {
  if [[ "${MODE}" != "local" ]]; then
    return 0
  fi
  if [[ "${KEEP_DOCKER}" == "1" ]]; then
    echo "==> KEEP_DOCKER=1，保留 Docker 栈"
    return 0
  fi
  echo "==> 清理 docker compose"
  (cd "${DOCKER_DIR}" && bash "${DOCKER_DIR}/dev_up_logs.sh" down) || true
}
trap _cleanup EXIT

_title "前置检查"
_require_cmd bash
_require_cmd python3
_require_cmd ctest
if [[ "${MODE}" == "local" ]]; then
  _require_cmd docker
fi
if [[ ! -d "${TEST_BUILD_DIR}" ]]; then
  echo "错误: 未找到 ${TEST_BUILD_DIR}" >&2
  echo "请先执行: cmake --build build -j4" >&2
  exit 1
fi
echo "模式: ${MODE} | 分组: ${GROUP}"

if [[ "${MODE}" == "local" ]]; then
  _title "启动本地依赖（docker compose）"
  (cd "${DOCKER_DIR}" && bash "${DOCKER_DIR}/dev_up_logs.sh" restart)
  if [[ "${POST_UP_WAIT_SEC}" != "0" ]]; then
    echo "等待 ${POST_UP_WAIT_SEC}s 让服务就绪..."
    sleep "${POST_UP_WAIT_SEC}"
  fi
fi

# external 模式做能力探测并给出可执行提示。
if [[ "${MODE}" == "external" ]]; then
  _title "external 模式能力探测"
  for p in 27006 27010 27443 6379 3306; do
    if _check_port 127.0.0.1 "${p}"; then
      echo "OK: 127.0.0.1:${p} 已监听"
    else
      echo "WARN: 127.0.0.1:${p} 未监听（相关分组可能失败或跳过）"
    fi
  done
fi

run_unit=0
run_integration=0
run_smoke=0
case "${GROUP}" in
  all) run_unit=1; run_integration=1; run_smoke=1 ;;
  unit) run_unit=1 ;;
  integration) run_integration=1 ;;
  docker-smoke) run_smoke=1 ;;
esac

if [[ "${run_unit}" == "1" ]]; then
  _run "ctest unit-like (排除 ORM 集成用例)" \
    ctest --output-on-failure --test-dir "${TEST_BUILD_DIR}" \
      -E "ThunderOrmMysql.Integration_AsyncAndFuture|ThunderOrmRedis.Integration_SetGetFuture"
fi

if [[ "${run_integration}" == "1" ]]; then
  _run "ORM integration 前置建表" _prepare_mysql_schema
  _run "ctest ORM integration (mysql+redis)" \
    env THUNDER_ORM_INTEGRATION=1 \
      THUNDER_ORM_MYSQL_HOST="${THUNDER_ORM_MYSQL_HOST:-127.0.0.1}" \
      THUNDER_ORM_MYSQL_PORT="${THUNDER_ORM_MYSQL_PORT:-3306}" \
      THUNDER_ORM_MYSQL_USER="${THUNDER_ORM_MYSQL_USER:-root}" \
      THUNDER_ORM_MYSQL_PASSWORD="${THUNDER_ORM_MYSQL_PASSWORD:-thunder}" \
      THUNDER_ORM_MYSQL_DB="${THUNDER_ORM_MYSQL_DB:-thunder_test}" \
      THUNDER_ORM_REDIS_HOST="${THUNDER_ORM_REDIS_HOST:-127.0.0.1}" \
      THUNDER_ORM_REDIS_PORT="${THUNDER_ORM_REDIS_PORT:-6379}" \
      ctest --output-on-failure --test-dir "${TEST_BUILD_DIR}" \
      -R "ThunderOrmMysql.Integration_AsyncAndFuture|ThunderOrmRedis.Integration_SetGetFuture"

  _run "HTTPS Python integration" \
    env -u ALL_PROXY -u all_proxy -u HTTP_PROXY -u http_proxy -u HTTPS_PROXY -u https_proxy \
      python3 "${DOCKER_DIR}/test_helloserver_https_integration.py"
fi

if [[ "${run_smoke}" == "1" ]]; then
  _run "docker smoke total入口" bash "${DOCKER_DIR}/test_all_docker_smoke.sh"
fi

_title "汇总"
echo "总步骤: ${runs}"
echo "失败数: ${failures}"
if [[ "${failures}" -gt 0 ]]; then
  echo "结论: FAIL（请查看上面的 FAIL 分段）" >&2
  exit 1
fi
echo "结论: PASS"
