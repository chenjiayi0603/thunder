#!/usr/bin/env bash
# Hello HTTPS 冒烟（Docker Compose 场景，验证 CODEC_HTTPS 握手 + 业务路由）
#
# 用法（在 deploy/docker 下）:
#   ./test_helloserver_https_smoke.sh
#
# 可选环境变量:
#   HELLO_HTTPS_HOST HELLO_HTTPS_PORT HELLO_HTTPS_PATH  默认 127.0.0.1:27443 /hello/hello
#   CURL_MAXTIME_HELLO                                 默认 60
#   PRE_CURL_SEC                                       默认 0
#   REQUIRE_PORTS                                      为 1 时先检查监听
#   SKIP_PLUGIN_CHECK                                  为 1 时跳过插件检查
#   START_HTTPS_NODE                                   为 1 时尝试 docker compose exec 启动 HelloHttps（默认 1）
#   GENERATE_CERT                                      为 1 时缺证书自动生成（默认 1）
#   INSECURE_TLS                                       为 1 时使用 curl -k 跳过证书校验（默认 0）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HELLO_DIR="${DEPLOY_ROOT}/HelloHttps"

HELLO_HTTPS_HOST="${HELLO_HTTPS_HOST:-127.0.0.1}"
HELLO_HTTPS_PORT="${HELLO_HTTPS_PORT:-27443}"
HELLO_HTTPS_PATH="${HELLO_HTTPS_PATH:-/hello/hello}"
CURL_MAXTIME_HELLO="${CURL_MAXTIME_HELLO:-60}"
PRE_CURL_SEC="${PRE_CURL_SEC:-0}"
REQUIRE_PORTS="${REQUIRE_PORTS:-0}"
SKIP_PLUGIN_CHECK="${SKIP_PLUGIN_CHECK:-0}"
START_HTTPS_NODE="${START_HTTPS_NODE:-1}"
GENERATE_CERT="${GENERATE_CERT:-1}"
INSECURE_TLS="${INSECURE_TLS:-0}"

PLUGIN_SO="${DEPLOY_ROOT}/HelloHttps/plugins/ModuleHello.so"
CA_CRT="${HELLO_DIR}/conf/certs/ca.crt"
SERVER_CRT="${HELLO_DIR}/conf/certs/server.crt"
SERVER_KEY="${HELLO_DIR}/conf/certs/server.key"
CERT_GEN_SCRIPT="${HELLO_DIR}/scripts/gen_self_signed_https_cert.sh"

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

_start_https_node_if_needed() {
  if [[ "${START_HTTPS_NODE}" != "1" ]]; then
    return 0
  fi
  if ! command -v docker >/dev/null 2>&1; then
    echo "警告: 未找到 docker，跳过自动启动 HelloHttps。"
    return 0
  fi
  echo "=== 启动 HelloHttps 节点（docker compose exec hello_https ./start.sh） ==="
  (
    cd "${SCRIPT_DIR}"
    docker compose exec -T hello_https bash -lc "cd /thunder/deploy/HelloHttps && chmod +x ./start.sh ./scripts/gen_self_signed_https_cert.sh && ./scripts/gen_self_signed_https_cert.sh && ./start.sh" >/dev/null 2>&1 || true
  )
}

_https_post_check() {
  local name="$1"
  local payload="$2"
  shift 2

  local tmp code
  tmp="$(mktemp)"

  local url="https://${HELLO_HTTPS_HOST}:${HELLO_HTTPS_PORT}${HELLO_HTTPS_PATH}"
  echo "=== ${name}: POST ${url} ==="

  if [[ "${INSECURE_TLS}" == "1" ]]; then
    # 执行 HTTPS POST 请求，例如：
    # curl -k -sS -o /tmp/tmpfile -w "%{http_code}" --max-time 60 \
    #   -X POST "https://127.0.0.1:27007/hello/hello" -H 'Content-Type: application/json' -d '{"msg": "hi"}'
    code="$(
      _curl_smoke -k -sS -o "${tmp}" -w "%{http_code}" --max-time "${CURL_MAXTIME_HELLO}" \
        -X POST "${url}" -H 'Content-Type: application/json' -d "${payload}" || true
    )"
  else
    # 执行 HTTPS POST 请求，例如：
    # curl --cacert ./ca.crt -sS -o /tmp/tmpfile -w "%{http_code}" --max-time 60 \
    #   -X POST "https://127.0.0.1:27007/hello/hello" -H 'Content-Type: application/json' -d '{"msg": "hi"}'
    code="$(
      _curl_smoke --cacert "${CA_CRT}" -sS -o "${tmp}" -w "%{http_code}" --max-time "${CURL_MAXTIME_HELLO}" \
        -X POST "${url}" -H 'Content-Type: application/json' -d "${payload}" || true
    )"
  fi

  cat "${tmp}"
  echo ""

  if [[ "${code}" != "200" ]]; then
    echo "错误: ${name} HTTPS HTTP ${code}（期望 200）" >&2
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
  exit 1
fi

if [[ ! -f "${SERVER_CRT}" || ! -f "${SERVER_KEY}" || ! -f "${CA_CRT}" ]]; then
  if [[ "${GENERATE_CERT}" == "1" ]]; then
    echo "=== 证书不存在，开始生成自签证书 ==="
    bash "${CERT_GEN_SCRIPT}"
  else
    echo "错误: 缺少 HTTPS 证书，请先执行 ${CERT_GEN_SCRIPT}" >&2
    exit 1
  fi
fi

_start_https_node_if_needed

if [[ "${PRE_CURL_SEC}" != "0" ]]; then
  echo "=== PRE_CURL_SEC=${PRE_CURL_SEC}s，等待后再发 HTTPS ==="
  sleep "${PRE_CURL_SEC}"
fi

if [[ "${REQUIRE_PORTS}" == "1" ]]; then
  if ! _tcp_listening "${HELLO_HTTPS_PORT}"; then
    echo "错误: ${HELLO_HTTPS_HOST}:${HELLO_HTTPS_PORT} 未监听（请检查 HelloHttps 是否启动）" >&2
    exit 1
  fi
  echo "已检测到 HTTPS 端口监听: ${HELLO_HTTPS_PORT}"
fi

_https_post_check 'TLSHandshake+Echo' '{"option":"Echo"}' '"code"' '"msg"'
_https_post_check 'TLSHandshake+TestHelloPoolCpu' '{"option":"TestHelloPoolCpu"}' 'TestHelloPoolCpu' '786432'
_https_post_check 'TLSHandshake+TestHelloPoolBlock' '{"option":"TestHelloPoolBlock"}' 'TestHelloPoolBlock' '"slept_ms":80'

echo "=== Hello HTTPS 冒烟通过（CODEC_HTTPS 双向握手链路） ==="

