#!/usr/bin/env bash
# 顺序运行 deploy/tests 下主要联调/冒烟脚本（总入口）
#
# 包含：
#   test_helloserver.sh          — HTTP Hello + 轻量 wrk（wrk_helloserver.lua）
#   test_helloserver_wrk.sh      — 完整 wrk 压测（默认 10s，同上 Lua；本脚本内会临时拉起 Hello）
#   test_helloserver_ws.sh       — WebSocket Hello（HelloWs.json）
#   test_interface_http_co20.sh  — Interface Echo + 外网 HTTP 协程（需访问 example.com）
#   test_interfaceserver.sh      — Center → Logic → Interface GenKey/VerifyKey
#   test_multicenter_raft.sh      — 三 Center Raft + admin + 复用 test_interfaceserver
#
# wrk_helloserver.lua 由 test_helloserver*.sh 通过 -s 引用，不单独执行。
#
# 用法（建议在仓库根或任意目录）:
#   ./deploy/tests/test_all.sh
#   bash deploy/tests/test_all.sh
#
# 跳过部分步骤（默认全跑）：
#   SKIP_TEST_HELLOSERVER=1
#   SKIP_TEST_HELLOSERVER_WRK=1
#   SKIP_TEST_HELLOSERVER_WS=1
#   SKIP_TEST_INTERFACE_HTTP_CO20=1    # 无出网/离线 CI 建议跳过
#   SKIP_TEST_INTERFACESERVER=1
#   SKIP_TEST_MULTICENTER_RAFT=1
#
# 子脚本支持的环境变量仍生效（如 WRK_DURATION、HELLO_HOST、NUM_CENTER_INSTANCES 等）。

set -u

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_ROOT="$(cd "${TESTS_DIR}/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

failures=0

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

# test_helloserver_wrk 需要 Hello 已监听；test_helloserver.sh 退出会 stop Hello，故此处临时拉起同配置实例。
_wrk_start_hello_http() {
  local conf="${1:-conf/Hello.json}"
  if [[ ! -x "${DEPLOY_ROOT}/Hello/bin/Hello" ]]; then
    echo "错误: 缺少 ${DEPLOY_ROOT}/Hello/bin/Hello，跳过 wrk 前置启动" >&2
    return 1
  fi
  echo "=== 为 wrk 临时启动 Hello (${conf}) ===" >&2
  pkill Hello_robot 2>/dev/null || true
  sleep 1
  mkdir -p "${DEPLOY_ROOT}/Hello/log"
  (
    cd "${DEPLOY_ROOT}/Hello" || exit 1
    nohup "${DEPLOY_ROOT}/Hello/bin/Hello" "${conf}" >>log/test_all_wrk_prep.log 2>&1 &
  )
}

_wrk_stop_hello_http() {
  echo "=== wrk 结束，停止 Hello_robot ==="
  pkill Hello_robot 2>/dev/null || true
  sleep 1 || true
}

if [[ "${SKIP_TEST_HELLOSERVER:-0}" != "1" ]]; then
  _run "test_helloserver.sh" bash "${TESTS_DIR}/test_helloserver.sh"
else
  echo "# 跳过 SKIP_TEST_HELLOSERVER=1"
fi

if [[ "${SKIP_TEST_HELLOSERVER_WRK:-0}" != "1" ]]; then
  if _wrk_start_hello_http "conf/Hello.json"; then
    sleep "${STARTUP_WAIT_SEC:-2}"
    _run "test_helloserver_wrk.sh" bash "${TESTS_DIR}/test_helloserver_wrk.sh"
    _wrk_stop_hello_http
  else
    echo "# FAIL: 无法为 test_helloserver_wrk 启动 Hello" >&2
    failures=$((failures + 1))
  fi
else
  echo "# 跳过 SKIP_TEST_HELLOSERVER_WRK=1"
fi

if [[ "${SKIP_TEST_HELLOSERVER_WS:-0}" != "1" ]]; then
  _run "test_helloserver_ws.sh" bash "${TESTS_DIR}/test_helloserver_ws.sh"
else
  echo "# 跳过 SKIP_TEST_HELLOSERVER_WS=1"
fi

if [[ "${SKIP_TEST_INTERFACE_HTTP_CO20:-0}" != "1" ]]; then
  _run "test_interface_http_co20.sh" bash "${TESTS_DIR}/test_interface_http_co20.sh"
else
  echo "# 跳过 SKIP_TEST_INTERFACE_HTTP_CO20=1"
fi

if [[ "${SKIP_TEST_INTERFACESERVER:-0}" != "1" ]]; then
  (
    cd "${DEPLOY_ROOT}"
    _run "test_interfaceserver.sh" bash "${TESTS_DIR}/test_interfaceserver.sh"
  )
else
  echo "# 跳过 SKIP_TEST_INTERFACESERVER=1"
fi

if [[ "${SKIP_TEST_MULTICENTER_RAFT:-0}" != "1" ]]; then
  (
    cd "${DEPLOY_ROOT}"
    _run "test_multicenter_raft.sh" bash "${TESTS_DIR}/test_multicenter_raft.sh"
  )
else
  echo "# 跳过 SKIP_TEST_MULTICENTER_RAFT=1"
fi

echo ""
echo "###############################################################################"
if [[ "${failures}" -eq 0 ]]; then
  echo "# 全部步骤成功（${TESTS_DIR}）"
  echo "###############################################################################"
  exit 0
else
  echo "# 完成，失败 ${failures} 个步骤（见上方 FAIL 标记）" >&2
  echo "###############################################################################"
  exit 1
fi
