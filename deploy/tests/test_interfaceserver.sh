#!/usr/bin/env bash
# 一键联调：简易注册中心 Center → Logic → Interface → HTTP 冒烟
#
# Center：简易注册中心（节点注册与路由）。Logic / Interface 向 Center 登记后，Interface 经 Center 将请求转发到 LOGIC。
# 默认启动顺序：
#   [1/4] Center ×3（conf/Center.json、conf2、conf3，内网 27000/27022/27032，与 CenterCmd.json 中 Raft centers 一致）
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
#   SKIP_CENTER_START=1                           — 不启动 Center（假定已有多实例或其它脚本已拉起）；与 REQUIRED_CENTER_INNER_PORTS 联用
#   REQUIRED_CENTER_INNER_PORTS="27000 27022 ..." — SKIP_CENTER_START=1 时校验这些内网端口均已 LISTEN（默认与 NUM_CENTER_INSTANCES 一致）
#   NUM_CENTER_INSTANCES=3|1                      — 启动 Center 个数（默认 3，与 CenterCmd.json Raft 成员一致）；设为 1 时需将 CenterCmd.json 的 centers 置为 [] 否则选不出 Leader
#   CENTER_RAFT_SETTLE_SEC                        — 三 Center 全部监听后等待 Raft 选主（秒，默认 5，可按机器调大）
#   CENTER_CONF CENTER_CONF_2 CENTER_CONF_3       — 相对 deploy/Center，默认 conf/Center.json、conf2/Center.json、conf3/Center.json
#   CENTER_CONF LOGIC_CONF                        — 单 Center 模式时的主配置；Logic 默认 conf/Logic.json
#   CENTER_BIN LOGIC_BIN                          — 可执行文件路径（默认 deploy/Center/bin/Center、deploy/Logic/bin/Logic）
#   CENTER_PORT LOGIC_PORT                        — 单 Center 时检测端口（默认 27000）；多 Center 时检测 27000/27022/27032
#   LOGIC_REGISTER_WAIT_SEC                       — Logic 启动后等待向 Center 注册的秒数（默认：三 Center 时 8，单 Center 时 4）
#   INTERFACE_ROUTE_READY_SEC                     — Interface 启动后再等待秒数再发 GenKey（默认：三 Center 时 4，否则同 STARTUP_WAIT_SEC）
#   INTERFACE_HOST INTERFACE_PORT INTERFACE_PATH — HTTP 探测地址（默认与 conf/Interface.json 中 access 一致）
#   INTERFACE_BIN                                 — 可执行文件路径（默认优先 Interface/bin/Interface，否则回退 Hello/bin/Hello）
#   GENKEY_VERIFY_MAXTIME                         — GenKey/VerifyKey 单次 curl 超时秒数（默认 120，与 step_timeout 对齐）
#   SKIP_CLEAN_LOGS=1                             — 启动前不删除 Center/Interface/Logic 目录下 *.log
#
# 测试结束（成功或中途失败）后，会自动关闭本脚本本次启动的 Center / Logic / Interface 进程（SKIP_CENTER_LOGIC=1 时仅 Interface）。

set -euo pipefail

# 本脚本启动的进程 PID（用于 EXIT 时统一关闭）
CENTER_TEST_PIDS=()
LOGIC_TEST_PID=""
INTERFACE_TEST_PID=""

# 按依赖逆序停止：Interface → Logic → Center（多 Center 逐个 kill）
_cleanup_test_servers() {
  if [[ -n "${INTERFACE_TEST_PID}" ]] && kill -0 "${INTERFACE_TEST_PID}" 2>/dev/null; then
    kill "${INTERFACE_TEST_PID}" 2>/dev/null || true
    wait "${INTERFACE_TEST_PID}" 2>/dev/null || true
  fi
  if [[ -n "${LOGIC_TEST_PID}" ]] && kill -0 "${LOGIC_TEST_PID}" 2>/dev/null; then
    kill "${LOGIC_TEST_PID}" 2>/dev/null || true
    wait "${LOGIC_TEST_PID}" 2>/dev/null || true
  fi
  for _cpid in "${CENTER_TEST_PIDS[@]}"; do
    if [[ -n "${_cpid}" ]] && kill -0 "${_cpid}" 2>/dev/null; then
      kill "${_cpid}" 2>/dev/null || true
      wait "${_cpid}" 2>/dev/null || true
    fi
  done
}

