#!/usr/bin/env bash
#
# 统一部署脚本：启停、重启、清理
# 用法（在 deploy 目录下）:
#   ./nodes.sh start all | <节点名>     # 节点名：Interface、Logic、Hello、Center（与 SERVER_LIST 一致）；例：./nodes.sh start Interface
#   ./nodes.sh stop all | force | <节点名>   # 例：./nodes.sh stop Logic
#   ./nodes.sh restart all | reload | force | <节点名>   # 例：./nodes.sh restart Hello
#   ./nodes.sh restartforce all | <节点名>   # 强制重启：先 pkill 该节点进程再 start（stop 杀不掉时用）
#   ./nodes.sh clean all | plugins | log | bin | core
#
# 修改节点列表或清理路径：编辑本脚本「配置」段。
#
# restart 时 stop 与 start 之间默认等待 2 秒（旧进程释放端口）；可覆盖：RESTART_WAIT_SEC=5 ./nodes.sh restart all
#
set -u

SERVER_HOME="$(cd "$(dirname "$0")" && pwd)"
cd "${SERVER_HOME}"

# ========== 配置（原 server_list.conf / server_dir.conf）==========
# 启停顺序（与旧 server_list.conf 一致）
SERVER_LIST=(Interface Logic Hello Center)

# 原 server_dir.conf：每行 nodetype plugin_path 第三列及以后为保留字段
# 供 clean（plugins/log/bin）使用
_server_dir_lines() {
  cat <<'EOF'
Interface 	   	   /Interface/plugins/          			/plugins/Interface
Hello 	   /Hello/plugins/          	    /plugins/Hello
Center               /Center/plugins/          			/plugins/Center
Logic               /Logic/plugins/          			/plugins/Logic
EOF
}
# ========== end 配置 ==========

_chmod_scripts() {
  find ./ -maxdepth 3 -type f -name "*.sh" 2>/dev/null | while read -r f; do
    chmod +x "$f"
  done
}

_list_servers() {
  local s
  for s in "${SERVER_LIST[@]}"; do
    echo "${s}"
  done
}

# 重启时 stop 后等待再 start，避免旧进程未退出导致 start 误判「已存在」而不拉起新进程
_stop_then_start() {
  local node="$1"
  echo "=== restart ${node}: stop → wait → start ===" >&2
  "${SERVER_HOME}/${node}/stop.sh" "yes"
  sleep "${RESTART_WAIT_SEC:-2}"
  "${SERVER_HOME}/${node}/start.sh"
}

