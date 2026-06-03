#!/bin/bash
# etcd 集群健康检查。用法: ./admin_status.sh [endpoint] (默认 http://127.0.0.1:2379)
ENDPOINT="${1:-http://127.0.0.1:2379}"
echo "=== etcd 健康检查 ($ENDPOINT) ==="
curl -sf "$ENDPOINT/health" && echo "  health: OK" || echo "  health: FAIL"
echo ""
echo "=== 成员列表 ==="
curl -sf -X POST "$ENDPOINT/v3/cluster/member/list" -d '{}' | python3 -m json.tool
echo ""
echo "=== 状态 ==="
curl -sf -X POST "$ENDPOINT/v3/maintenance/status" -d '{}' | python3 -m json.tool 2>/dev/null || echo "(状态查询失败)"
