#!/bin/bash

set -euo pipefail

SERVER_HOME="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_NAME="$(basename "$0")"

SERVER_BIN="${SERVER_HOME}/bin"
SERVER_CONF="${THUNDER_CONF_DIR:-${SERVER_HOME}/conf}"
SERVER_LIB="${SERVER_HOME}/lib"
SERVER_3LIB="${SERVER_HOME}/../3lib"
_THUNDER_DEPLOY="$(cd "${SERVER_HOME}/.." && pwd)"
_CODE="$(cd "${_THUNDER_DEPLOY}/../code" && pwd)"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:${SERVER_LIB}:${SERVER_3LIB}:${_THUNDER_DEPLOY}/lib:${_CODE}/3party/lib:${_CODE}/3party/lib/mariadb:${_CODE}/3party/protobuf/build"

LOG_FILE="${SERVER_HOME}/log/${SCRIPT_NAME}.log"
. "${SERVER_HOME}/scripts/script_func.sh"

usage() {
  cat <<'EOF'
Usage:
  ./node.sh start [all]
  ./node.sh stop [all] [--yes]
  ./node.sh restart [all]
  ./node.sh reload [all]
  ./node.sh worker [all]
EOF
}

start_all() {
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${SERVER_CONF}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,20)}')"
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"

    if [[ -z "${running_pid}" ]]; then
      "${server_bin}" "${conf_file}"
      if [[ $? -eq 0 ]]; then
        info_log "${SERVER_HOME}/bin/${bin_name} start successfully."
      else
        error_log "failed to start ${bin_name}"
      fi
      ps -ef | awk -vpname="${target_server}" '{idx=index($8,pname); if (idx == 1)print}'
    else
      info_log "the server process for ${bin_name} already exists!"
    fi
  done
}

stop_all() {
  local server_bin
  local killed_pids=()
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${SERVER_CONF}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"

    if [[ -n "${running_pid}" ]]; then
      echo "kill ${running_pid}    ${target_server}"
      kill "${running_pid}"
      killed_pids+=("${running_pid}")
    fi
  done
  local i pid
  for i in $(seq 1 25); do
    local all_gone=true
    for pid in "${killed_pids[@]+"${killed_pids[@]}"}"; do
      kill -0 "${pid}" 2>/dev/null && { all_gone=false; break; }
    done
    ${all_gone} && return 0
    sleep 1
  done
}

signal_all() {
  local sig="$1"
  local msg="$2"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${SERVER_CONF}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"

    if [[ -n "${running_pid}" ]]; then
      echo "${msg}: ${target_server} pid:${running_pid}"
      kill "-${sig}" "${running_pid}"
    fi
  done
}

main() {
  local action="${1:-restart}"
  case "${action}" in
    start)
      start_all
      ;;
    stop)
      if [[ "${3:-}" != "--yes" && "${2:-}" != "--yes" ]]; then
        echo "do you want to stop im server process ${SERVER_HOME}? [yes|no]"
        read -r stop_im_server
        [[ "${stop_im_server}" == "yes" ]] || { echo "cancel"; exit 0; }
      fi
      stop_all
      ;;
    restart)
      stop_all
      start_all
      ;;
    reload)
      signal_all SIGUSR1 "reloading so files for node"
      ;;
    worker)
      signal_all SIGUSR2 "restart workers for"
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      echo "wrong command: ${action}" >&2
      usage
      exit 1
      ;;
  esac
}

main "$@"
