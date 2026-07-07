#!/bin/sh
# 已迁移到 deploy/scripts/admin.py，本脚本保留兼容转发
echo "→ 请使用: deploy.sh admin $(basename $0 | sed 's/admin_//;s/\.py//;s/\.sh//')" >&2
exec python3 "$(dirname "$0")/admin.py" "$(basename "$0" | sed 's/admin_//;s/\.py//;s/\.sh//')" "$@"
