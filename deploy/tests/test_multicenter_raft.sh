#!/usr/bin/env bash
# 多 Center（Raft）联调：三进程 + ModuleAdmin 校验唯一 Leader + 复用 test_interfaceserver 验证路由/node_id（Logic 注册）
#
# 前置：已编译 Center 插件（含 CmdRaftRequestVote.so、CmdRaftAppendEntries.so）与 Logic/Interface。
# 用法（在 deploy 目录）: ./tests/test_multicenter_raft.sh
#
# 环境变量：
#   RAFT_SETTLE_SEC     启动三 Center 后等待选举稳定的秒数（默认 5）
#   SKIP_INTERFACE_SMOKE=1  仅测 Raft + admin，不跑 GenKey/VerifyKey
#   BUILD_LIB             默认 ../build/lib
#
# 依赖：curl；解析 admin JSON 需 jq 或 python3（二者有其一即可）

set -euo pipefail

DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODE_ROOT="$(cd "${DEPLOY_ROOT}/../code" && pwd)"
BUILD_LIB="${BUILD_LIB:-${DEPLOY_ROOT}/../build/lib}"
RAFT_SETTLE_SEC="${RAFT_SETTLE_SEC:-5}"
FIXTURE="${DEPLOY_ROOT}/tests/fixtures/CenterCmd.multicenter.json"
CENTER_BIN="${CENTER_BIN:-${DEPLOY_ROOT}/Center/bin/Center}"

export LD_LIBRARY_PATH="${DEPLOY_ROOT}/lib:${CODE_ROOT}/3party/lib:${CODE_ROOT}/3party/lib/mariadb:${CODE_ROOT}/3party/protobuf/build${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

MULTI_PIDS=()

_cleanup_multicenter() {
  for pid in "${MULTI_PIDS[@]}"; do
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
}


_curl_admin() {
  local access_port="$1"
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' -sS -m 15 -X POST "http://127.0.0.1:${access_port}/admin" \
      -H 'Content-Type: application/json' \
      -d '{"cmd":"show","args":["center"]}'
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' -sS -m 15 -X POST "http://127.0.0.1:${access_port}/admin" \
      -H 'Content-Type: application/json' \
      -d '{"cmd":"show","args":["center"]}'
  fi
}

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

# admin show center 的 JSON 解析：优先 jq，否则 python3（避免仅因未装 jq 失败）
_have_admin_json_parser() {
  command -v jq >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1
}

# 从响应体取 .code（字符串）；合法成功一般为 0
_admin_json_code() {
  local body="$1"
  if command -v jq >/dev/null 2>&1; then
    echo "${body}" | jq -r '.code // empty'
  else
    echo "${body}" | python3 -c 'import json,sys; j=json.load(sys.stdin); c=j.get("code"); sys.stdout.write("" if c is None else str(c))'
  fi
}

# leader==yes 的条数
_admin_leader_yes_count() {
  local body="$1"
  if command -v jq >/dev/null 2>&1; then
    echo "${body}" | jq '[.data[]? | select(.leader == "yes")] | length'
  else
    echo "${body}" | python3 -c '
import json,sys
j=json.load(sys.stdin)
data=j.get("data")
if not isinstance(data,list): data=[]
n=sum(1 for x in data if isinstance(x,dict) and str(x.get("leader",""))=="yes")
print(n)
'
  fi
}

# 第一个 leader=yes 的 identify（应唯一）
_admin_leader_identify() {
  local body="$1"
  if command -v jq >/dev/null 2>&1; then
    echo "${body}" | jq -r '.data[] | select(.leader == "yes") | .identify' | head -n1
  else
    echo "${body}" | python3 -c '
import json,sys
j=json.load(sys.stdin)
data=j.get("data")
if not isinstance(data,list): data=[]
for x in data:
    if isinstance(x,dict) and str(x.get("leader",""))=="yes":
        sys.stdout.write(str(x.get("identify","") or ""))
        break
'
  fi
}

if [[ ! -f "${FIXTURE}" ]]; then
  echo "错误: 缺少 ${FIXTURE}" >&2
  exit 1
fi
if [[ ! -x "${CENTER_BIN}" ]]; then
  echo "错误: 无可执行 Center: ${CENTER_BIN}" >&2
  exit 1
fi
if ! _have_admin_json_parser; then
  echo "错误: 解析 admin JSON 需要 jq 或 python3，请安装其一（如: sudo apt install jq）" >&2
  exit 1
fi

echo "=== 部署 Center 插件（从 ${BUILD_LIB}）==="
mkdir -p "${DEPLOY_ROOT}/Center/plugins"
for so in CmdRaftRequestVote CmdRaftAppendEntries CmdNodeRegister CmdNodeReport CmdNodeDisconnect ModuleAdmin; do
  src="${BUILD_LIB}/${so}.so"
  if [[ -f "${src}" ]]; then
    install -m0644 "${src}" "${DEPLOY_ROOT}/Center/plugins/${so}.so"
    echo "  ${so}.so"
  else
    echo "错误: 未找到 ${src}，请先 cmake --build <build> --target CenterPlugins" >&2
    exit 1
  fi
