#!/usr/bin/env bash
# 启动 Hello 节点（deploy/Hello/bin/Hello，工作目录 deploy/Hello）
# 若已有 Hello_robot 进程则先结束；启动后用 wrk 做一次简短冒烟（默认 1 线程 1 连接 1 秒）。
#
# 用法: ./start_helloserver.sh
#       CONF=conf/Hello.json ./start_helloserver.sh
#
# 可选环境变量（与 test_helloserver_wrk.sh 一致）：
#   HELLO_HOST HELLO_PORT HELLO_PATH WRK_SCRIPT

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")" && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Hello.json}"

HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27006}"
HELLO_PATH="${HELLO_PATH:-/hello/hello}"
WRK_SCRIPT="${WRK_SCRIPT:-${DEPLOY_ROOT}/wrk_helloserver.lua}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

echo "=== 若已有 Hello_robot 则先停止 ==="
pkill Hello_robot 2>/dev/null || true
sleep 1

cd "${DEPLOY_ROOT}/Hello"
mkdir -p log

echo "=== 后台启动 Hello (${CONF}) ==="
nohup "${DEPLOY_ROOT}/Hello/bin/Hello" "${CONF}" >> log/start_helloserver.log 2>&1 &
echo "PID=$! 日志: ${DEPLOY_ROOT}/Hello/log/start_helloserver.log"

BASE_URL="http://${HELLO_HOST}:${HELLO_PORT}${HELLO_PATH}"
echo "=== HTTP 探测（单条 POST）: ${BASE_URL} ==="
# 单条请求；响应体打印到 stdout，末尾附 HTTP 状态码（连接失败等非 0 退出）
if ! curl -sS -X POST "${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"option":"Echo"}' \
    -w '\n[HTTP %{http_code}]\n'; then
  echo "错误: curl 请求失败（服务未就绪或网络错误，见 ${DEPLOY_ROOT}/Hello/log/start_helloserver.log）" >&2
  exit 1
fi

if ! command -v wrk >/dev/null 2>&1; then
  echo "提示: 未安装 wrk，跳过冒烟。可安装: sudo apt install wrk" >&2
  exit 0
fi

if [[ ! -f "${WRK_SCRIPT}" ]]; then
  echo "提示: 未找到 ${WRK_SCRIPT}，跳过 wrk 冒烟" >&2
  exit 0
fi

# 与 test_helloserver_wrk.sh 同一 Lua；就绪检测已用 curl POST 过一次，此处 wrk 为轻量 1 秒冒烟
echo "=== wrk 冒烟 (-t1 -c1 -d1s) ==="
wrk -t1 -c1 -d1s -s"${WRK_SCRIPT}" --latency "${BASE_URL}"

echo "=== Hello 已在后台运行，查看日志: tail -f ${DEPLOY_ROOT}/Hello/log/start_helloserver.log ==="
echo "=== 进程列表: ==="
ps -ef | grep Hello_robot
