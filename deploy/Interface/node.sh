#!/bin/bash

set -euo pipefail

SERVER_HOME="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_NAME="$(basename "$0")"

SERVER_BIN="${SERVER_HOME}/bin"
SERVER_CONF="${SERVER_HOME}/conf"
SERVER_CONF_WEB="${SERVER_HOME}/confweb"
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
  ./node.sh start
  ./node.sh stop [--yes]
  ./node.sh restart
  ./node.sh reload
  ./node.sh worker
EOF
}

for_each_conf() {
  local fn="$1"
  local conf
  for conf in "${SERVER_CONF}" "${SERVER_CONF_WEB}"; do
    [[ -d "${conf}" ]] || continue
    "${fn}" "${conf}"
  done
}

start_in_conf() {
  local conf_dir="$1"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${conf_dir}/${bin_name}.json"
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

stop_in_conf() {
  local conf_dir="$1"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${conf_dir}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue
    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"
    if [[ -n "${running_pid}" ]]; then
      echo "kill ${running_pid}    ${target_server}"
      kill "${running_pid}"
    fi
  done
}

signal_in_conf() {
  local conf_dir="$1"
  local signal_name="$2"
  local action_text="$3"
  local server_bin
  for server_bin in "${SERVER_BIN}"/*; do
    [[ -f "${server_bin}" ]] || continue
    local bin_name conf_file target_server target_server_tag target_port running_pid
    bin_name="$(basename "${server_bin}")"
    conf_file="${conf_dir}/${bin_name}.json"
    [[ -f "${conf_file}" ]] || continue
    target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
    target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
    target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${conf_file}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
    running_pid="$(netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true)"
    if [[ -n "${running_pid}" ]]; then
      echo "${action_text}: ${target_server} pid:${running_pid}"
      kill "-${signal_name}" "${running_pid}"
    fi
  done
}

main() {
  local action="${1:-restart}"
  case "${action}" in
    start)
      for_each_conf start_in_conf
      ;;
    stop)
      if [[ "${2:-}" != "--yes" ]]; then
        echo "do you want to stop im server process ${SERVER_HOME}? [yes|no]"
        read -r stop_im_server
        [[ "${stop_im_server}" == "yes" ]] || { echo "cancel"; exit 0; }
      fi
      for_each_conf stop_in_conf
      ;;
    restart)
      for_each_conf stop_in_conf
      for_each_conf start_in_conf
      ;;
    reload)
      for_each_conf signal_in_conf SIGUSR1 "reloading so files for node"
      ;;
    worker)
      for_each_conf signal_in_conf SIGUSR2 "restart workers for"
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
