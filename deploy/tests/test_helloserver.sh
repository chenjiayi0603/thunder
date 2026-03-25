#!/usr/bin/env bash
# 启动 Hello 节点（deploy/Hello/bin/Hello，工作目录 deploy/Hello）
# 若已有 Hello_robot 进程则先结束；启动后对 /hello/hello 做 JSON POST 用例 + 可选 wrk 冒烟；
# 脚本退出时（成功或失败）会再次 pkill Hello_robot，避免测试进程残留。
#
# 用法: ./test_helloserver.sh
#       CONF=conf/Hello.json ./test_helloserver.sh
#
# 可选环境变量（与 test_helloserver_wrk.sh 一致）：
#   HELLO_HOST HELLO_PORT HELLO_PATH WRK_SCRIPT
#   CURL_MAXTIME_HELLO     — 单条 curl 超时秒（默认 60；线程池阻塞用例约 sleep 80ms）
#   HELLO_TEST_EXTERNAL_CO20 — 置 1 时额外跑 TestHttpRequestCo（需出网访问百度等，超时 120s）
#   STARTUP_WAIT_SEC

set -euo pipefail

# 脚本在 deploy/tests/，仓库内 deploy 根为其父目录（与 test_interfaceserver.sh 一致）
DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Hello.json}"

HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27006}"
HELLO_PATH="${HELLO_PATH:-/hello/hello}"
WRK_SCRIPT="${WRK_SCRIPT:-${DEPLOY_ROOT}/tests/wrk_helloserver.lua}"
CURL_MAXTIME_HELLO="${CURL_MAXTIME_HELLO:-60}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# 探测 HTTP 时勿让系统 curl 加载 3party/libcurl（否则 no version information available）
# --noproxy '*'：避免 http_proxy 导致访问本机失败
_curl_smoke() {
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' "$@"
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' "$@"
  fi
}

# POST JSON 到 BASE_URL；打印响应体；校验 HTTP 200 且响应体包含所有 grep 片段（固定字符串）
_hello_post_check() {
  local name="$1"
  local payload="$2"
  shift 2
  local tmp code
  tmp="$(mktemp)"
  echo "=== ${name}: POST ${BASE_URL} ==="
  code="$(
    _curl_smoke -sS -o "${tmp}" -w "%{http_code}" --max-time "${CURL_MAXTIME_HELLO}" \
      -X POST "${BASE_URL}" \
      -H 'Content-Type: application/json' \
      -d "${payload}" || true
  )"
  cat "${tmp}"
  echo ""
  if [[ "${code}" != "200" ]]; then
    echo "错误: ${name} HTTP ${code}（期望 200），见 ${DEPLOY_ROOT}/Hello/log/test_helloserver.log" >&2
    rm -f "${tmp}"
    return 1
  fi
  local needle
  for needle in "$@"; do
    if ! grep -qF "${needle}" "${tmp}"; then
      echo "错误: ${name} 响应未包含: ${needle}" >&2
      rm -f "${tmp}"
      return 1
    fi
  done
  rm -f "${tmp}"
  echo "--- ${name}: OK ---"
}

_stop_hello_robot() {
  echo "=== 测试结束，停止 Hello_robot ===" >&2
  pkill Hello_robot 2>/dev/null || true
  sleep 1 || true
}
trap '_stop_hello_robot' EXIT INT TERM

echo "=== 若已有 Hello_robot 则先停止 ==="
pkill Hello_robot 2>/dev/null || true
sleep 1

cd "${DEPLOY_ROOT}/Hello"
mkdir -p log

echo "=== 后台启动 Hello (${CONF}) ==="
nohup "${DEPLOY_ROOT}/Hello/bin/Hello" "${CONF}" >> log/test_helloserver.log 2>&1 &
echo "PID=$! 日志: ${DEPLOY_ROOT}/Hello/log/test_helloserver.log"

sleep "${STARTUP_WAIT_SEC:-2}"

BASE_URL="http://${HELLO_HOST}:${HELLO_PORT}${HELLO_PATH}"

# --- ModuleHello::TestMsg 用例（无需外网）---
_hello_post_check 'Echo' '{"option":"Echo"}' '"code"' '"msg"'
# 256KiB × 每项为 3 → checksum = 786432（与 ModuleHello HelloPoolCpuCo 一致）
_hello_post_check 'TestHelloPoolCpu' '{"option":"TestHelloPoolCpu"}' 'TestHelloPoolCpu' '786432'
# slept_ms 与 HelloPoolBlockCo 中 delay_ms=80 一致（避免单独匹配 "80" 误伤其它数字）
_hello_post_check 'TestHelloPoolBlock' '{"option":"TestHelloPoolBlock"}' 'TestHelloPoolBlock' '"slept_ms":80'
# 未知 option 走默认分支，仍应 200 + code
_hello_post_check 'UnknownOption' '{"option":"NoSuchOption"}' '"code"'

if [[ "${HELLO_TEST_EXTERNAL_CO20:-0}" == "1" ]]; then
  echo "=== HELLO_TEST_EXTERNAL_CO20=1：外网协程用例（可能较慢/失败）==="
  CURL_MAXTIME_HELLO="${HELLO_TEST_EXTERNAL_CO20_MAXTIME:-120}" \
    _hello_post_check 'TestHttpRequestCo' '{"option":"TestHttpRequestCo"}' '"code"'
else
  echo "=== 跳过外网协程用例（TestHttpRequestCo）；需出网时: HELLO_TEST_EXTERNAL_CO20=1 $0 ==="
fi

if ! command -v wrk >/dev/null 2>&1; then
  echo "提示: 未安装 wrk，跳过 wrk 冒烟。可安装: sudo apt install wrk" >&2
  echo "提示: 日志见 ${DEPLOY_ROOT}/Hello/log/test_helloserver.log" >&2
  exit 0
fi

if [[ ! -f "${WRK_SCRIPT}" ]]; then
  echo "提示: 未找到 ${WRK_SCRIPT}，跳过 wrk 冒烟" >&2
  echo "提示: 日志见 ${DEPLOY_ROOT}/Hello/log/test_helloserver.log" >&2
  exit 0
fi

# 与 test_helloserver_wrk.sh 同一 Lua；上面 curl 已覆盖业务路径，此处 wrk 为轻量 1 秒冒烟
echo "=== wrk 冒烟 (-t1 -c1 -d1s) ==="
wrk -t1 -c1 -d1s -s"${WRK_SCRIPT}" --latency "${BASE_URL}"

echo "=== 用例与 wrk 完成；日志: ${DEPLOY_ROOT}/Hello/log/test_helloserver.log ==="