trap _cleanup_test_servers EXIT

# 本脚本位于 deploy/tests/，deploy 根目录为其父目录
DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
CONF="${CONF:-conf/Interface.json}"

SKIP_CENTER_LOGIC="${SKIP_CENTER_LOGIC:-0}"
SKIP_CENTER_START="${SKIP_CENTER_START:-0}"
NUM_CENTER_INSTANCES="${NUM_CENTER_INSTANCES:-3}"
CENTER_RAFT_SETTLE_SEC="${CENTER_RAFT_SETTLE_SEC:-5}"
REQUIRED_CENTER_INNER_PORTS="${REQUIRED_CENTER_INNER_PORTS:-}"
CENTER_CONF="${CENTER_CONF:-conf/Center.json}"
CENTER_CONF_2="${CENTER_CONF_2:-conf2/Center.json}"
CENTER_CONF_3="${CENTER_CONF_3:-conf3/Center.json}"
LOGIC_CONF="${LOGIC_CONF:-conf/Logic.json}"
CENTER_BIN_DEFAULT="${DEPLOY_ROOT}/Center/bin/Center"
LOGIC_BIN_DEFAULT="${DEPLOY_ROOT}/Logic/bin/Logic"
CENTER_BIN="${CENTER_BIN:-${CENTER_BIN_DEFAULT}}"
LOGIC_BIN="${LOGIC_BIN:-${LOGIC_BIN_DEFAULT}}"
CENTER_PORT="${CENTER_PORT:-27000}"
CENTER_PORT_2="${CENTER_PORT_2:-27022}"
CENTER_PORT_3="${CENTER_PORT_3:-27032}"
LOGIC_PORT="${LOGIC_PORT:-16068}"

# 三 Center + Raft 时 Logic 连上 Leader 并完成注册往往 >4s；过短会导致 Interface 已起但 Center 尚未下发 LOGIC 路由
if [[ "${NUM_CENTER_INSTANCES}" == "3" ]]; then
  LOGIC_REGISTER_WAIT_SEC="${LOGIC_REGISTER_WAIT_SEC:-8}"
else
  LOGIC_REGISTER_WAIT_SEC="${LOGIC_REGISTER_WAIT_SEC:-4}"
