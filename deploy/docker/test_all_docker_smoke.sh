#!/usr/bin/env bash
# Docker Compose 冒烟总入口：先 dev_up_logs restart（已运行则先 down 再 up），再顺序执行各 *smoke.sh，最后 compose down。
#
# 用法（仓库任意路径）:
#   ./deploy/docker/test_all_docker_smoke.sh
#
# 环境变量:
#   KEEP_DOCKER=1                   任意退出路径上均不执行 down（方便排障）
#   POST_UP_WAIT_SEC                dev_up 完成后到开始测 HTTP 前的等待秒数（默认 15）
#   SKIP_TEST_HELLOSERVER_SMOKE=1   跳过 test_helloserver_smoke.sh
#   SKIP_TEST_HELLOSERVER_WS_SMOKE=1 跳过 test_helloserver_ws_smoke.sh
#   SKIP_TEST_INTERFACESERVER_SMOKE=1 跳过 test_interfaceserver_smoke.sh
#   dev_up_logs.sh 常用变量子进程继承: SKIP_BUILD、SKIP_THIRD_PARTY、BUILD_JOBS、CMAKE_BUILD_TYPE 等
#   各子脚本原有变量仍生效（PRE_CURL_SEC、HELLO_TEST_REDIS_MYSQL、REQUIRE_PORTS 等）
#
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DOCKER_DIR="${SCRIPT_DIR}"
KEEP_DOCKER="${KEEP_DOCKER:-0}"
POST_UP_WAIT_SEC="${POST_UP_WAIT_SEC:-15}"

failures=0

_cleanup() {
  if [[ "${KEEP_DOCKER}" == "1" ]]; then
    echo ""
    echo "==> KEEP_DOCKER=1，跳过 compose down"
    return 0
  fi
  echo ""
  echo "==> 关闭 Docker Compose 栈（dev_up_logs.sh down）"
  bash "${DOCKER_DIR}/dev_up_logs.sh" down || true
}

trap _cleanup EXIT

_run() {
  local title="$1"
  shift
  echo ""
  echo "###############################################################################"
  echo "# ${title}"
  echo "###############################################################################"
  if "$@"; then
    echo "# OK: ${title}"
  else
    echo "# FAIL: ${title}" >&2
    failures=$((failures + 1))
  fi
}

echo "==> 启动栈: ${DOCKER_DIR}/dev_up_logs.sh restart"
if ! bash "${DOCKER_DIR}/dev_up_logs.sh" restart; then
  echo "错误: dev_up_logs.sh restart 失败" >&2
  exit 1
fi

if [[ "${POST_UP_WAIT_SEC}" != "0" ]]; then
  echo "==> POST_UP_WAIT_SEC=${POST_UP_WAIT_SEC}s，等待服务就绪后继续"
  sleep "${POST_UP_WAIT_SEC}"
fi

if [[ "${SKIP_TEST_HELLOSERVER_SMOKE:-0}" != "1" ]]; then
  _run "test_helloserver_smoke.sh" bash "${DOCKER_DIR}/test_helloserver_smoke.sh"
else
  echo "# 跳过 SKIP_TEST_HELLOSERVER_SMOKE=1"
fi

if [[ "${SKIP_TEST_HELLOSERVER_WS_SMOKE:-0}" != "1" ]]; then
  _run "test_helloserver_ws_smoke.sh" bash "${DOCKER_DIR}/test_helloserver_ws_smoke.sh"
else
  echo "# 跳过 SKIP_TEST_HELLOSERVER_WS_SMOKE=1"
fi

if [[ "${SKIP_TEST_INTERFACESERVER_SMOKE:-0}" != "1" ]]; then
  _run "test_interfaceserver_smoke.sh" bash "${DOCKER_DIR}/test_interfaceserver_smoke.sh"
else
  echo "# 跳过 SKIP_TEST_INTERFACESERVER_SMOKE=1"
fi

echo ""
echo "###############################################################################"
if [[ "${failures}" -eq 0 ]]; then
  echo "# 全部 Docker 冒烟成功（${DOCKER_DIR}）"
  echo "###############################################################################"
  exit 0
else
  echo "# 失败 ${failures} 个步骤（见上方 FAIL）" >&2
  echo "###############################################################################"
  exit 1
fi
