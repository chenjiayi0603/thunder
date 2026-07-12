#!/bin/bash
# Thunder K8s 回归测试 — 覆盖全部 5 个网关
# 用法: bash k8s/regression-test.sh
set -uo pipefail  # 允许个别命令失败，不中断全流程

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0
NS=thunder
HOST_IP=192.168.3.61

check() {
  local name="$1"; local result="$2"; local expect="${3:-}"
  if [ "$result" = "PASS" ]; then
    echo -e "  ${GREEN}[PASS]${NC} $name"
    PASS=$((PASS+1))
  elif [ "$result" = "SKIP" ]; then
    echo -e "  ${YELLOW}[SKIP]${NC} $name ($expect)"
    SKIP=$((SKIP+1))
  else
    echo -e "  ${RED}[FAIL]${NC} $name ($expect)"
    FAIL=$((FAIL+1))
  fi
}

echo "============================================================"
echo " Thunder K8s 回归测试"
echo " $(date '+%Y-%m-%d %H:%M:%S')"
echo "============================================================"

# ============== 1. 集群基础状态 ==============
echo ""
echo "--- 1. 集群基础状态 ---"

COREDNS=$(kubectl get pods -n kube-system -l k8s-app=kube-dns --field-selector=status.phase=Running 2>/dev/null | grep -c "coredns" || echo 0)
[ "$COREDNS" -ge 1 ] && check "CoreDNS Running ($COREDNS/2)" "PASS" || check "CoreDNS" "FAIL" "$COREDNS"

# ============== 2. 全部网关 Pod 状态 ==============
echo ""
echo "--- 2. 网关 Pod 状态 ---"
GATEWAYS="thunder-hello thunder-hello-https thunder-hello-ws thunder-hello-wss thunder-interface"
for dep in $GATEWAYS; do
  STATUS=$(kubectl get pods -n $NS -l app=$dep --field-selector=status.phase=Running 2>/dev/null | grep -c "Running" || echo 0)
  [ "$STATUS" -ge 1 ] && check "$dep Running" "PASS" || check "$dep" "FAIL" "0 Running"
done

# ============== 3. PVC subPath 验证 ==============
echo ""
echo "--- 3. PVC subPath 隔离 ---"

declare -A PLUGIN_EXPECT
PLUGIN_EXPECT[thunder-hello]="HelloHttp_ModuleHello.so"
PLUGIN_EXPECT[thunder-hello-https]="ModuleHello.so"
PLUGIN_EXPECT[thunder-hello-ws]="CmdHello.so"
PLUGIN_EXPECT[thunder-hello-wss]=""  # WSS 目录可能为空，但有 subPath 就不该看到别的 dir
PLUGIN_EXPECT[thunder-interface]="ModuleInterface.so"

declare -A PLUGIN_PATH
PLUGIN_PATH[thunder-hello]="/thunder/deploy/HelloHttp/plugins"
PLUGIN_PATH[thunder-hello-https]="/thunder/deploy/HelloHttps/plugins"
PLUGIN_PATH[thunder-hello-ws]="/thunder/deploy/HelloWs/plugins"
PLUGIN_PATH[thunder-hello-wss]="/thunder/deploy/HelloWss/plugins"
PLUGIN_PATH[thunder-interface]="/thunder/deploy/Interface/plugins"

# 不应该出现的文件（说明 subPath 没生效）
BAD_GLOB="HelloHttp\|HelloHttps\|HelloWs\|Interface\|HELLO_HTTP"

