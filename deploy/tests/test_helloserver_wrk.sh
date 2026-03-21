#!/usr/bin/env bash
# 使用 wrk 压测 Helloserver 的 HTTP 接入（默认 ModuleHello：POST /hello/hello）
#
# 前置：已启动 Hello，例如：
#   cd deploy && ./start_helloserver.sh
#
# 环境变量（可选）：
#   HELLO_HOST       默认 127.0.0.1（与 deploy/Hello/conf/Hello.json 中 access_host 一致）
#   HELLO_PORT       默认 27006（access_port）
#   HELLO_PATH       默认 /hello/hello（Hello.json module 里 ModuleHello 的 url_path）
#   WRK_THREADS      wrk -t，默认 4
#   WRK_CONNECTIONS  wrk -c，默认 100
#   WRK_DURATION     wrk -d，默认 10s
#   WRK_SCRIPT       Lua 脚本路径，默认 deploy/tests/wrk_helloserver.lua
#
# 示例：
#   ./test_helloserver_wrk.sh
#   HELLO_HOST=192.168.1.10 HELLO_PORT=27006 WRK_CONNECTIONS=200 ./test_helloserver_wrk.sh

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27006}"
HELLO_PATH="${HELLO_PATH:-/hello/hello}"
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-100}"
WRK_DURATION="${WRK_DURATION:-10s}"
WRK_SCRIPT="${WRK_SCRIPT:-${DEPLOY_ROOT}/tests/wrk_helloserver.lua}"

if ! command -v wrk >/dev/null 2>&1; then
  echo "错误: 未找到 wrk。请先安装，例如: sudo apt install wrk" >&2
  exit 1
fi

if [[ ! -f "${WRK_SCRIPT}" ]]; then
  echo "错误: 找不到 wrk Lua 脚本: ${WRK_SCRIPT}" >&2
  exit 1
fi

BASE_URL="http://${HELLO_HOST}:${HELLO_PORT}${HELLO_PATH}"

echo "=== Helloserver wrk 压测 ==="
echo "URL:    ${BASE_URL}"
echo "wrk:    -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${WRK_DURATION}"
echo "script: ${WRK_SCRIPT}"
echo ""

exec wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" \
  -s"${WRK_SCRIPT}" \
  --latency \
  "${BASE_URL}"
