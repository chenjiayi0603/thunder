#!/usr/bin/env bash
set -euo pipefail

# Dedicated launcher: run Claude Code with DeepSeek Anthropic-compatible API.
#
# 终端里 「Checking connectivity…」 含义：CLI 在启动阶段做网络自检（历史版本还会打 api.anthropic.com）。
# 本脚本已通过环境变量 +默认 --bare 尽量只走 DeepSeek。若仍卡住：
#   bash scripts/test-claude-deepseek-connectivity.sh
#   CLAUDE_DEEPSEEK_UNSET_PROXY=1 …        # 只有你明确要「不要任何代理」时再开
#
# Usage:
#   ./claude-cli/claude-deepseek.sh [--flash] [--max]

MODEL="deepseek-v4-pro"
USE_MAX=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --flash)
      MODEL="deepseek-v4-flash"
      shift
      ;;
    --max)
      USE_MAX=true
      shift
      ;;
    *)
      break
      ;;
  esac
done

# Fixed key (as requested).
KEY="sk-90512d21961f41dd94fbea786bd04cbc"

unset ANTHROPIC_AUTH_TOKEN ANTHROPIC_DEFAULT_HAIKU_MODEL
# 部分环境把 NODE_EXTRA_CA_CERTS 指到不可用证书会导致 Node  TLS 卡住（表现像一直 Checking connectivity）
unset NODE_EXTRA_CA_CERTS

export ANTHROPIC_BASE_URL="https://api.deepseek.com/anthropic"
export ANTHROPIC_API_KEY="$KEY"
export ANTHROPIC_MODEL="$MODEL"
# 默认保留当前环境的 HTTP(S)_PROXY —— 翻墙/公司代理照旧生效（之前脚本清空代理反而会导致「能上浏览器、CLI 上不了」）。
# 只有当你确信代理劫持了 TLS、需要强制直连时再设： CLAUDE_DEEPSEEK_UNSET_PROXY=1
if [[ "${CLAUDE_DEEPSEEK_UNSET_PROXY:-}" == "1" ]]; then
  unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
  export no_proxy="localhost,127.0.0.1"
  export NO_PROXY="$no_proxy"
fi

# 交互模式仍会连 api.anthropic.com 做组织/遥测等；在只用 DeepSeek 时需关掉非必要外联，否则会卡在 “Checking connectivity…”
# 参见: https://github.com/anthropics/claude-code/issues/36998
export CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
export CLAUDE_CODE_SKIP_FAST_MODE_NETWORK_ERRORS=1
# 交互启动仍会跑一轮联网自检；--bare / CLAUDE_CODE_SIMPLE 可跳过钩子、预取、大量启动期请求（仍会走 DeepSeek）
export CLAUDE_CODE_SIMPLE=1

if [[ "$USE_MAX" == "true" ]]; then
  THINKING_PART='"thinking":{"type":"enabled","budget_tokens":32000}'
else
  THINKING_PART='"thinking":true'
fi

SETTINGS_JSON="$(cat <<EOF
{"env":{"ANTHROPIC_BASE_URL":"https://api.deepseek.com/anthropic","ANTHROPIC_MODEL":"$MODEL","CLAUDE_CODE_SIMPLE":"1","CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC":"1","CLAUDE_CODE_SKIP_FAST_MODE_NETWORK_ERRORS":"1"},$THINKING_PART}
EOF
)"

declare -a bare=(--bare)
if [[ "${CLAUDE_DEEPSEEK_NO_BARE:-}" == "1" ]]; then
  bare=()
fi

exec claude "${bare[@]}" --settings "$SETTINGS_JSON" "$@"