# 强制杀某节点相关进程（不依赖 netstat；用于 stop.sh 无法匹配 PID 时）
_force_kill_node() {
  local node="$1"
  local bin_dir="${SERVER_HOME}/${node}/bin"
  local exe
  shopt -s nullglob
  for exe in "${bin_dir}"/*; do
    [[ -f "$exe" && -x "$exe" ]] || continue
    pkill -9 -f "$exe" 2>/dev/null || true
  done
  shopt -u nullglob
  case "$node" in
    Hello) pkill -9 -f Hello_robot 2>/dev/null || true ;;
    Center) pkill -9 -f Center_robot 2>/dev/null || true ;;
    Logic) pkill -9 -f Logic_robot 2>/dev/null || true ;;
    Interface) pkill -9 -f Interface_robot 2>/dev/null || true ;;
  esac
}

_ps_robots() {
  # 避免 grep 自身出现在结果里（[H] 技巧）
  ps -eo pid,lstart,etime,cmd 2>/dev/null | grep -E '[H]ello_robot|[C]enter_robot|[L]ogic_robot|[I]nterface_robot' || true
}

_usage() {
  cat <<EOF
Usage: $(basename "$0") <command> [args]

Commands:
  start      all | <node>              启动全部或单节点
  stop       all | force | <node>      停止全部；force 强杀 _im；或单节点
  restart    all | reload | force | <node>
  restartforce all | <node>            强制重启（pkill 后再 start）
  clean      all | plugins | log | bin | core

Nodes: $(_list_servers | tr '\n' ' ')

Examples:
  $(basename "$0") start all
  $(basename "$0") restart Hello
  $(basename "$0") restartforce all
  $(basename "$0") clean log
EOF
}

CleanPlugin() {
  while read -r nodetype plugin _rest; do
    [[ -z "${nodetype}" ]] && continue
    [[ "${nodetype}" =~ ^# ]] && continue
    find "${SERVER_HOME}${plugin}" -type f -name "*.so" 2>/dev/null | xargs -r rm -f
  done < <(_server_dir_lines)
  echo "succ to clean plugin"
}

CleanLog() {
  while read -r nodetype _a _b; do
    [[ -z "${nodetype}" ]] && continue
    [[ "${nodetype}" =~ ^# ]] && continue
    find "${SERVER_HOME}/${nodetype}/log" -type f -name '*.log*' 2>/dev/null | xargs -r rm -f
  done < <(_server_dir_lines)
  echo "succ to clear log"
}

CleanBin() {
  while read -r nodetype _a _b; do
    [[ -z "${nodetype}" ]] && continue
    [[ "${nodetype}" =~ ^# ]] && continue
    find "${SERVER_HOME}/${nodetype}/bin" -type f 2>/dev/null | xargs -r rm -f
  done < <(_server_dir_lines)
  echo "succ to clear bin"
}

CleanCore() {
  find ./ -maxdepth 3 -type f -name "core*" 2>/dev/null | xargs -r rm -f
}

_clean_usage() {
  echo "configure server list / paths at top of $(basename "$0") before running"
  echo "Usage: $(basename "$0") clean [all|plugins|log|bin|core]"
}

_cmd_start() {
  _chmod_scripts
  local arg="${1:-}"
  if [[ -z "$arg" ]]; then
    echo "USAGE: $(basename "$0") start all|<nodename>:" >&2
    _list_servers
    exit 1
  fi
  if [[ "$arg" == "all" ]]; then
    local s
    for s in "${SERVER_LIST[@]}"; do
      "${SERVER_HOME}/${s}/start.sh"
    done
    return 0
  fi
  local s
  for s in "${SERVER_LIST[@]}"; do
    if [[ "$arg" == "$s" ]]; then
      "${SERVER_HOME}/${s}/start.sh"
      echo "start ${s} ok"
      exit 0
    fi
  done
  echo "USAGE: $(basename "$0") start all|<nodename>:" >&2
  _list_servers
  exit 1
}

_cmd_stop() {
  _chmod_scripts
  local arg="${1:-}"
  if [[ -z "$arg" ]]; then
    echo "USAGE: $(basename "$0") stop all/force/nodetype" >&2
    exit 1
  fi
  if [[ "$arg" == "all" ]]; then
    local s
    for s in "${SERVER_LIST[@]}"; do
      "${SERVER_HOME}/${s}/stop.sh" "yes"
    done
    return 0
  fi
  if [[ "$arg" == "force" ]]; then
    local server_name=_im
    # shellcheck disable=SC2009
    ps -ef | grep "$server_name" | grep -v grep | awk '{print $2}' | xargs -r kill -9
    ps -eo pid,lstart,etime,cmd | grep "$server_name" || true
    return 0
  fi
  local s
  for s in "${SERVER_LIST[@]}"; do
    if [[ "$arg" == "$s" ]]; then
      "${SERVER_HOME}/${s}/stop.sh" "yes"
      echo "stop ${s} ok"
      exit 0
    fi
  done
  echo "USAGE: $(basename "$0") stop all/force/nodetype" >&2
  _list_servers
  exit 1
}

_cmd_restart() {
  _chmod_scripts
  local arg="${1:-}"
  if [[ -z "$arg" ]]; then
    echo "USAGE: $(basename "$0") restart param1(servername)" >&2
    echo "All" >&2
    _list_servers
    exit 1
  fi
  if [[ "$arg" == "reload" ]]; then
    local s
    for s in "${SERVER_LIST[@]}"; do
      "${SERVER_HOME}/${s}/restart.sh" "reload"
    done
    return 0
  fi
  if [[ "$arg" == "force" ]]; then
    bash "${BASH_SOURCE[0]}" stop force
    local s
    for s in "${SERVER_LIST[@]}"; do
      "${SERVER_HOME}/${s}/stop.sh" "yes"
      sleep "${RESTART_WAIT_SEC:-2}"
      "${SERVER_HOME}/${s}/start.sh"
    done
    sleep 1
    echo "server list:"
    _ps_robots
    return 0
  fi
  if [[ "$arg" == "all" ]]; then
    local s
    for s in "${SERVER_LIST[@]}"; do
      _stop_then_start "${s}"
    done
    sleep 1
    echo "server list:"
    _ps_robots
    return 0
  fi
  local s
  for s in "${SERVER_LIST[@]}"; do
    if [[ "$arg" == "$s" ]]; then
      _stop_then_start "${s}"
      echo "restart ${s} ok"
      exit 0
    fi
  done
  echo "USAGE: $(basename "$0") restart param1(servername)" >&2
  echo "All" >&2
  _list_servers
  exit 1
}

_cmd_restartforce() {
  _chmod_scripts
  local arg="${1:-}"
  if [[ -z "$arg" ]]; then
    echo "USAGE: $(basename "$0") restartforce all | <node>" >&2
    _list_servers
    exit 1
  fi
  if [[ "$arg" == "all" ]]; then
    local s
    for s in "${SERVER_LIST[@]}"; do
      echo "=== restartforce ${s}: pkill → wait → start ===" >&2
      _force_kill_node "${s}"
      sleep "${RESTART_WAIT_SEC:-2}"
      "${SERVER_HOME}/${s}/start.sh"
    done
    sleep 1
    echo "server list:"
    _ps_robots
    return 0
  fi
  local s
  for s in "${SERVER_LIST[@]}"; do
    if [[ "$arg" == "$s" ]]; then
      echo "=== restartforce ${s}: pkill → wait → start ===" >&2
      _force_kill_node "${s}"
      sleep "${RESTART_WAIT_SEC:-2}"
      "${SERVER_HOME}/${s}/start.sh"
      echo "restartforce ${s} ok"
      exit 0
    fi
  done
  echo "USAGE: $(basename "$0") restartforce all | <node>" >&2
  _list_servers
  exit 1
}

_cmd_clean() {
  local opt="${1:-}"
  case "${opt}" in
    -h | --help)
      _clean_usage
      ;;
    all)
      CleanLog
      CleanPlugin
      CleanBin
      CleanCore
      ;;
    plugins)
      CleanPlugin
      ;;
    log)
      CleanLog
      ;;
    bin)
      CleanBin
      ;;
    core)
      CleanCore
      ;;
    *)
      echo "nothings to done"
      _clean_usage
      exit 1
      ;;
  esac
}

main() {
  local cmd="${1:-}"
  if [[ -z "$cmd" ]]; then
    _usage
    exit 1
  fi
  # 常见拼写纠错（restart）
  case "$cmd" in
    resatart | restat | restrat) cmd=restart ;;
    restartfore | restartforece | restartforcee) cmd=restartforce ;;
  esac
  shift || true
  case "$cmd" in
    -h | --help | help)
      _usage
      ;;
    start)
      _cmd_start "$@"
      ;;
    stop)
      _cmd_stop "$@"
      ;;
    restart)
      _cmd_restart "$@"
      ;;
    restartforce)
      _cmd_restartforce "$@"
      ;;
    clean)
      _cmd_clean "$@"
      ;;
    *)
      echo "Unknown command: $cmd" >&2
      echo "提示: 子命令为 start | stop | restart | restartforce | clean（例如: $(basename "$0") restart all）" >&2
      _usage
      exit 1
      ;;
  esac
}

main "$@"
