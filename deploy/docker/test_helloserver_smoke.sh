#!/usr/bin/env bash
# Hello HTTP 冒烟（Docker Compose 场景）
#
# 假定已在仓库根已执行:
#   docker compose up -d
# 且 docker-compose 使用 network_mode: host（与 conf/Hello.json 中 127.0.0.1 一致）。
#
# 本脚本只在宿主机发 curl，不要求 Hello 可执行文件在宿主机。
# 但在卷挂载路径下（- ../../:/thunder），需要存在：
#   deploy/Hello/plugins/ModuleHello.so
# 否则容器内可能路由失败或空响应。
#
# 用法（在 deploy/docker 下）:
#   ./test_helloserver_smoke.sh
#
# 可选环境变量:
#   HELLO_HOST HELLO_PORT HELLO_PATH — 默认 127.0.0.1:27006 /hello/hello
#   CURL_MAXTIME_HELLO              — 单次 curl 超时秒数（默认 60）
#   PRE_CURL_SEC                   — 发请求前额外 sleep 秒数（默认 0）
#   REQUIRE_PORTS                  — 为 1 时先检查 Hello 端口已 LISTEN（默认 0）
#   SKIP_PLUGIN_CHECK             — 为 1 时跳过 ModuleHello.so 存在性检查（默认 0）
#   HELLO_TEST_REDIS_MYSQL       — 为 1 时额外测 TestHelloCoRedis / TestHelloCoMysql（默认 0）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HELLO_HOST="${HELLO_HOST:-127.0.0.1}"
HELLO_PORT="${HELLO_PORT:-27006}"
HELLO_PATH="${HELLO_PATH:-/hello/hello}"

CURL_MAXTIME_HELLO="${CURL_MAXTIME_HELLO:-60}"
PRE_CURL_SEC="${PRE_CURL_SEC:-0}"
REQUIRE_PORTS="${REQUIRE_PORTS:-0}"
SKIP_PLUGIN_CHECK="${SKIP_PLUGIN_CHECK:-0}"
HELLO_TEST_REDIS_MYSQL="${HELLO_TEST_REDIS_MYSQL:-0}"

PLUGIN_SO="${DEPLOY_ROOT}/Hello/plugins/ModuleHello.so"

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

# 避免 LD_LIBRARY_PATH 中的 libcurl 干扰系统 curl（与 deploy/tests 里一致）
_curl_smoke() {
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' "$@"
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' "$@"
  fi
}

_hello_post_check() {
  local name="$1"
  local payload="$2"
  shift 2

  local tmp code
  tmp="$(mktemp)"

  echo "=== ${name}: POST http://${HELLO_HOST}:${HELLO_PORT}${HELLO_PATH} ==="
  code="$(
    _curl_smoke -sS -o "${tmp}" -w "%{http_code}" --max-time "${CURL_MAXTIME_HELLO}" \
      -X POST "http://${HELLO_HOST}:${HELLO_PORT}${HELLO_PATH}" \
      -H 'Content-Type: application/json' \
      -d "${payload}" || true
  )"

  cat "${tmp}"
  echo ""

  if [[ "${code}" != "200" ]]; then
    echo "错误: ${name} HTTP ${code}（期望 200）" >&2
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

if [[ "${SKIP_PLUGIN_CHECK}" != "1" && ! -f "${PLUGIN_SO}" ]]; then
  echo "错误: 缺少 ${PLUGIN_SO}（Hello 依赖此插件）。" >&2
  echo "请编译并安装: 在仓库根 cmake --build <build> --target ModuleHello && cmake --install <build>" >&2
  echo "或: SKIP_PLUGIN_CHECK=1 ./test_helloserver_smoke.sh（确认容器内另有挂载/构建产物路径一致）" >&2
  exit 1
fi

if [[ "${REQUIRE_PORTS}" == "1" ]]; then
  if ! _tcp_listening "${HELLO_PORT}"; then
    echo "错误: ${HELLO_HOST}:${HELLO_PORT} 未监听（请先 docker compose up -d 或检查 network_mode: host）" >&2
    exit 1
  fi
  echo "已检测到端口监听: ${HELLO_PORT}"
fi

if [[ "${PRE_CURL_SEC}" != "0" ]]; then
  echo "=== PRE_CURL_SEC=${PRE_CURL_SEC}s，等待后再发 HTTP ==="
  sleep "${PRE_CURL_SEC}"
fi

_hello_post_check 'Echo' '{"option":"Echo"}' '"code"' '"msg"'
_hello_post_check 'TestHelloPoolCpu' '{"option":"TestHelloPoolCpu"}' 'TestHelloPoolCpu' '786432'
_hello_post_check 'TestHelloPoolBlock' '{"option":"TestHelloPoolBlock"}' 'TestHelloPoolBlock' '"slept_ms":80'

if [[ "${HELLO_TEST_REDIS_MYSQL}" == "1" ]]; then
  echo "=== HELLO_TEST_REDIS_MYSQL=1：额外测试 Redis/MySQL 协程用例 ==="
  _hello_post_check 'TestHelloCoRedis' '{"option":"TestHelloCoRedis"}' '"option":"TestHelloCoRedis"' '"get_ok":1'
  _hello_post_check 'TestHelloCoMysql' '{"option":"TestHelloCoMysql"}' '"option":"TestHelloCoMysql"' '"create_ok":1' '"insert_ok":1' '"select_ok":1'
else
  echo "=== 跳过 Redis/MySQL 协程用例（TestHelloCoRedis / TestHelloCoMysql）；如需启用请设置 HELLO_TEST_REDIS_MYSQL=1 ==="
fi

echo "=== Hello 冒烟通过 ==="

