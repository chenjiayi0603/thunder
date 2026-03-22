#!/usr/bin/env bash
# 一键联调：简易注册中心 Center → Logic → Interface → HTTP 冒烟
#
# Center：简易注册中心（节点注册与路由）。Logic / Interface 向 Center 登记后，Interface 经 Center 将请求转发到 LOGIC。
# 默认启动顺序：
#   [1/4] Center
#   [2/4] Logic（向 Center 注册，脚本会短暂等待）
#   [3/4] Interface（若已有 Interface_robot 会先停止再拉起；HTTP 入口）
#   [4/4] HTTP 冒烟：GenKey → VerifyKey（路径 Interface→Center→LOGIC）
#
# 工作目录：deploy/Interface，配置默认 conf/Interface.json。
#
# 用法（在 deploy 目录下）: ./tests/test_interfaceserver.sh
#       CONF=conf/Interface.json ./tests/test_interfaceserver.sh
#
# 可选环境变量：
#   SKIP_CENTER_LOGIC=1                           — 不启动/不检查 Center 与 Logic（仅 Interface；无法验证发往 LOGIC）
#   CENTER_CONF LOGIC_CONF                        — Center/Logic 配置相对各自目录，默认 conf/Center.json、conf/Logic.json
#   CENTER_BIN LOGIC_BIN                          — 可执行文件路径（默认 deploy/Center/bin/Center、deploy/Logic/bin/Logic）
#   CENTER_PORT LOGIC_PORT                        — 用于检测是否已监听（默认 27000、16068，与默认 conf 一致）
#   LOGIC_REGISTER_WAIT_SEC                       — Logic 启动后等待向 Center 注册的秒数（默认 4）
#   INTERFACE_HOST INTERFACE_PORT INTERFACE_PATH — HTTP 探测地址（默认与 conf/Interface.json 中 access 一致）
#   INTERFACE_BIN                                 — 可执行文件路径（默认优先 Interface/bin/Interface，否则回退 Hello/bin/Hello）
#   GENKEY_VERIFY_MAXTIME                         — GenKey/VerifyKey 单次 curl 超时秒数（默认 120，与 step_timeout 对齐）
#   SKIP_CLEAN_LOGS=1                             — 启动前不删除 Center/Interface/Logic 目录下 *.log
#
# 测试结束（成功或中途失败）后，会自动关闭本脚本本次启动的 Center / Logic / Interface 进程（SKIP_CENTER_LOGIC=1 时仅 Interface）。

set -euo pipefail

# 本脚本启动的进程 PID（用于 EXIT 时统一关闭）
CENTER_TEST_PID=""
LOGIC_TEST_PID=""
INTERFACE_TEST_PID=""

# 按依赖逆序停止：Interface → Logic → Center
_cleanup_test_servers() {
  if [[ -n "${INTERFACE_TEST_PID}" ]] && kill -0 "${INTERFACE_TEST_PID}" 2>/dev/null; then
    kill "${INTERFACE_TEST_PID}" 2>/dev/null || true
    wait "${INTERFACE_TEST_PID}" 2>/dev/null || true
  fi
  if [[ -n "${LOGIC_TEST_PID}" ]] && kill -0 "${LOGIC_TEST_PID}" 2>/dev/null; then
    kill "${LOGIC_TEST_PID}" 2>/dev/null || true
    wait "${LOGIC_TEST_PID}" 2>/dev/null || true
  fi
  if [[ -n "${CENTER_TEST_PID}" ]] && kill -0 "${CENTER_TEST_PID}" 2>/dev/null; then
    kill "${CENTER_TEST_PID}" 2>/dev/null || true
    wait "${CENTER_TEST_PID}" 2>/dev/null || true
  fi
}

trap _cleanup_test_servers EXIT

# 本脚本位于 deploy/tests/，deploy 根目录为其父目录
DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Interface.json}"

