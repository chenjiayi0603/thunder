#!/bin/bash
# ============================================================
# Thunder 节点注册 + 下发 全面测试
#
# 用法:
#   ./tests/e2e/test_registration_deploy.sh
#
# 测试项:
#   1. etcd 节点注册完整性 (所有 node_type 已注册)
#   2. SO 制品上传+下发+验证
#   3. Lua 制品上传+下发+验证
#   4. 配置下发验证
# ============================================================
set -uo pipefail  # no -e: handle failures explicitly in pass/fail

PASS=0; FAIL=0
GREEN='\033[32m'; RED='\033[31m'; YELLOW='\033[33m'; NC='\033[0m'

pass() { echo -e "  ${GREEN}✅ PASS${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}❌ FAIL${NC} $1 — $2"; ((FAIL++)); }

ETCD_POD=$(kubectl get pods -n thunder -l app=thunder-etcd -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)
ADMIN_URL="${E2E_ADMIN_HOST:-http://192.168.3.61:30090}"
TOKEN="REG_TEST_$(date +%s)"

echo "========================================"
echo " Thunder 注册+下发全面测试"
echo " etcd pod : $ETCD_POD"
echo " admin    : $ADMIN_URL"
echo " token    : $TOKEN"
echo "========================================"

# ── helper ──
etcd_keys() { kubectl exec -n thunder "$ETCD_POD" -- etcdctl get "$1" --prefix --keys-only 2>/dev/null || true; }
http_get()  { curl -s "${ADMIN_URL}$1" 2>/dev/null || true; }
http_post() { curl -s -X POST "${ADMIN_URL}$1" -H 'Content-Type: application/json' -d "$2" 2>/dev/null || true; }
http_put()  { curl -s -X PUT "${ADMIN_URL}$1" --data-binary "@$2" 2>/dev/null || true; }

# ============================================================
# 1. 节点注册完整性
# ============================================================
echo ""
echo "── 1. 节点注册完整性 ──"

EXPECTED_TYPES=("HELLO_HTTP" "HELLO_HTTPS" "HELLO_WS" "HELLO_WSS" "INTERFACE" "LOGIC")

for t in "${EXPECTED_TYPES[@]}"; do
    count=$(etcd_keys "/thunder/registry/$t/" | wc -l)
    if [ "$count" -ge 1 ]; then
        pass "etcd registry $t: $count node(s)"
    else
        # 检查 Pod 是否存在
        pod_count=$(kubectl get pods -n thunder -l "app=thunder-${t,,}" --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l)
        # 用 node_type 更准确的匹配
        if [ "$pod_count" -eq 0 ]; then
            # HELLO_HTTP → hello, HELLO_HTTPS → hello-https
            case $t in
                HELLO_HTTP)  label="app=thunder-hello,app!=thunder-hello-https,app!=thunder-hello-ws,app!=thunder-hello-wss" ;;
                HELLO_HTTPS) label="app=thunder-hello-https" ;;
                HELLO_WS)    label="app=thunder-hello-ws" ;;
                HELLO_WSS)   label="app=thunder-hello-wss" ;;
                INTERFACE)   label="app=thunder-interface" ;;
                LOGIC)       label="app=thunder-logic" ;;
            esac
            pod_count=$(kubectl get pods -n thunder -l "$label" --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l)
        fi
        if [ "$pod_count" -ge 1 ]; then
            fail "etcd registry $t" "Pod Running 但未注册到 etcd (lease 过期?)"
        else
            echo -e "  ${YELLOW}⚠ SKIP${NC} $t: 无 Running Pod"
        fi
    fi
done

# 验证 overview API
echo ""
overview=$(http_get "/api/overview")
if echo "$overview" | python3 -c "import json,sys; d=json.load(sys.stdin); assert d['ok'] and d['data']['total_nodes']>=5" 2>/dev/null; then
    echo "$overview" | python3 -c "
import json,sys;d=json.load(sys.stdin)
for s in d['data']['services']:
    print(f'    {s[\"node_type\"]:<14} count={s[\"count\"]} online={s[\"online\"]}')"
    pass "overview API"
else
    fail "overview API" "节点数不足"
fi

# ============================================================
# 2. SO 制品上传+下发+验证
# ============================================================
echo ""
echo "── 2. SO 制品上传+下发+验证 ──"

SO_NAME="_reg_test_${TOKEN}.so"
SO_TYPE="HelloHttp"
# 2a. 上传
TMP_SO=$(mktemp)
python3 -c "
import sys
token=b'${TOKEN}'
sys.stdout.buffer.write(bytes([0x7f,0x45,0x4c,0x46])+b'\x00'*60+token)" > "$TMP_SO"
resp=$(http_put "/api/plugins/${SO_TYPE}/${SO_NAME}" "$TMP_SO")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok')" 2>/dev/null; then
    pass "SO upload: $SO_NAME"
else
    fail "SO upload" "$(echo "$resp" | head -1)"
fi

# 2b. 下发
resp=$(http_post "/api/plugins/${SO_TYPE}/deploy" "{\"filename\":\"${SO_NAME}\"}")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok') and d['data']['deployed']" 2>/dev/null; then
    echo "$resp" | python3 -c "
import json,sys;d=json.load(sys.stdin)['data']
print(f'    pods: {d[\"succeeded\"]}/{d[\"total_pods\"]} etcd_bumped={d[\"etcd_bumped\"]}')"
    pass "SO deploy: ${SO_NAME}"
else
    fail "SO deploy" "$(echo "$resp" | head -1)"
fi

# 2c. 验证已部署列表
resp=$(http_get "/api/plugins/${SO_TYPE}/deployed")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin)
files=[f['filename'] for f in d['data']['files']]
assert '${SO_NAME}' in files" 2>/dev/null; then
    pass "SO deployed-list: ${SO_NAME} found"