fi

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

  if [[ "${SKIP_CENTER_START}" == "1" ]]; then
    echo "=== SKIP_CENTER_START=1：跳过启动 Center，使用已运行的实例 ==="
    _center_ports_check=()
    if [[ -n "${REQUIRED_CENTER_INNER_PORTS}" ]]; then
      read -r -a _center_ports_check <<< "${REQUIRED_CENTER_INNER_PORTS}"
    elif [[ "${NUM_CENTER_INSTANCES}" == "3" ]]; then
      _center_ports_check=("${CENTER_PORT}" "${CENTER_PORT_2}" "${CENTER_PORT_3}")
    else
      _center_ports_check=("${CENTER_PORT}")
    fi
    for _cp in "${_center_ports_check[@]}"; do
      if ! _tcp_listening "${_cp}"; then
        echo "错误: Center 内网端口 ${_cp} 未监听（请设 REQUIRED_CENTER_INNER_PORTS 或先启动多 Center）" >&2
        exit 1
      fi
      echo "已检测到 Center inner 端口监听: ${_cp}"
    done
    CENTER_TEST_PIDS=()
  else
    _stop_center_robots() {
      pkill Center_robot 2>/dev/null || true
      pkill Center_robot2 2>/dev/null || true
      pkill Center_robot3 2>/dev/null || true
    }
    if [[ "${NUM_CENTER_INSTANCES}" == "3" ]]; then
      _center_listen_ports=("${CENTER_PORT}" "${CENTER_PORT_2}" "${CENTER_PORT_3}")
    else
      _center_listen_ports=("${CENTER_PORT}")
    fi
    # 若目标端口已被占用则先停止 Center_robot*（保证本次联调用新进程）
    _need_free=0
    for _p in "${_center_listen_ports[@]}"; do
      if _tcp_listening "${_p}"; then
        _need_free=1
        break
      fi
    done
    if [[ "${_need_free}" == "1" ]]; then
      echo "提示: Center 目标端口 ${_center_listen_ports[*]} 中有占用，先停止 Center_robot / Center_robot2 / Center_robot3"
      _stop_center_robots
      sleep 1
      for _i in $(seq 1 30); do
        _still=0
        for _p in "${_center_listen_ports[@]}"; do
          if _tcp_listening "${_p}"; then
            _still=1
            break
          fi
        done
        [[ "${_still}" == "0" ]] && break
        sleep 1
      done
      for _p in "${_center_listen_ports[@]}"; do
        if _tcp_listening "${_p}"; then
          echo "错误: 停止 Center 后端口 ${_p} 仍被占用，请手动处理" >&2
          exit 1
        fi
      done
    fi

    mkdir -p "${DEPLOY_ROOT}/Center/log"
    _center_log="${DEPLOY_ROOT}/Center/log/test_interfaceserver.log"

    if [[ "${NUM_CENTER_INSTANCES}" == "3" ]]; then
      echo "=== [1/4] 启动 3 个 Center（Raft）— ${CENTER_CONF}、${CENTER_CONF_2}、${CENTER_CONF_3} ==="
      (
        cd "${DEPLOY_ROOT}/Center"
        nohup "${CENTER_BIN}" "${CENTER_CONF}" >>"${_center_log}" 2>&1 &
        echo $! >log/test_interfaceserver_c1.pid
      )
      CENTER_TEST_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_interfaceserver_c1.pid")")
      (
        cd "${DEPLOY_ROOT}/Center"
        nohup "${CENTER_BIN}" "${CENTER_CONF_2}" >>"${_center_log}" 2>&1 &
        echo $! >log/test_interfaceserver_c2.pid
      )
      CENTER_TEST_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_interfaceserver_c2.pid")")
      (
        cd "${DEPLOY_ROOT}/Center"
        nohup "${CENTER_BIN}" "${CENTER_CONF_3}" >>"${_center_log}" 2>&1 &
        echo $! >log/test_interfaceserver_c3.pid
      )
      CENTER_TEST_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_interfaceserver_c3.pid")")
      echo "Center PIDs=${CENTER_TEST_PIDS[*]}（log/test_interfaceserver_c1.pid … c3.pid）"
      for _p in "${CENTER_PORT}" "${CENTER_PORT_2}" "${CENTER_PORT_3}"; do
        for _i in $(seq 1 40); do
          _tcp_listening "${_p}" && break
          sleep 0.5
        done
        if ! _tcp_listening "${_p}"; then
          echo "错误: Center 未在 ${_p} 监听，见 ${_center_log}" >&2
          exit 1
        fi
      done
      echo "=== 等待 Raft 选主收敛（CENTER_RAFT_SETTLE_SEC=${CENTER_RAFT_SETTLE_SEC}s）==="
      sleep "${CENTER_RAFT_SETTLE_SEC}"
    else
      echo "=== [1/4] 启动单个 Center — ${CENTER_CONF}，监听 ${CENTER_PORT}（请确认 CenterCmd.json 中 centers 与实例数一致）==="
      (
        cd "${DEPLOY_ROOT}/Center"
        nohup "${CENTER_BIN}" "${CENTER_CONF}" >>"${_center_log}" 2>&1 &
        echo $! >log/test_interfaceserver_center.pid
      )
      CENTER_TEST_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_interfaceserver_center.pid")")
      echo "Center PID=${CENTER_TEST_PIDS[0]}（见 ${DEPLOY_ROOT}/Center/log/test_interfaceserver_center.pid）"
      for _i in $(seq 1 30); do
        _tcp_listening "${CENTER_PORT}" && break
        sleep 1
      done
      if ! _tcp_listening "${CENTER_PORT}"; then
        echo "错误: Center 未在 ${CENTER_PORT} 监听，见 ${_center_log}" >&2
        exit 1
      fi
    fi
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

# Interface 连 Center 注册与收到 NODE_REG_NOTICE（含 LOGIC）略晚于 Logic 注册完成；多 Center 时多留几秒再 curl
_iface_smoke_wait="${STARTUP_WAIT_SEC:-2}"
if [[ "${SKIP_CENTER_LOGIC}" != "1" && "${NUM_CENTER_INSTANCES}" == "3" ]]; then
  _iface_smoke_wait="${INTERFACE_ROUTE_READY_SEC:-4}"
fi
echo "=== Interface 启动后等待 ${_iface_smoke_wait}s 再发 HTTP（避免 Center 尚未下发 LOGIC 路由）==="
sleep "${_iface_smoke_wait}"

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