SKIP_CENTER_LOGIC="${SKIP_CENTER_LOGIC:-0}"
CENTER_CONF="${CENTER_CONF:-conf/Center.json}"
LOGIC_CONF="${LOGIC_CONF:-conf/Logic.json}"
CENTER_BIN_DEFAULT="${DEPLOY_ROOT}/Center/bin/Center"
LOGIC_BIN_DEFAULT="${DEPLOY_ROOT}/Logic/bin/Logic"
CENTER_BIN="${CENTER_BIN:-${CENTER_BIN_DEFAULT}}"
LOGIC_BIN="${LOGIC_BIN:-${LOGIC_BIN_DEFAULT}}"
CENTER_PORT="${CENTER_PORT:-27000}"
LOGIC_PORT="${LOGIC_PORT:-16068}"

INTERFACE_HOST="${INTERFACE_HOST:-127.0.0.1}"
INTERFACE_PORT="${INTERFACE_PORT:-27008}"
INTERFACE_PATH="${INTERFACE_PATH:-/Interface/gentoken}"
GENKEY_VERIFY_MAXTIME="${GENKEY_VERIFY_MAXTIME:-120}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# 探测本机 TCP 端口是否在 LISTEN（用于判断 Center/Logic 是否已起）
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

# 探测 HTTP 时用系统 curl + 系统 libcurl。若带着上面的 LD_LIBRARY_PATH 调 curl，会链到 3party/libcurl.so，
# 易出现：「no version information available (required by curl)」。
# --noproxy '*'：避免环境变量 http_proxy 把 127.0.0.1 走 SOCKS/HTTP 代理，导致 (52) Empty reply 或超时。
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
  echo "提示: 未找到 Interface/bin/Interface，使用 Hello 二进制 + Interface 配置（Net 主程序入口相同）" >&2
else
  echo "错误: 未找到可执行文件。请先编译安装：deploy/Interface/bin/Interface 或 deploy/Hello/bin/Hello" >&2
  exit 1
fi
if [[ ! -x "${BIN}" ]]; then
  echo "错误: 不可执行: ${BIN}" >&2
  exit 1
fi

# Worker 按 conf 里 module.url_path 加载 so；若 plugins 下无 ModuleInterface.so，请求会 404（见 Worker::Dispose）
mkdir -p "${DEPLOY_ROOT}/Interface/plugins"
PLUGIN_SO="${DEPLOY_ROOT}/Interface/plugins/ModuleInterface.so"
if [[ ! -f "${PLUGIN_SO}" ]]; then
  BUILT_SO="${DEPLOY_ROOT}/../build/lib/ModuleInterface.so"
  if [[ -f "${BUILT_SO}" ]]; then
    echo "提示: 部署插件 ${PLUGIN_SO}（从 ${BUILT_SO} 复制）" >&2
    install -m0644 "${BUILT_SO}" "${PLUGIN_SO}"
  else
    echo "错误: 未找到 ${PLUGIN_SO}，模块未加载时 HTTP 会返回 404。" >&2
    echo "请先编译: cmake --build <build> --target ModuleInterface" >&2
    echo "再复制: install -D <build>/lib/ModuleInterface.so ${PLUGIN_SO}" >&2
    exit 1
  fi
fi

# 启动前清理 deploy/Center|Interface|Logic/log 下日志文件（便于本次联调日志干净）
# 注意：仅 -name '*.log' 不会匹配 log4cplus 轮转名 *.log.1、*.log.10 等（不以 .log 结尾）
mkdir -p "${DEPLOY_ROOT}/Center/log" "${DEPLOY_ROOT}/Interface/log" "${DEPLOY_ROOT}/Logic/log"
if [[ "${SKIP_CLEAN_LOGS:-0}" != "1" ]]; then
  echo "=== 清理日志: Center/log Interface/log Logic/log 下 *.log 与 *.log.*（轮转）==="
  _cleaned="$(find "${DEPLOY_ROOT}/Center/log" "${DEPLOY_ROOT}/Interface/log" "${DEPLOY_ROOT}/Logic/log" \
    -maxdepth 1 -type f \( -name '*.log' -o -name '*.log.*' \) -print 2>/dev/null | wc -l)"
  find "${DEPLOY_ROOT}/Center/log" "${DEPLOY_ROOT}/Interface/log" "${DEPLOY_ROOT}/Logic/log" \
    -maxdepth 1 -type f \( -name '*.log' -o -name '*.log.*' \) -delete 2>/dev/null || true
  echo "已删除 ${_cleaned} 个日志文件（SKIP_CLEAN_LOGS=1 可跳过）"