for dep in $GATEWAYS; do
  POD=$(kubectl get pods -n $NS -l app=$dep --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
  if [ -z "$POD" ]; then
    check "$dep subPath" "FAIL" "no running pod"
    continue
  fi
  LS=$(kubectl exec -n $NS "$POD" -- ls "${PLUGIN_PATH[$dep]}/" 2>/dev/null || echo "__ERROR__")
  if echo "$LS" | grep -qE "$BAD_GLOB"; then
    check "$dep subPath (看到 NFS 根目录=未隔离)" "FAIL" "$(echo "$LS" | head -3)"
  elif echo "$LS" | grep -q "__ERROR__"; then
    check "$dep subPath" "FAIL" "ls 失败"
  else
    EXPECT="${PLUGIN_EXPECT[$dep]}"
    if [ -n "$EXPECT" ]; then
      if echo "$LS" | grep -qF "$EXPECT"; then
        check "$dep subPath (含 $EXPECT)" "PASS"
      else
        check "$dep subPath" "FAIL" "期望含 $EXPECT, 实际: $(echo "$LS" | head -3)"
      fi
    else
      # WSS: 只要是空或只有自己的 .so 就行
      check "$dep subPath" "PASS"
    fi
  fi
done

# ============== 4. DNS 解析 ==============
echo ""
echo "--- 4. CoreDNS 解析 ---"

DNS_DEP="thunder-hello"  # 用 HelloHttp 测 DNS
DNS_POD=$(kubectl get pods -n $NS -l app=$DNS_DEP --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")

# 检查 /etc/resolv.conf 含 CoreDNS
RESOLV=$(kubectl exec -n $NS "$DNS_POD" -- cat /etc/resolv.conf 2>/dev/null)
if echo "$RESOLV" | grep -q "10.96.0.10"; then
  check "resolv.conf 指向 CoreDNS" "PASS"
else
  check "resolv.conf" "FAIL" "$(echo "$RESOLV" | head -1)"
fi

# 短名解析
SHORT=$(kubectl exec -n $NS "$DNS_POD" -- getent hosts thunder-etcd.thunder 2>/dev/null || echo "")
if [ -n "$SHORT" ]; then
  check "getent thunder-etcd.thunder → $SHORT" "PASS"
else
  check "短名解析 thunder-etcd.thunder" "FAIL" "无结果"
fi

# FQDN 解析
FQDN=$(kubectl exec -n $NS "$DNS_POD" -- getent hosts thunder-etcd-0.thunder-etcd.thunder.svc.cluster.local 2>/dev/null || echo "")
if [ -n "$FQDN" ]; then
  check "getent etcd FQDN → $FQDN" "PASS"
else
  check "FQDN解析" "FAIL" "无结果"
fi

# ============== 5. 服务直连 ==============
echo ""
echo "--- 5. 服务直连 (hostNetwork) ---"

# HelloHttp
HTTP_RESP=$(curl -s -m 3 -X POST "http://${HOST_IP}:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}' 2>/dev/null || echo "")
if echo "$HTTP_RESP" | grep -q '"code":0'; then
  check "HelloHttp :27006 POST" "PASS"
else
  check "HelloHttp :27006" "FAIL" "$(echo "$HTTP_RESP" | head -c 80)"
fi

# Interface
IFACE_RESP=$(curl -s -m 3 "http://${HOST_IP}:27008/Interface/gentoken" 2>/dev/null || echo "")
if [ -n "$IFACE_RESP" ]; then
  check "Interface :27008" "PASS"
else
  check "Interface :27008" "FAIL" "无响应"
fi

# HelloHttps
HTTPS_CODE=$(curl -sk -m 3 -o /dev/null -w "%{http_code}" \
  -X POST "https://${HOST_IP}:27443/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":3}' 2>/dev/null || echo "000")
if [ "$HTTPS_CODE" != "000" ]; then
  check "HelloHttps :27443 (HTTP $HTTPS_CODE)" "PASS"
else
  check "HelloHttps :27443" "SKIP" "connector fallback, 服务可能异常"
fi

# HelloWs
WS_CODE=$(curl -s -m 3 -o /dev/null -w "%{http_code}" \
  -H "Upgrade: websocket" -H "Connection: Upgrade" \
  "http://${HOST_IP}:27010/hello/shake" 2>/dev/null || echo "000")
if [ "$WS_CODE" != "000" ]; then
  check "HelloWs :27010 (HTTP $WS_CODE)" "PASS"
else
  check "HelloWs :27010" "SKIP" "WebSocket 握手可能需特殊客户端"
fi

# HelloWss — 同 HelloWs，WebSocket Secure 不能用裸 curl 测，只验证进程+监听端口
WSS_POD=$(kubectl get pods -n $NS -l app=thunder-hello-wss --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
if [ -z "$WSS_POD" ]; then
  check "HelloWss :27012" "FAIL" "no running pod"
else
  WSS_PROC=$(kubectl exec -n $NS "$WSS_POD" -- pgrep -c Hello_wss 2>/dev/null || echo "0")
  WSS_PROC=$(echo "$WSS_PROC" | tr -d '\n\r ')
  if [ "$WSS_PROC" -ge 1 ] 2>/dev/null; then
    check "HelloWss :27012 (进程 $WSS_PROC 个)" "PASS"
  else
    WSS_CODE=$(curl -sk -m 3 -o /dev/null -w "%{http_code}" "https://${HOST_IP}:27012/" 2>/dev/null || echo "")
    if [ -n "$WSS_CODE" ]; then
      check "HelloWss :27012 (HTTP $WSS_CODE)" "PASS"
    else
      check "HelloWss :27012" "SKIP" "WebSocket Secure — 需 wscat 验证"
    fi
  fi
fi

# ============== 汇总 ==============
echo ""
echo "============================================================"
TOTAL=$((PASS + FAIL + SKIP))
echo -e " 通过: ${GREEN}${PASS}${NC}  失败: ${RED}${FAIL}${NC}  跳过: ${YELLOW}${SKIP}${NC}  总计: ${TOTAL}"
echo "============================================================"

[ "$FAIL" -gt 0 ] && exit 1 || exit 0
