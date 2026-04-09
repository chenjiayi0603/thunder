#!/bin/bash

set -euo pipefail

SERVER_HOME="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_NAME="$(basename "$0")"

SERVER_BIN="${SERVER_HOME}/bin"
SERVER_CONF="${SERVER_HOME}/conf"
SERVER_CONF2="${SERVER_HOME}/conf2"
SERVER_CONF3="${SERVER_HOME}/conf3"
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
  ./node.sh start [1|2|3|all]
  ./node.sh stop [1|2|3|all] [--yes]
  ./node.sh restart [1|2|3|all]
  ./node.sh reload [1|2|3|all]
  ./node.sh worker [1|2|3|all]
EOF
}

select_conf_dirs() {
  local selector="$1"
  case "${selector}" in
    1) echo "${SERVER_CONF}" ;;
    2) echo "${SERVER_CONF2}" ;;
    3) echo "${SERVER_CONF3}" ;;
    all | "") echo "${SERVER_CONF} ${SERVER_CONF2} ${SERVER_CONF3}" ;;
    *)
      echo "invalid selector: ${selector}" >&2
      return 1
      ;;
  esac
}

start_node_in_dir() {
  local conf_dir="$1"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name
    bin_name="$(basename "${server_bin}")"
    local conf_file="${conf_dir}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    local target_server
    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    local target_server_tag
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,20)}')"
    local target_port
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    local running_pid
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

stop_node_in_dir() {
  local conf_dir="$1"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name
    bin_name="$(basename "${server_bin}")"
    local conf_file="${conf_dir}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    local target_server
    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    local target_port
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    local target_server_tag
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    local running_pid
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"

    if [[ -n "${running_pid}" ]]; then
      echo "kill ${running_pid}    ${target_server}"
      kill "${running_pid}"
    fi
  done
}

signal_node_in_dir() {
  local conf_dir="$1"
  local signal_name="$2"
  local action_text="$3"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name
    bin_name="$(basename "${server_bin}")"
    local conf_file="${conf_dir}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue

    local target_server
    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    local target_port
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    local target_server_tag
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    local running_pid
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"

    if [[ -n "${running_pid}" ]]; then
      echo "${action_text}: ${target_server} pid:${running_pid}"
      kill "-${signal_name}" "${running_pid}"
    fi
  done
}

main() {
  local action="${1:-restart}"
  local selector="${2:-all}"
  local yes_flag="${3:-}"

  case "${action}" in
    start)
      for conf_dir in $(select_conf_dirs "${selector}"); do
        start_node_in_dir "${conf_dir}"
      done
      ;;
    stop)
      if [[ -z "${2:-}" && "${yes_flag}" != "--yes" ]]; then
        echo "do you want to stop im server process ${SERVER_HOME}? [yes|no]"
        read -r stop_im_server
        if [[ "${stop_im_server}" != "yes" ]]; then
          echo "cancel"
          exit 0
        fi
      fi
      for conf_dir in $(select_conf_dirs "${selector}"); do
        stop_node_in_dir "${conf_dir}"
      done
      ;;
    restart)
      for conf_dir in $(select_conf_dirs "${selector}"); do
        stop_node_in_dir "${conf_dir}"
      done
      for conf_dir in $(select_conf_dirs "${selector}"); do
        start_node_in_dir "${conf_dir}"
      done
      ;;
    reload)
      for conf_dir in $(select_conf_dirs "${selector}"); do
        signal_node_in_dir "${conf_dir}" "SIGUSR1" "reloading so files for node"
      done
      ;;
    worker)
      for conf_dir in $(select_conf_dirs "${selector}"); do
        signal_node_in_dir "${conf_dir}" "SIGUSR2" "restart workers for"
      done
      ;;
    -h | --help | help)
      usage
      ;;
    *)
      echo "wrong command: ${action}" >&2
      usage
      return 1
      ;;
  esac
}

main "$@"