else
    fail "SO deployed-list" "${SO_NAME} not found"
fi

# 2d. 验证制品库
resp=$(http_get "/api/plugins/${SO_TYPE}")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin)
files=[f['filename'] for f in d['data']['files']]
assert '${SO_NAME}' in files" 2>/dev/null; then
    pass "SO artifact-list: ${SO_NAME} found"
else
    fail "SO artifact-list" "${SO_NAME} not found"
fi

rm -f "$TMP_SO"

# ============================================================
# 3. Lua 制品上传+下发+验证
# ============================================================
echo ""
echo "── 3. Lua 制品上传+下发+验证 ──"

LUA_NAME="_reg_test_${TOKEN}.lua"
LUA_TYPE="HELLO_HTTP"
LUA_URL="/hello/_reg_test_${TOKEN}"
LUA_CONTENT="function handle_request(msg)\n  SendToClientFast('{\"code\":0,\"msg\":\"${TOKEN}\"}')\n  return true\nend"

TMP_LUA=$(mktemp)
echo -e "$LUA_CONTENT" > "$TMP_LUA"

# 3a. 上传
resp=$(http_put "/api/lua/${LUA_TYPE}/${LUA_NAME}" "$TMP_LUA")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok')" 2>/dev/null; then
    pass "Lua upload: $LUA_NAME"
else
    fail "Lua upload" "$(echo "$resp" | head -1)"
fi

# 3b. 下发
resp=$(http_post "/api/lua/${LUA_TYPE}/deploy" "{\"filename\":\"${LUA_NAME}\",\"url_path\":\"${LUA_URL}\"}")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok')" 2>/dev/null; then
    echo "$resp" | python3 -c "
import json,sys;d=json.load(sys.stdin)['data']
print(f'    url_path: {d[\"url_path\"]} size: {d[\"size\"]}')"
    pass "Lua deploy: ${LUA_NAME}"
else
    fail "Lua deploy" "$(echo "$resp" | head -1)"
fi

# 3c. 验证已部署
resp=$(http_get "/api/lua/${LUA_TYPE}")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin)
urls=[s['url_path'] for s in d['data']['scripts']]
assert '${LUA_URL}' in urls" 2>/dev/null; then
    resp2=$(http_get "/api/lua/${LUA_TYPE}")
    echo "$resp2" | python3 -c "
import json,sys
for s in json.load(sys.stdin)['data']['scripts']:
    if s['url_path']=='${LUA_URL}':
        print(f'    version: v{s[\"version\"]} content_ok: {\"${TOKEN}\" in s[\"script_content\"]}')"
    pass "Lua deployed: ${LUA_URL} found"
else
    fail "Lua deployed" "${LUA_URL} not found"
fi

# 3d. 验证制品库
resp=$(http_get "/api/lua/${LUA_TYPE}/files")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin)
files=[f['filename'] for f in d['data']['files']]
assert '${LUA_NAME}' in files" 2>/dev/null; then
    pass "Lua artifact: ${LUA_NAME} found"
else
    fail "Lua artifact" "${LUA_NAME} not found"
fi

rm -f "$TMP_LUA"

# ============================================================
# 4. 配置下发验证
# ============================================================
echo ""
echo "── 4. 配置下发验证 ──"

# 读 HELLO_HTTP 配置
resp=$(http_get "/api/config/HELLO_HTTP?type=Logic.json")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok')" 2>/dev/null; then
    pass "Config read HELLO_HTTP"
else
    fail "Config read HELLO_HTTP" "API error"
fi

# 读 LOGIC 配置
resp=$(http_get "/api/config/LOGIC?type=Logic.json")
if echo "$resp" | python3 -c "import json,sys;d=json.load(sys.stdin);assert d.get('ok')" 2>/dev/null; then
    pass "Config read LOGIC"
else
    fail "Config read LOGIC" "API error"
fi

# ============================================================
# 5. 清理测试数据
# ============================================================
CLEANUP="${1:-}"  # 传 --cleanup 才清理
if [ "$CLEANUP" = "--cleanup" ]; then
    echo ""
    echo "── 5. 清理测试数据 ──"

    # 清理 etcd 中测试 Lua 条目
    http_get "/api/config/HELLO_HTTP?type=Logic.json" > /tmp/_reg_clean_cfg.json
    python3 -c "
import json
with open('/tmp/_reg_clean_cfg.json') as f: d=json.load(f)
cfg=d['data']['content']; mods=cfg.get('module',[])
keep=[m for m in mods if not (
    (m.get('script_content','') and m.get('url_path','').split('/')[-1].startswith('_reg_test_'))) ]
cfg['module']=keep
import urllib.request as u
body=json.dumps({'type':'Logic.json','content':cfg}).encode()
u.urlopen(u.Request('${ADMIN_URL}/api/config/HELLO_HTTP',data=body,method='PUT',
    headers={'Content-Type':'application/json'}),timeout=10)
print(f'  cleaned etcd: {len(keep)} kept')
" 2>/dev/null

    # 清理 PVC 测试文件
    kubectl exec -n thunder deploy/thunder-admin-web -- sh -c "
      rm -f /app/data/lua_scripts/HELLO_HTTP/_reg_test_*.lua 2>/dev/null
      rm -f /app/data/artifacts/HelloHttp/_reg_test_*.so 2>/dev/null
    " 2>/dev/null
    echo "  cleaned PVC test files"
fi

# ============================================================
# 结果汇总
# ============================================================
echo ""
echo "========================================"
echo -e "  结果: ${GREEN}${PASS} PASS${NC}  ${RED}${FAIL} FAIL${NC}"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
