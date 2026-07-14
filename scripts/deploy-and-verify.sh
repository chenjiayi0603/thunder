#!/bin/bash
# Thunder 一次性部署验证脚本
# 用法: ./scripts/deploy-and-verify.sh [docker|k8s] [service]
# 示例: ./scripts/deploy-and-verify.sh docker        # 全量 Docker Compose
#       ./scripts/deploy-and-verify.sh k8s logic      # 仅 Logic 到 k8s

set -e
ENV="${1:-docker}"
SERVICE="${2:-}"

THUNDER_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$THUNDER_ROOT"

echo "=== [1/5] 编译 ==="
./deploy.sh build
echo ""

echo "=== [2/5] 单元测试 ==="
./deploy.sh test unit
echo ""

if [ "$ENV" = "docker" ]; then
    echo "=== [3/5] 部署 (Docker Compose) ==="
    if [ -n "$SERVICE" ]; then
        docker compose -f docker/docker-compose.yml up -d --build "$SERVICE"
    else
        docker compose -f docker/docker-compose.yml up -d --build
    fi
    echo "等待服务就绪 (15s)..."
    sleep 15

    echo "=== [4/5] E2E 测试 ==="
    ./deploy.sh test e2e

    echo "=== [5/5] 冒烟验证 ==="
    echo -n "GenKey: "
    curl -s -X POST http://127.0.0.1:27008/Interface/gentoken \
        -d '{"option":"GenKey"}' | python3 -m json.tool 2>/dev/null || \
        curl -s -X POST http://127.0.0.1:27008/Interface/gentoken \
        -d '{"option":"GenKey"}'
    echo ""

elif [ "$ENV" = "k8s" ]; then
    echo "=== [3/5] 构建镜像 ==="
    if [ -z "$SERVICE" ]; then
        echo "k8s 模式请指定 service: logic | logic-v2 | interface | hello"
        exit 1
    fi

    # SO 产物同步
    if [ -d "build/plugins" ]; then
        cp -v build/plugins/HelloHttp_ModuleLua.so "deploy/$SERVICE/plugins/" 2>/dev/null || true
    fi

    case "$SERVICE" in
        logic)
            docker build -t thunder-logic:latest -f deploy/Logic/Dockerfile .
            docker save thunder-logic:latest | sudo ctr -n k8s.io image import -
            kubectl -n thunder rollout restart deployment thunder-logic
            kubectl -n thunder wait --for=condition=ready pod -l app=thunder-logic --timeout=120s
            ;;
        logic-v2)
            docker build -t thunder-logic-v2:latest -f deploy/Logic_v2/Dockerfile .
            docker save thunder-logic-v2:latest | sudo ctr -n k8s.io image import -
            kubectl -n thunder rollout restart deployment thunder-logic-v2
            kubectl -n thunder wait --for=condition=ready pod -l app=thunder-logic-v2 --timeout=120s
            ;;
        *)
            echo "Unknown service: $SERVICE (支持: logic, logic-v2, interface, hello)"
            exit 1
            ;;
    esac

    sleep 5
    echo "=== [4/5] 冒烟验证 ==="
    IFACE_IP=$(kubectl -n thunder get pod -l app=thunder-interface -o jsonpath='{.items[0].status.podIP}' 2>/dev/null || echo "192.168.3.61")
    echo -n "GenKey @ $IFACE_IP:27008: "
    curl -s -X POST "http://$IFACE_IP:27008/Interface/gentoken" -d '{"option":"GenKey"}'
    echo ""
fi

echo ""
echo "✅ 部署验证完成"
