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
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu":"${LD_LIBRARY_PATH:-}:${SERVER_LIB}:${SERVER_3LIB}:${_THUNDER_DEPLOY}/lib:${_CODE}/3party/lib:${_CODE}/3party/lib/mariadb:${_CODE}/3party/protobuf/build"
LOG_FILE="${SERVER_HOME}/log/${SCRIPT_NAME}.log"
. "${SERVER_HOME}/scripts/script_func.sh"

BIN_NAME="HelloHttp"
JSON_NAME="Hello.json"
CONF_FILE="${THUNDER_CONF_FILE:-${SERVER_CONF}/${JSON_NAME}}"

usage() {
  echo "Usage: ./node.sh start|stop|restart|reload|worker"
}

resolve_pid() {
  local target_server target_server_tag target_port
  target_server="$(awk -F'"server_name"' '/server_name/{print $2}' "${CONF_FILE}" | sed 's/ //g' | awk -F'[:",]' '{print $3}')"
  target_server_tag="$(echo "${target_server}" | awk '{print substr($0,0,10)}')"
  target_port="$(awk -F'"inner_port"' '/inner_port/{print $2}' "${CONF_FILE}" | sed 's/ //g' | awk -F'[:",]' '{print $2}')"
  netstat -apn 2>/dev/null | grep -w "${target_port}" | grep "${target_server_tag}" | awk -F/ '/^tcp/{print $1}' | awk '/LISTEN/{print $NF}' || true
}

require_files() {
  [[ -f "${CONF_FILE}" ]] || { error_log "missing config ${CONF_FILE}"; exit 1; }
  [[ -x "${SERVER_BIN}/${BIN_NAME}" ]] || { error_log "missing executable ${SERVER_BIN}/${BIN_NAME}"; exit 1; }
}

do_start() {
  require_files
  local pid
  pid="$(resolve_pid)"
  if [[ -z "${pid}" ]]; then
    "${SERVER_BIN}/${BIN_NAME}" "${CONF_FILE}"
    info_log "${SERVER_BIN}/${BIN_NAME} start successfully."
  else
    info_log "the server process for ${BIN_NAME} already exists (pid ${pid})"
  fi
}

do_stop() {
  require_files
  local pid
  pid="$(resolve_pid)"
  if [[ -n "${pid}" ]]; then
    echo "kill ${pid}    ${BIN_NAME}"
    kill "${pid}"
  else
    echo "no running process found for ${BIN_NAME}"
  fi
}

do_signal() {
  local sig="$1"
  local msg="$2"
  require_files
  local pid
  pid="$(resolve_pid)"
  [[ -n "${pid}" ]] && echo "${msg}: ${BIN_NAME} pid:${pid}" && kill "-${sig}" "${pid}"
}

case "${1:-restart}" in
  start) do_start ;;
  stop) do_stop ;;
  restart) do_stop; do_start ;;
  reload) do_signal SIGUSR1 "reloading so files for node" ;;
  worker) do_signal SIGUSR2 "restart workers for" ;;
  -h|--help|help) usage ;;
  *) echo "wrong command: ${1:-}"; usage; exit 1 ;;
esac