else
  echo "提示: SKIP_CLEAN_LOGS=1，未清理 *.log / *.log.*"
fi

if [[ "${SKIP_CENTER_LOGIC}" != "1" ]]; then
  echo "=== 联调顺序：Center(注册中心) → Logic → Interface → HTTP 测试 ==="
  if [[ ! -x "${CENTER_BIN}" ]]; then
    echo "错误: 未找到可执行 Center: ${CENTER_BIN}" >&2
    echo "请编译后复制: cmake --build <build> --target Center → deploy/Center/bin/Center，或设置 CENTER_BIN=" >&2
    exit 1
  fi
  if [[ ! -x "${LOGIC_BIN}" ]]; then
    echo "错误: 未找到可执行 Logic: ${LOGIC_BIN}" >&2
    echo "请使用与 Hello 相同主程序复制到 deploy/Logic/bin/Logic，或设置 LOGIC_BIN=" >&2
    exit 1
  fi

  # Center：若端口已被占用则先停止再启动（与 Interface 一致，保证本次联调用新进程）
  if _tcp_listening "${CENTER_PORT}"; then
    echo "提示: 端口 ${CENTER_PORT} 已在监听，先停止 Center_robot"
    pkill Center_robot 2>/dev/null || true
    sleep 1
    for _i in $(seq 1 30); do
      _tcp_listening "${CENTER_PORT}" || break
      sleep 1
    done
    if _tcp_listening "${CENTER_PORT}"; then
      echo "错误: 停止 Center 后端口 ${CENTER_PORT} 仍被占用，请手动处理" >&2
      exit 1
    fi
  fi
  echo "=== [1/4] 启动简易注册中心 Center — ${CENTER_CONF}，监听 ${CENTER_PORT} ==="
  mkdir -p "${DEPLOY_ROOT}/Center/log"
  (
    cd "${DEPLOY_ROOT}/Center"
    nohup "${CENTER_BIN}" "${CENTER_CONF}" >> log/test_interfaceserver.log 2>&1 &
    echo $! >log/test_interfaceserver_center.pid
  )
  CENTER_TEST_PID="$(cat "${DEPLOY_ROOT}/Center/log/test_interfaceserver_center.pid")"
  echo "Center PID=${CENTER_TEST_PID}（见 ${DEPLOY_ROOT}/Center/log/test_interfaceserver_center.pid）"
  for _i in $(seq 1 30); do
    _tcp_listening "${CENTER_PORT}" && break
    sleep 1
  done
  if ! _tcp_listening "${CENTER_PORT}"; then
    echo "错误: Center 未在 ${CENTER_PORT} 监听，见 ${DEPLOY_ROOT}/Center/log/test_interfaceserver.log" >&2
    exit 1
  fi

  if _tcp_listening "${LOGIC_PORT}"; then
    echo "提示: 端口 ${LOGIC_PORT} 已在监听，先停止 Logic_robot"
    pkill Logic_robot 2>/dev/null || true
    sleep 1
    for _i in $(seq 1 30); do
      _tcp_listening "${LOGIC_PORT}" || break
      sleep 1
    done
    if _tcp_listening "${LOGIC_PORT}"; then
      echo "错误: 停止 Logic 后端口 ${LOGIC_PORT} 仍被占用，请手动处理" >&2
      exit 1
    fi
  fi
  echo "=== [2/4] 启动 Logic 节点 — ${LOGIC_CONF}，监听 ${LOGIC_PORT}（向 Center 注册）==="
  mkdir -p "${DEPLOY_ROOT}/Logic/log"
  (
    cd "${DEPLOY_ROOT}/Logic"
    nohup "${LOGIC_BIN}" "${LOGIC_CONF}" >> log/test_interfaceserver.log 2>&1 &
    echo $! >log/test_interfaceserver_logic.pid
  )
  LOGIC_TEST_PID="$(cat "${DEPLOY_ROOT}/Logic/log/test_interfaceserver_logic.pid")"
  echo "Logic PID=${LOGIC_TEST_PID}（见 ${DEPLOY_ROOT}/Logic/log/test_interfaceserver_logic.pid）"
  for _i in $(seq 1 30); do
    _tcp_listening "${LOGIC_PORT}" && break
    sleep 1
  done
  if ! _tcp_listening "${LOGIC_PORT}"; then
    echo "错误: Logic 未在 ${LOGIC_PORT} 监听，见 ${DEPLOY_ROOT}/Logic/log/test_interfaceserver.log 与 Logic_robot.log" >&2
    echo "常见原因: conf/Logic.json 中 inner_host 非本机地址（日志或见 error 99 Cannot assign requested address，请改为 127.0.0.1）；" >&2
    echo "  LD_LIBRARY_PATH 缺 libmariadb；或 plugins/CmdGetToken.so 未就绪。" >&2
    exit 1
  fi

  echo "=== 等待 Logic 在 Center 上完成注册（${LOGIC_REGISTER_WAIT_SEC:-4}s）==="
  sleep "${LOGIC_REGISTER_WAIT_SEC:-4}"
