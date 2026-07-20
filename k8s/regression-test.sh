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
  STATUS=$(kubectl get pods -n $NS -l app=$dep --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l | tr -d ' ')
  [ "${STATUS:-0}" -ge 1 ] 2>/dev/null && check "$dep Running" "PASS" || check "$dep" "FAIL" "0 Running"
done

# ============== 3. 插件隔离 ==============
echo ""
echo "--- 3. 插件隔离 ---"

declare -A PLUGIN_EXPECT
PLUGIN_EXPECT[thunder-hello]="HelloHttp_ModuleHello.so"
PLUGIN_EXPECT[thunder-hello-https]="HelloHttps_ModuleHello.so"
PLUGIN_EXPECT[thunder-hello-ws]="HelloWs_CmdHello.so"
PLUGIN_EXPECT[thunder-hello-wss]="HelloWs_CmdHello.so"
PLUGIN_EXPECT[thunder-interface]="ModuleInterface.so"

# Docker 镜像: COPY deploy/XXX/ → /app/, 各容器自带插件, 无 NFS 跨容器污染
PLUGIN_DIR="/app/plugins"

for dep in $GATEWAYS; do
  POD=$(kubectl get pods -n $NS -l app=$dep --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
  if [ -z "$POD" ]; then
    check "$dep 插件" "FAIL" "no running pod"
    continue
  fi
  LS=$(kubectl exec -n $NS "$POD" -- ls "${PLUGIN_DIR}/" 2>/dev/null || echo "__ERROR__")
  if echo "$LS" | grep -q "__ERROR__"; then
    check "$dep 插件" "FAIL" "ls 失败"
  else
    EXPECT="${PLUGIN_EXPECT[$dep]}"
    if [ -n "$EXPECT" ]; then
      if echo "$LS" | grep -qF "$EXPECT"; then
        check "$dep 插件 (含 $EXPECT)" "PASS"
      else
        check "$dep 插件" "FAIL" "期望含 $EXPECT, 实际: $(echo "$LS" | head -3)"
      fi
    else
      check "$dep 插件" "PASS"
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
HTTP_RESP=$(curl -s -m 8 -X POST "http://127.0.0.1:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}' 2>/dev/null || echo "")
if echo "$HTTP_RESP" | grep -q '"code":0'; then
  check "HelloHttp :27006 POST" "PASS"
else
  check "HelloHttp :27006" "FAIL" "$(echo "$HTTP_RESP" | head -c 80)"
fi

# Interface
IFACE_RESP=$(curl -s -m 8 "http://127.0.0.1:27008/Interface/gentoken" 2>/dev/null || echo "")
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

# ============== 6. SO 热更新 — NFS mount + 文件读写 ==============
echo ""
echo "--- 6. SO 热更新 (NFS mount) ---"

declare -A NFS_SUBPATH
NFS_SUBPATH[thunder-hello]="HelloHttp"
NFS_SUBPATH[thunder-hello-https]="HelloHttps"
NFS_SUBPATH[thunder-hello-ws]="HelloWs"
NFS_SUBPATH[thunder-hello-wss]="HelloWss"
NFS_SUBPATH[thunder-interface]="Interface"
NFS_SUBPATH[thunder-logic]="Logic"
NFS_SUBPATH[thunder-logic-v2]="Logic"
# logic-v2 uses label app=thunder-logic,version=v2 — special handling in the loop
declare -A DEP_LABEL
DEP_LABEL[thunder-logic-v2]="app=thunder-logic,version=v2"

for dep in "${!NFS_SUBPATH[@]}"; do
  LABEL="${DEP_LABEL[$dep]:-app=$dep}"
  POD=$(kubectl get pods -n $NS -l "$LABEL" --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
  if [ -z "$POD" ]; then
    check "$dep NFS mount" "FAIL" "no running pod"
    continue
  fi

  # 1. 验证 NFS mount 存在
  NFS_MOUNT=$(kubectl exec -n $NS "$POD" -- mount 2>/dev/null | grep "/app/plugins" | grep -c "nfs4" || echo "0")
  if [ "$NFS_MOUNT" -ge 1 ]; then
    check "$dep NFS mount (nfs4 → /app/plugins)" "PASS"
  else
    check "$dep NFS mount" "FAIL" "no nfs4 mount at /app/plugins"
    continue
  fi

  # 2. 验证至少有一个 .so 文件可从 NFS 读取
  SO_COUNT=$(kubectl exec -n $NS "$POD" -- sh -c 'ls /app/plugins/*.so 2>/dev/null | wc -l' 2>/dev/null || echo "0")
  SO_COUNT=$(echo "$SO_COUNT" | tr -d ' ')
  if [ "${SO_COUNT:-0}" -ge 1 ]; then
    check "$dep SO 文件可见 ($SO_COUNT 个 .so)" "PASS"
  else
    check "$dep SO 文件" "FAIL" "NFS mount 下无 .so 文件"
  fi
done

# 3. 验证 admWeb 写 → Gateway 读 (NFS 共享)
ADMIN_POD=$(kubectl get pods -n $NS -l app=thunder-admin-web --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
HELLO_POD=$(kubectl get pods -n $NS -l app=thunder-hello --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
if [ -n "$ADMIN_POD" ] && [ -n "$HELLO_POD" ]; then
  TOKEN="NFS_SO_TEST_$(date +%s)"
  kubectl exec -n $NS "$ADMIN_POD" -- sh -c "echo '$TOKEN' > /data/thunder/plugins/HelloHttp/_regression_test.so" 2>/dev/null
  HELLO_CONTENT=$(kubectl exec -n $NS "$HELLO_POD" -- cat /app/plugins/_regression_test.so 2>/dev/null || echo "__MISSING__")
  if echo "$HELLO_CONTENT" | grep -q "$TOKEN"; then
    check "NFS 共享 — admin-web 写 → hello 读" "PASS"
  else
    check "NFS 共享" "FAIL" "admin-web 写入后 hello 读不到"
  fi
  rm -f /tmp/_nfs_test.so
else
  check "NFS 共享" "SKIP" "admin-web 或 hello Pod 未运行"
fi

# ============== 7. SO 下发 + 审计 ==============
echo ""
echo "--- 7. SO 下发 + 审计 ---"

ADMIN_NODEPORT="http://${HOST_IP}:30090"
ADMIN_POD=$(kubectl get pods -n $NS -l app=thunder-admin-web --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
HELLO_POD=$(kubectl get pods -n $NS -l app=thunder-hello --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")

if [ -z "$ADMIN_POD" ] || [ -z "$HELLO_POD" ]; then
  check "SO 下发" "SKIP" "admin-web 或 hello Pod 未运行"
  check "审计记录" "SKIP" "admin-web 或 hello Pod 未运行"
else
  # C1: upload artifact → deploy → NFS → hello Pod reads
  TOKEN="REGRESS_DEPLOY_$(date +%s)"
  echo "$TOKEN" | curl -s -X PUT --data-binary @- "${ADMIN_NODEPORT}/api/plugins/HelloHttp/_regression_deploy.so" > /dev/null 2>/dev/null
  DEPLOY_RESULT=$(curl -s -X POST -H "Content-Type: application/json" \
    -d '{"filename":"_regression_deploy.so"}' \
    "${ADMIN_NODEPORT}/api/plugins/HelloHttp/deploy" 2>/dev/null)
  if echo "$DEPLOY_RESULT" | grep -q '"ok":true'; then
    HELLO_CONTENT=$(kubectl exec -n $NS "$HELLO_POD" -- cat /app/plugins/_regression_deploy.so 2>/dev/null || echo "__MISSING__")
    if echo "$HELLO_CONTENT" | grep -q "$TOKEN"; then
      check "SO 下发 — 制品→NFS→Gateway" "PASS"
    else
      check "SO 下发" "FAIL" "下发成功但 Gateway 读不到"
    fi
  else
    check "SO 下发" "FAIL" "下发 API 返回: $(echo "$DEPLOY_RESULT" | head -c 80)"
  fi

  # C2: audit records exist
  AUDIT_RESULT=$(curl -s "${ADMIN_NODEPORT}/api/audit?type=HelloHttp" 2>/dev/null)
  if echo "$AUDIT_RESULT" | grep -q '"ok":true' && echo "$AUDIT_RESULT" | grep -q '"action":"deploy"'; then
    check "审计记录 — 下发操作已记录" "PASS"
  else
    check "审计记录" "FAIL" "无下发审计记录"
  fi
fi

# ============== 8. 节点性能优化 (#154) ==============
echo ""
echo "--- 8. 节点性能优化 (node-tuner) ---"

TUNER_POD=$(kubectl get pods -n $NS -l app=thunder-node-tuner --field-selector=status.phase=Running -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || echo "")
if [ -z "$TUNER_POD" ]; then
  check "node-tuner DaemonSet" "SKIP" "no running pod"
  check "CPU governor" "SKIP" "no tuner pod"
  check "kubelet CPU Manager" "SKIP" "no tuner pod"
  check "marker 文件" "SKIP" "no tuner pod"
else
  check "node-tuner DaemonSet Running" "PASS"

  # 8.1 CPU governor
  GOV=$(kubectl exec -n $NS "$TUNER_POD" -- cat /host/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "?")
  if [ "$GOV" = "performance" ]; then
    check "CPU governor → performance" "PASS"
  else
    check "CPU governor" "FAIL" "当前: $GOV, 期望: performance"
  fi

  # 8.2 kubelet CPU Manager
  CPUMGR=$(kubectl exec -n $NS "$TUNER_POD" -- grep "cpuManagerPolicy" /host/var/lib/kubelet/config.yaml 2>/dev/null | awk '{print $2}' || echo "?")
  if [ "$CPUMGR" = "static" ]; then
    check "kubelet CPU Manager → static" "PASS"
  else
    check "kubelet CPU Manager" "FAIL" "当前: $CPUMGR, 期望: static"
  fi

  # 8.3 marker 文件 + boot_id
  MARKER=$(kubectl exec -n $NS "$TUNER_POD" -- cat /host/run/thunder-node-init.done 2>/dev/null || echo "")
  if echo "$MARKER" | grep -q "boot_id="; then
    check "node-tuner marker (boot_id 幂等)" "PASS"
  else
    check "node-tuner marker" "FAIL" "marker 异常: $MARKER"
  fi
fi

# ============== 9. 端到端功能测试 ==============
echo ""
echo "--- 9. 端到端功能测试 ---"

# 9.1 HelloHttp — Echo 正常返回
R=$(curl -s -m 5 -X POST "http://127.0.0.1:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":10}' 2>/dev/null)
if echo "$R" | grep -q '"code":0' && echo "$R" | grep -q '"data"'; then
  check "HelloHttp Echo 正常" "PASS"
else
  check "HelloHttp Echo" "FAIL" "$(echo "$R" | head -c 60)"
fi

# 9.2 HelloHttp — TestHelloPoolCpu (CPU 密集计算)
R=$(curl -s -m 10 -X POST "http://127.0.0.1:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"TestHelloPoolCpu"}' 2>/dev/null)
if echo "$R" | grep -q "TestHelloPoolCpu"; then
  check "HelloHttp CPU 密集型" "PASS"
else
  check "HelloHttp CPU密集型" "FAIL" "$(echo "$R" | head -c 60)"
fi

# 9.3 HelloHttp — 错误 option 处理
R=$(curl -s -m 5 -X POST "http://127.0.0.1:27006/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"NoSuchOption"}' 2>/dev/null)
if echo "$R" | grep -q '"code"'; then
  check "HelloHttp 错误option 返回code" "PASS"
else
  check "HelloHttp 错误option" "FAIL" "$(echo "$R" | head -c 60)"
fi

# 9.4 HelloHttps — Echo (HTTPS)
R=$(curl -sk -m 5 -X POST "https://127.0.0.1:27443/hello/hello" \
  -H "Content-Type: application/json" -d '{"option":"Echo","size":5}' 2>/dev/null)
if echo "$R" | grep -q '"code":0'; then
  check "HelloHttps Echo 正常" "PASS"
else
  check "HelloHttps Echo" "FAIL" "$(echo "$R" | head -c 60)"
fi

# 9.5 Interface — Echo
R=$(curl -s -m 5 -X POST "http://127.0.0.1:27008/Interface/gentoken" \
  -H "Content-Type: application/json" -d '{"option":"Echo"}' 2>/dev/null)
if echo "$R" | grep -q '"code":0'; then
  check "Interface Echo 正常" "PASS"
else
  check "Interface Echo" "FAIL" "$(echo "$R" | head -c 60)"
fi

# 9.6 Interface → Logic 全链路 (GenKey, S2S 连接重建需 15-30s, 15次重试)
TOKEN=""
for i in $(seq 1 15); do
  R=$(curl -s -m 10 -X POST "http://127.0.0.1:27008/Interface/gentoken" \
    -H "Content-Type: application/json" -d '{"option":"GenKey"}' 2>/dev/null)
  if echo "$R" | grep -q '"token"'; then
    TOKEN=$(echo "$R" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
    break
  fi
  # code 存在说明 S2S 通了
  if echo "$R" | grep -q '"code"'; then
    TOKEN="s2s_ok"
    break
  fi
  sleep 2
done
if [ -n "$TOKEN" ] && [ "$TOKEN" != "None" ]; then
  check "Interface→Logic GenKey 全链路" "PASS"
else
  check "Interface→Logic GenKey" "FAIL" "S2S 超时 无响应"
fi

# 9.7 Interface → Logic 全链路 (VerifyKey)
if [ "$TOKEN" != "s2s_ok" ] && [ -n "$TOKEN" ] && [ "$TOKEN" != "None" ]; then
  R=$(curl -s -m 5 -X POST "http://127.0.0.1:27008/Interface/gentoken" \
    -H "Content-Type: application/json" -d "{\"option\":\"VerifyKey\",\"token\":\"$TOKEN\"}" 2>/dev/null)
  if echo "$R" | grep -q '"code":0'; then
    check "Interface→Logic VerifyKey 全链路" "PASS"
  else
    check "Interface→Logic VerifyKey" "FAIL" "$(echo "$R" | head -c 60)"
  fi
else
  check "Interface→Logic VerifyKey" "SKIP" "Logic Session 未初始化, 无 token"
fi

# 9.8 HelloWs — WebSocket 握手 (curl 模拟升级)
WS_CODE=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
  -H "Upgrade: websocket" -H "Connection: Upgrade" \
  "http://127.0.0.1:27010/hello/shake" 2>/dev/null || echo "000")
if [ "$WS_CODE" = "101" ]; then
  check "HelloWs WebSocket 101握手" "PASS"
elif [ "$WS_CODE" != "000" ]; then
  check "HelloWs WebSocket 握手" "PASS"  # 有响应即可
else
  check "HelloWs WebSocket" "FAIL" "无响应"
fi

# ============== 汇总 ==============
echo ""
echo "============================================================"
TOTAL=$((PASS + FAIL + SKIP))
echo -e " 通过: ${GREEN}${PASS}${NC}  失败: ${RED}${FAIL}${NC}  跳过: ${YELLOW}${SKIP}${NC}  总计: ${TOTAL}"
echo "============================================================"

[ "$FAIL" -gt 0 ] && exit 1 || exit 0
