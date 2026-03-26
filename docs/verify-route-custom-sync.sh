#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "用法: $0 <log_file_path>"
  echo "示例: $0 /path/to/net.log"
  exit 1
fi

LOG_FILE="$1"
if [[ ! -f "$LOG_FILE" ]]; then
  echo "日志文件不存在: $LOG_FILE"
  exit 2
fi

echo "检查日志文件: $LOG_FILE"
echo

check_pattern() {
  local title="$1"
  local pattern="$2"
  local cnt
  cnt=$(rg -n "$pattern" "$LOG_FILE" | wc -l | tr -d ' ')
  echo "- $title: $cnt"
  if [[ "$cnt" -gt 0 ]]; then
    rg -n "$pattern" "$LOG_FILE" | head -n 3
  fi
  echo
}

check_pattern "路由镜像更新" "route mirror updated"
check_pattern "路由镜像未变化跳过写入" "route mirror unchanged, skip write"
check_pattern "Worker检测到路由版本变化" "NodeNoticeVersion:"
check_pattern "Worker路由解析失败(不推进ack)" "route mirror parse/read failed"

check_pattern "custom镜像更新" "custom mirror updated"
check_pattern "Worker应用custom镜像成功" "custom mirror applied"
check_pattern "Worker解析custom失败(不推进ack)" "custom mirror parse failed|custom mirror read failed"

check_pattern "配置下发重试日志" "config apply failed on .* retry="
check_pattern "配置下发重试耗尽" "config apply failed on .* retries exhausted"

echo "检查完成。请结合业务操作时间窗口确认这些日志是否按预期出现。"
