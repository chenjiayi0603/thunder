#!/usr/bin/env bash
# 测试 ModuleInterface：DispatchJsonTestsFromBody 中 option=TestStepCo20Binary（协程发往 LOGIC）
#
# 用法（在 deploy 目录下）:
#   ./tests/test_interfaceserver_stepco20binary.sh
#   ./tests/test_interfaceserver_stepco20binary.sh 127.0.0.1 27008 /Interface/gentoken
#
# 前置（缺一不可）:
#   - Center 监听 27000（简易注册中心）
#   - Logic 监听 16068 并已向 Center 注册
#   - Interface 监听 access_port（默认 27008），且已部署 plugins/ModuleInterface.so
# 若缺 Center/Logic，协程会一直等 LOGIC 回包，curl 可能 (28) 超时 120s 且 0 bytes，或先 (52) Empty reply（连接被回收）。
# 一键启动: cd deploy && ./start_interfaceserver.sh（或 ./tests/start_interfaceserver.sh）
#
# 若设置了 http_proxy/https_proxy，curl 可能把 127.0.0.1 走代理导致 (52) Empty reply / 超时；本脚本已加 --noproxy '*'。
#
# Worker 对无 Keep-Alive 的 HTTP 默认 dKeepAlive≈10s：协程若长时间等待 LOGIC，连接会先被回收，
# 再 ResponseToClient 会报 send to tagMsgShell error，随后 curl (52)。本脚本已加 Keep-Alive: 120 与 -m 120。

set -euo pipefail

# deploy 根目录（本脚本位于 deploy/tests/）
DEPLOY_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${1:-127.0.0.1}"
PORT="${2:-27008}"
PATH_URL="${3:-/Interface/gentoken}"
URL="http://${HOST}:${PORT}${PATH_URL}"

_curl() {
  if [[ -x /usr/bin/curl ]]; then
    env -u LD_LIBRARY_PATH /usr/bin/curl --noproxy '*' "$@"
  else
    env -u LD_LIBRARY_PATH curl --noproxy '*' "$@"
  fi
}

_hint_stack() {
  echo "排查: 本请求依赖 Center(27000) + Logic(16068) + Interface。请确认:" >&2
  echo "  ss -tlnp | grep -E '27000|16068|27008'  或  pgrep -af 'Center_robot|Logic_robot|Interface_robot'" >&2
  echo "一键拉起: cd ${DEPLOY_ROOT} && ./start_interfaceserver.sh" >&2
}

# 可选：本机端口粗检（仅提示，不阻断）
if command -v ss >/dev/null 2>&1; then
  if ! ss -tln 2>/dev/null | grep -q ':27000 '; then
    echo "警告: 未检测到本机 LISTEN :27000（Center）。TestStepCo20Binary 若无 Center/Logic 将长时间无 HTTP 回包 → curl (28) 120s / 0 bytes。" >&2
    _hint_stack
  elif ! ss -tln 2>/dev/null | grep -q ':16068 '; then
    echo "警告: 未检测到本机 LISTEN :16068（Logic）。Interface 无法把请求转到 LOGIC，协程可能挂起到超时。" >&2
    _hint_stack
  fi
fi

echo "POST ${URL}"
echo 'Body: {"option":"TestStepCo20Binary","note":"test_interfaceserver_stepco20binary.sh"}'
echo "---"
_body=$(
  _curl -sS -f -m 120 -X POST "${URL}" \
    -H 'Content-Type: application/json' \
    -H 'Connection: keep-alive' \
    -H 'Keep-Alive: 120' \
    -d '{"option":"TestStepCo20Binary","note":"test_interfaceserver_stepco20binary.sh"}' \
    -w '\n[HTTP %{http_code}]\n'
) || {
    echo "请求失败。" >&2
    echo "  (28) 超时 / 0 bytes: 多为 Center 或 Logic 未起、未注册，协程一直等 LOGIC，HTTP 无响应。" >&2
    echo "  (52) Empty reply: 常为连接被回收、或未正确回 HTTP（见 Interface 日志）。" >&2
    echo "  其它: 404/路径与 conf 中 module.url_path 不一致；http_proxy 需 --noproxy（本脚本已加）。" >&2
    _hint_stack
    exit 1
  }
echo "${_body}"