done

echo "=== 覆盖 CenterCmd（多中心 Raft 成员列表），备份为 CenterCmd.json.bak.multitest ==="
cp -a "${DEPLOY_ROOT}/Center/conf/CenterCmd.json" "${DEPLOY_ROOT}/Center/conf/CenterCmd.json.bak.multitest" 2>/dev/null || true
cp -a "${FIXTURE}" "${DEPLOY_ROOT}/Center/conf/CenterCmd.json"
if ! grep -q '27032' "${DEPLOY_ROOT}/Center/conf/CenterCmd.json"; then
  echo "错误: 覆盖后的 CenterCmd.json 应含三中心 inner（如 27032），请检查 ${FIXTURE}" >&2
  exit 1
fi

_restore_centercmd() {
  if [[ -f "${DEPLOY_ROOT}/Center/conf/CenterCmd.json.bak.multitest" ]]; then
    mv -f "${DEPLOY_ROOT}/Center/conf/CenterCmd.json.bak.multitest" "${DEPLOY_ROOT}/Center/conf/CenterCmd.json"
    echo "=== 已恢复 Center/conf/CenterCmd.json ==="
  fi
}
trap '_restore_centercmd; _cleanup_multicenter' EXIT

echo "=== 停止可能残留的 Center_robot* ==="
pkill -f 'Center.*Center\.json' 2>/dev/null || true
pkill -f 'Center.*conf2' 2>/dev/null || true
pkill -f 'Center.*conf3' 2>/dev/null || true
sleep 1

mkdir -p "${DEPLOY_ROOT}/Center/log"
LOGF="${DEPLOY_ROOT}/Center/log/test_multicenter_raft.log"
: >"${LOGF}"

echo "=== 启动 3 个 Center（conf / conf2 / conf3）==="
(
  cd "${DEPLOY_ROOT}/Center"
  nohup "${CENTER_BIN}" conf/Center.json >>"${LOGF}" 2>&1 &
  echo $! >log/test_multicenter_c1.pid
)
MULTI_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_multicenter_c1.pid")")
(
  cd "${DEPLOY_ROOT}/Center"
  nohup "${CENTER_BIN}" conf2/Center.json >>"${LOGF}" 2>&1 &
  echo $! >log/test_multicenter_c2.pid
)
MULTI_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_multicenter_c2.pid")")
(
  cd "${DEPLOY_ROOT}/Center"
  nohup "${CENTER_BIN}" conf3/Center.json >>"${LOGF}" 2>&1 &
  echo $! >log/test_multicenter_c3.pid
)
MULTI_PIDS+=("$(cat "${DEPLOY_ROOT}/Center/log/test_multicenter_c3.pid")")

for p in 27000 27022 27032; do
  for _i in $(seq 1 40); do
    _tcp_listening "${p}" && break
    sleep 0.5
  done
  if ! _tcp_listening "${p}"; then
    echo "错误: Center inner 端口 ${p} 未监听，见 ${LOGF}" >&2
    exit 1
  fi
done

echo "=== 等待 Raft 收敛（${RAFT_SETTLE_SEC}s）==="
sleep "${RAFT_SETTLE_SEC}"

echo "=== ModuleAdmin show center（access 26000 / 26022 / 26032）— 应各恰有 1 个 leader=yes 且 identify 一致 ==="
LEADER_IDS=()
for ap in 26000 26022 26032; do
  body="$(_curl_admin "${ap}")" || true
  code="$(_admin_json_code "${body}")"
  if [[ -n "${code}" && "${code}" != "0" && "${code}" != "null" ]]; then
    echo "错误: admin(${ap}) 返回异常 code=${code} body=${body}" >&2
    exit 1
  fi
  cnt="$(_admin_leader_yes_count "${body}")"
  if [[ "${cnt}" != "1" ]]; then
    echo "错误: ${ap} 上 leader=yes 数量应为 1，实际 ${cnt}。body=${body}" >&2
    exit 1
  fi
  lid="$(_admin_leader_identify "${body}")"
  echo "  access ${ap} -> leader ${lid}"
  LEADER_IDS+=("${lid}")
done
if [[ "${LEADER_IDS[0]}" != "${LEADER_IDS[1]}" ]] || [[ "${LEADER_IDS[0]}" != "${LEADER_IDS[2]}" ]]; then
  echo "错误: 三处 admin 看到的 Leader 不一致: ${LEADER_IDS[*]}" >&2
  exit 1
fi

echo "=== Raft 视图一致，Leader=${LEADER_IDS[0]} ==="

if [[ "${SKIP_INTERFACE_SMOKE:-0}" == "1" ]]; then
  echo "SKIP_INTERFACE_SMOKE=1，跳过 Interface/GenKey 联调"
  exit 0
fi

echo "=== 调用 test_interfaceserver.sh（SKIP_CENTER_START，三 Center 已运行）==="
export SKIP_CENTER_START=1
export REQUIRED_CENTER_INNER_PORTS="27000 27022 27032"
cd "${DEPLOY_ROOT}"
bash ./tests/test_interfaceserver.sh
echo "=== test_multicenter_raft 全部通过 ==="