else
  echo "提示: SKIP_CENTER_LOGIC=1，跳过 Center/Logic（仅 [1/2] Interface + [2/2] HTTP）" >&2
fi

if [[ "${SKIP_CENTER_LOGIC}" == "1" ]]; then
  echo "=== [1/2] 启动 Interface — 若已有 Interface_robot 则先停止 ==="
else
  echo "=== [3/4] 启动 Interface — 若已有 Interface_robot 则先停止 ==="
fi
pkill Interface_robot 2>/dev/null || true
sleep 1

cd "${DEPLOY_ROOT}/Interface"
mkdir -p log

echo "=== 后台启动 Interface (${CONF})，HTTP 对外入口 ==="
nohup "${BIN}" "${CONF}" >> log/test_interfaceserver.log 2>&1 &
INTERFACE_TEST_PID=$!
echo "${INTERFACE_TEST_PID}" >log/test_interfaceserver_interface.pid
echo "PID=${INTERFACE_TEST_PID} 可执行: ${BIN}  日志: ${DEPLOY_ROOT}/Interface/log/test_interfaceserver.log"

sleep "${STARTUP_WAIT_SEC:-2}"

BASE_URL="http://${INTERFACE_HOST}:${INTERFACE_PORT}${INTERFACE_PATH}"
if [[ "${SKIP_CENTER_LOGIC}" == "1" ]]; then
  echo "=== [2/2] HTTP：GenKey → VerifyKey — ${BASE_URL} ==="
else
  echo "=== [4/4] HTTP：GenKey → VerifyKey（经 Interface → Center → LOGIC）— ${BASE_URL} ==="
fi

# GenKey：POST JSON {"option":"GenKey"}；VerifyKey：POST 同路径 JSON {"option":"VerifyKey","token","key"}
#（与 ModuleInterface DispatchJsonTestsFromBody + StepCo20Func 一致）
# --fail：4xx/5xx 时 curl 非 0
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

GENKEY_BODY=""
if ! GENKEY_BODY=$(_curl_smoke -f -sS -m "${GENKEY_VERIFY_MAXTIME}" -X POST "${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"option":"GenKey"}' \
    -w '\n[HTTP %{http_code}]\n'); then
  echo "错误: GenKey 请求失败。(404) 多为未部署 plugins/ModuleInterface.so；(52) 常为连接在异步回包前被关闭（需重编 Net/Interface）、Center/Logic 未起、或 step 超时" >&2
  echo "日志: ${DEPLOY_ROOT}/Interface/log/test_interfaceserver.log；Center/Logic 见各自 log/test_interfaceserver.log" >&2
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
  echo "错误: GenKey 响应中未解析到 token/key，无法执行 VerifyKey" >&2
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
  echo "错误: VerifyKey 请求失败。请核对 token/key 与 Logic CmdGetToken 配置；日志见 Interface/Logic" >&2
  echo "日志: ${DEPLOY_ROOT}/Interface/log/test_interfaceserver.log" >&2
  exit 1
fi
echo "=== VerifyKey 响应（节选）==="
echo "${VERIFY_BODY}" | head -n -1
echo "=== VerifyKey 完成 ==="

echo "=== 测试结束，正在关闭本次脚本启动的进程（Interface → Logic → Center）==="
trap - EXIT
_cleanup_test_servers
echo "=== 已关闭。联调日志仍保留: ${DEPLOY_ROOT}/Interface/log/test_interfaceserver.log 等 ==="
