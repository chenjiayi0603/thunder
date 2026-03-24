#!/usr/bin/env bash
# Interface HTTP 冒烟（Docker Compose 场景）
#
# 假定已在本目录执行 docker compose up -d，且 compose 使用 network_mode: host（与 conf 中 127.0.0.1 一致）。
# 本脚本只在宿主机发 curl，不要求 Interface 可执行文件在宿主机；但卷挂载下须存在
#   deploy/Interface/plugins/ModuleInterface.so
#（与 conf 中 module.so_path 一致），否则常见 curl (52) Empty reply。
#
# 用法（在 deploy/docker 下）:
#   ./test_interfaceserver_smoke.sh
#
# 可选环境变量（与 deploy/tests/test_interfaceserver.sh 语义一致）:
#   INTERFACE_HOST INTERFACE_PORT INTERFACE_PATH  — 默认 127.0.0.1:27008 /Interface/gentoken
#   GENKEY_VERIFY_MAXTIME                         — curl 超时秒数（默认 120）
#   PRE_CURL_SEC                                  — 发请求前额外 sleep 秒数（默认 0）
#   REQUIRE_PORTS=1                               — 为 1 时先检查 Interface access 端口已 LISTEN
#   SKIP_PLUGIN_CHECK=1                           — 跳过 ModuleInterface.so 存在性检查（一般勿用）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

INTERFACE_HOST="${INTERFACE_HOST:-127.0.0.1}"
INTERFACE_PORT="${INTERFACE_PORT:-27008}"
INTERFACE_PATH="${INTERFACE_PATH:-/Interface/gentoken}"
GENKEY_VERIFY_MAXTIME="${GENKEY_VERIFY_MAXTIME:-120}"
PRE_CURL_SEC="${PRE_CURL_SEC:-0}"
REQUIRE_PORTS="${REQUIRE_PORTS:-0}"
SKIP_PLUGIN_CHECK="${SKIP_PLUGIN_CHECK:-0}"
PLUGIN_SO="${DEPLOY_ROOT}/Interface/plugins/ModuleInterface.so"

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

# 避免 LD_LIBRARY_PATH 中的 libcurl 干扰系统 curl（与 test_interfaceserver.sh 一致）
_curl_smoke() {
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' "$@"
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' "$@"
  fi
}

_parse_genkey_json() {
  local json="$1"
  if command -v jq >/dev/null 2>&1; then
    TOKEN=$(echo "$json" | jq -r '.token // empty')
    KEY=$(echo "$json" | jq -r '.key // empty')
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    TOKEN=$(echo "$json" | python3 -c 'import json,sys; d=json.load(sys.stdin); t=d.get("token"); print("" if t is None else str(t))')
    KEY=$(echo "$json" | python3 -c 'import json,sys; d=json.load(sys.stdin); k=d.get("key"); print("" if k is None else str(k))')
    return 0
  fi
  echo "错误: 解析 GenKey 响应需要 jq 或 python3" >&2
  return 1
}

_build_verifykey_json() {
  if command -v jq >/dev/null 2>&1; then
    jq -n --arg t "${TOKEN}" --arg k "${KEY}" '{option:"VerifyKey",token:$t,key:$k}'
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import json,sys; print(json.dumps({"option":"VerifyKey","token":sys.argv[1],"key":sys.argv[2]}))' "${TOKEN}" "${KEY}"
    return 0
  fi
  echo "错误: 构造 VerifyKey 请求体需要 jq 或 python3" >&2
  return 1
}

if [[ "${SKIP_PLUGIN_CHECK}" != "1" && ! -f "${PLUGIN_SO}" ]]; then
  echo "错误: 缺少 ${PLUGIN_SO}（Interface.json 中 /Interface/gentoken 依赖此插件）。" >&2
  echo "请编译并安装: 在仓库根 cmake --build <build> --target ModuleInterface && cmake --install <build>" >&2
  echo "或: SKIP_PLUGIN_CHECK=1 ./test_interfaceserver_smoke.sh（仅当你确认容器内另有挂载路径）" >&2
  exit 1
fi

if [[ "${REQUIRE_PORTS}" == "1" ]]; then
  if ! _tcp_listening "${INTERFACE_PORT}"; then
    echo "错误: ${INTERFACE_HOST}:${INTERFACE_PORT} 未监听（请先 docker compose up -d 或检查 network_mode: host）" >&2
    exit 1
  fi
  echo "已检测到端口监听: ${INTERFACE_PORT}"
fi

if [[ "${PRE_CURL_SEC}" != "0" ]]; then
  echo "=== PRE_CURL_SEC=${PRE_CURL_SEC}s，等待后再发 HTTP ==="
  sleep "${PRE_CURL_SEC}"
fi

BASE_URL="http://${INTERFACE_HOST}:${INTERFACE_PORT}${INTERFACE_PATH}"
echo "=== Docker 场景 HTTP 冒烟：GenKey → VerifyKey — ${BASE_URL} ==="
echo "=== 排障: docker compose logs interface；宿主机日志 ${DEPLOY_ROOT}/Interface/log/ ==="

GENKEY_BODY=""
if ! GENKEY_BODY=$(_curl_smoke -f -sS -m "${GENKEY_VERIFY_MAXTIME}" -X POST "${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"option":"GenKey"}' \
    -w '\n[HTTP %{http_code}]\n'); then
  echo "错误: GenKey 失败。" >&2
  echo "  (52) Empty reply：多为未加载 gentoken 插件、异步链路异常或 Center/Logic 未就绪；先确认 ${PLUGIN_SO} 存在且 Interface 日志无 FATAL。" >&2
  echo "  (404)：路由未注册，仍多为 ModuleInterface.so 未加载。" >&2
  exit 1
fi
echo "=== GenKey 响应（节选）==="
echo "${GENKEY_BODY}" | head -n -1

TOKEN=""
KEY=""
if ! _parse_genkey_json "$(echo "${GENKEY_BODY}" | head -n -1)"; then
  exit 1
fi
if [[ -z "${TOKEN}" || -z "${KEY}" ]]; then
  echo "错误: GenKey 响应中未解析到 token/key" >&2
  exit 1
fi

VERIFY_JSON=""
if ! VERIFY_JSON=$(_build_verifykey_json); then
  exit 1
fi

VERIFY_BODY=""
if ! VERIFY_BODY=$(_curl_smoke -f -sS -m "${GENKEY_VERIFY_MAXTIME}" -X POST "${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d "${VERIFY_JSON}" \
    -w '\n[HTTP %{http_code}]\n'); then
  echo "错误: VerifyKey 失败。请查 Interface/Logic 日志与 CmdGetToken 配置" >&2
  exit 1
fi
echo "=== VerifyKey 响应（节选）==="
echo "${VERIFY_BODY}" | head -n -1
echo "=== VerifyKey 完成（docker 冒烟通过）==="
