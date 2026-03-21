#!/usr/bin/env bash
# Interface ModuleInterface：JSON option TestHttpRequestCo20（与 code/Interface/.../ModuleInterface.cpp 中分支一致）
# → 启动 StepHttpRequestCo20 协程，外呼 http://example.com/（小页面），成功则 HTTP 响应体 JSON 中 code=0。
#
# 参考同目录 test_interfaceserver.sh 的环境变量与 curl 用法；本脚本默认只拉起 Interface，不依赖 Center/Logic。
#
# 用法（在 deploy 目录下）: ./tests/test_interface_http_co20.sh
#
# 可选环境变量：
#   CONF                    — Interface 配置，默认 conf/Interface.json
#   INTERFACE_HOST PORT PATH — 与 test_interfaceserver.sh 相同（默认 127.0.0.1:27008 /Interface/gentoken）
#   INTERFACE_BIN           — 可执行文件（默认 Interface/bin/Interface，否则 Hello/bin/Hello）
#   TEST_HTTP_CO20_MAXTIME  — curl 超时秒数（默认 120，外网慢时可调大）
#   SKIP_CLEAN_LOGS=1       — 启动前不删 Interface/log 下 *.log
#   STARTUP_WAIT_SEC        — Interface 启动后等待秒数（默认 2）
#
# 注意：需本机出网能访问 example.com；离线或防火墙拦截时本测试会失败。

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Interface.json}"

INTERFACE_HOST="${INTERFACE_HOST:-127.0.0.1}"
INTERFACE_PORT="${INTERFACE_PORT:-27008}"
INTERFACE_PATH="${INTERFACE_PATH:-/Interface/gentoken}"
TEST_HTTP_CO20_MAXTIME="${TEST_HTTP_CO20_MAXTIME:-120}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

_tcp_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -tln 2>/dev/null | grep -q ":${port} " && return 0
  fi
  if command -v nc >/dev/null 2>&1; then
    nc -z 127.0.0.1 "${port}" 2>/dev/null && return 0
  fi
  return 1
}

_curl_smoke() {
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' "$@"
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' "$@"
  fi
}

if [[ -n "${INTERFACE_BIN:-}" ]]; then
  BIN="${INTERFACE_BIN}"
elif [[ -x "${DEPLOY_ROOT}/Interface/bin/Interface" ]]; then
  BIN="${DEPLOY_ROOT}/Interface/bin/Interface"
elif [[ -x "${DEPLOY_ROOT}/Hello/bin/Hello" ]]; then
  BIN="${DEPLOY_ROOT}/Hello/bin/Hello"
  echo "提示: 未找到 Interface/bin/Interface，使用 Hello 二进制 + Interface 配置" >&2
else
  echo "错误: 未找到可执行文件。请先编译安装 Interface 或 Hello" >&2
  exit 1
fi
if [[ ! -x "${BIN}" ]]; then
  echo "错误: 不可执行: ${BIN}" >&2
  exit 1
fi

mkdir -p "${DEPLOY_ROOT}/Interface/plugins"
PLUGIN_SO="${DEPLOY_ROOT}/Interface/plugins/ModuleInterface.so"
if [[ ! -f "${PLUGIN_SO}" ]]; then
  BUILT_SO="${DEPLOY_ROOT}/../build/lib/ModuleInterface.so"
  if [[ -f "${BUILT_SO}" ]]; then
    echo "提示: 部署插件 ${PLUGIN_SO}（从 ${BUILT_SO} 复制）" >&2
    install -m0644 "${BUILT_SO}" "${PLUGIN_SO}"
  else
    echo "错误: 未找到 ${PLUGIN_SO}。请: cmake --build build --target InterfacePlugins && cmake --install build" >&2
    exit 1
  fi
fi

mkdir -p "${DEPLOY_ROOT}/Interface/log"
if [[ "${SKIP_CLEAN_LOGS:-0}" != "1" ]]; then
  echo "=== 清理 Interface/log 下 *.log ==="
  find "${DEPLOY_ROOT}/Interface/log" -maxdepth 1 -type f -name '*.log' -delete 2>/dev/null || true
else
  echo "提示: SKIP_CLEAN_LOGS=1，未清理 *.log"
fi

echo "=== [1/2] 启动 Interface — 若已有 Interface_robot 则先停止 ==="
pkill Interface_robot 2>/dev/null || true
sleep 1

cd "${DEPLOY_ROOT}/Interface"
mkdir -p log

echo "=== 后台启动 Interface (${CONF}) ==="
nohup "${BIN}" "${CONF}" >> log/test_interface_http_co20.log 2>&1 &
echo "PID=$! 日志: ${DEPLOY_ROOT}/Interface/log/test_interface_http_co20.log"

sleep "${STARTUP_WAIT_SEC:-2}"

if ! _tcp_listening "${INTERFACE_PORT}"; then
  echo "错误: Interface 未在 ${INTERFACE_PORT} 监听，见 log/test_interface_http_co20.log" >&2
  exit 1
fi

BASE_URL="http://${INTERFACE_HOST}:${INTERFACE_PORT}${INTERFACE_PATH}"
echo "=== [2/2] HTTP：POST {\"option\":\"TestHttpRequestCo20\"} — ${BASE_URL} ==="

RESP_BODY=""
if ! RESP_BODY=$(_curl_smoke -f -sS -m "${TEST_HTTP_CO20_MAXTIME}" -X POST "${BASE_URL}" \
  -H 'Content-Type: application/json' \
  -d '{"option":"TestHttpRequestCo20"}' \
  -w '\n[HTTP %{http_code}]\n'); then
  echo "错误: TestHttpRequestCo20 请求失败。(404) 多为未部署 ModuleInterface.so；(52) 常为异步回包前连接关闭" >&2
  echo "日志: ${DEPLOY_ROOT}/Interface/log/test_interface_http_co20.log" >&2
  exit 1
fi

JSON_LINE="$(echo "${RESP_BODY}" | head -n -1)"
echo "=== 响应体 ==="
echo "${JSON_LINE}"

_check_code_zero() {
  local json="$1"
  if command -v jq >/dev/null 2>&1; then
    jq -e '.code == 0' >/dev/null 2>&1 <<<"${json}"
    return $?
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import json,sys; d=json.load(sys.stdin); sys.exit(0 if d.get("code")==0 else 1)' <<<"${json}"
    return $?
  fi
  grep -q '"code"[[:space:]]*:[[:space:]]*0' <<<"${json}"
}

if ! _check_code_zero "${JSON_LINE}"; then
  echo "错误: 期望 JSON 中 code==0（外呼 example.com 成功）。当前响应见上；code=1 多为网络不可达或 HttpGetAsync 失败" >&2
  exit 1
fi

echo "=== TestHttpRequestCo20 通过（code=0）==="
echo "=== Interface 仍在后台运行；日志: tail -f ${DEPLOY_ROOT}/Interface/log/test_interface_http_co20.log ==="
