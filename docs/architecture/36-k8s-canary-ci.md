# Thunder 灰度发布 CI/CD 设计

> 设计日期：2026-07-09  |  状态：📋 设计阶段  |  关联：`34-k8s-canary-routing.md`（核心路由）、`35-k8s-canary-operator.md`（CRD + Operator）
>
> **CI 的职责边界**：只做编排和检查，不做权重路由。路由是 etcd + Manager + Worker 的职责，CI 通过 `kubectl patch GrayRelease CRD`（或 `etcdctl put`）触发，让 Operator 接管执行。

---

## 0. CI 在灰度架构中的位置

```
┌─ CI Pipeline ──────────────────────────────────────────────────────────┐
│                                                                         │
│  源码 push → 构建镜像 → 推送 registry → 触发灰度 CRD → 监控指标 → 决策  │
│                                                                         │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │ kubectl apply GrayRelease CRD
                                   ▼
┌─ Thunder Operator ──────────────────────────────────────────────────────┐
│  Reconcile: 建 Deployment → 写 etcd 权重 → 监控指标 → 自动回滚           │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │ etcd put
                                   ▼
┌─ Thunder 路由层 (etcd → Manager → Worker) ──────────────────────────────┐
│  权重键变更 → 共享内存 version++ → Worker 下一笔请求用新权重              │
└─────────────────────────────────────────────────────────────────────────┘
```

CI 和运行时完全解耦——CI pipeline 挂了，不影响正在进行的灰度（Operator 在集群内继续 reconcile）。

---

## 1. 总体 CI 流水线

```
┌─ 代码 CI ──────────────────────────────────────────────────────────────┐
│                                                                         │
│  C++ 路由层 CI          Go Operator CI                                  │
│  ├─ lint (clang-format)  ├─ lint (golangci-lint)                       │
│  ├─ build (cmake)        ├─ test (go test)                             │
│  ├─ unit test (gtest)    ├─ e2e (kind 集群)                            │
│  └─ 集成测试 (docker)    └─ build image                                 │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─ 镜像 CI ──────────────────────────────────────────────────────────────┐
│                                                                         │
│  push tag/branch → docker build → push registry → 签名(cosign)          │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─ 灰度发布 Pipeline ────────────────────────────────────────────────────┐
│                                                                         │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌───────┐ │
│  │ 部署 v2  │ → │ 冒烟测试  │ → │ weight=5 │ → │ weight=20│ → │  ...  │ │
│  │ weight=0 │   │ (直连v2) │   │ 观察60s  │   │ 观察120s │   │ 100%  │ │
│  └──────────┘   └──────────┘   └────┬─────┘   └────┬─────┘   └───────┘ │
│                                     │               │                  │
│                                ┌────▼────┐     ┌────▼────┐             │
│                                │指标检查  │     │指标检查  │             │
│                                │超标→回滚 │     │超标→回滚 │             │
│                                └─────────┘     └─────────┘             │
│                                                                         │
│  回滚路径: 任何步骤失败 → kubectl patch weight=0 → 秒级生效              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 代码 CI

### 2.1 C++ 路由层 CI

路由层改动量小（~75 行），但直接影响所有请求的路径选择，测试覆盖非常关键。

```yaml
# .github/workflows/thunder-core-ci.yml
name: Thunder Core CI

on:
  push:
    paths:
      - 'code/Net/src/dispatcher/Nodes.cpp'
      - 'code/Net/src/dispatcher/EtcdCenterConnector.*'
      - 'code/Net/proto/**'

jobs:

  # ── 静态检查 ───────────────────────────────────────
  lint:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: clang-format check
        run: find code/Net/src/dispatcher -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
      - name: clang-tidy
        run: cmake --build build --target clang-tidy

  # ── 编译 + 单元测试 ────────────────────────────────
  build-and-test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)

      - name: Unit test — GetNodeIdentify 加权随机
        run: |
          ./build/test_nodes_canary --gtest_filter=NodesCanaryTest.*

  # ── 集成测试 (docker-compose) ──────────────────────
  integration:
    runs-on: ubuntu-22.04
    needs: build-and-test
    steps:
      - uses: actions/checkout@v4

      - name: Start etcd + Thunder Manager + Worker
        run: docker-compose -f ci/docker-compose.canary.yml up -d --wait

      - name: 注册 v1/v2 节点
        run: |
          etcdctl put /thunder/nodes/LOGIC/10.0.0.1:16001 \
            '{"node_type":"LOGIC","node_version":"v1"}'
          etcdctl put /thunder/nodes/LOGIC/10.0.0.2:16002 \
            '{"node_type":"LOGIC","node_version":"v2"}'

      - name: 测试权重路由分布
        run: |
          # 写权重键
          etcdctl put /thunder/canary/LOGIC/weights '{"v1":70,"v2":30}'
          sleep 1  # 等 Manager Watch → Worker 感知

          # 发 1000 个请求，统计分布
          ./ci/canary_test_harness --requests 1000 --service LOGIC --output /tmp/routes.txt

          V1_COUNT=$(grep -c "10.0.0.1" /tmp/routes.txt)
          V2_COUNT=$(grep -c "10.0.0.2" /tmp/routes.txt)

          # 70/30 分布，允许 ±5% 波动
          if [ $V1_COUNT -lt 650 ] || [ $V1_COUNT -gt 750 ]; then
            echo "v1 分布异常: $V1_COUNT/1000 (期望 700±50)"
            exit 1
          fi
          if [ $V2_COUNT -lt 250 ] || [ $V2_COUNT -gt 350 ]; then
            echo "v2 分布异常: $V2_COUNT/1000 (期望 300±50)"
            exit 1
          fi
          echo "✅ 70/30 分布通过: v1=$V1_COUNT, v2=$V2_COUNT"

      - name: 测试秒级回滚
        run: |
          etcdctl put /thunder/canary/LOGIC/weights '{"v1":100,"v2":0}'
          sleep 1
          ./ci/canary_test_harness --requests 100 --service LOGIC --output /tmp/rollback.txt
          if grep -q "10.0.0.2" /tmp/rollback.txt; then
            echo "❌ 回滚失败：仍有请求到 v2"; exit 1
          fi
          echo "✅ 秒级回滚通过"

      - name: 测试权重键删除 → 回退一致性哈希
        run: |
          etcdctl del /thunder/canary/LOGIC/weights
          sleep 1
          # 验证回退到默认 hash 路由（不崩溃即可）
          ./ci/canary_test_harness --requests 50 --service LOGIC
          echo "✅ 回退一致性哈希通过"

      - name: Cleanup
        if: always()
        run: docker-compose -f ci/docker-compose.canary.yml down
```

### 2.2 C++ 单元测试用例

```cpp
// test/NodesCanaryTest.cpp
TEST(NodesCanaryTest, WeightedRandomDistribution) {
    // 模拟权重表: node1→70, node2→30
    CanaryWeights weights = {{"node1", 70}, {"node2", 30}};

    int count1 = 0, count2 = 0;
    for (int i = 0; i < 10000; i++) {
        auto& node = GetNodeIdentifyByWeight(weights, GetHash(i));
        if (node == "node1") count1++;
        else count2++;
    }

    // 70/30 分布，允许 3σ 波动
    EXPECT_NEAR(count1, 7000, 150);   // 7000 ± 150
    EXPECT_NEAR(count2, 3000, 150);
}

TEST(NodesCanaryTest, EmptyWeightsFallbackToHash) {
    CanaryWeights empty;
    // 权重表为空 → 走一致性哈希
    // (验证不崩溃 + 返回值在已知节点集合内)
    auto& node = GetNodeIdentifyByWeight(empty, "some_hash_key");
    EXPECT_FALSE(node.empty());
}

TEST(NodesCanaryTest, WeightZeroExcludesNode) {
    CanaryWeights weights = {{"node1", 100}, {"node2", 0}};
    for (int i = 0; i < 1000; i++) {
        EXPECT_EQ(GetNodeIdentifyByWeight(weights, GetHash(i)), "node1");
    }
}

TEST(NodesCanaryTest, SingleNodeAlwaysSelected) {
    CanaryWeights weights = {{"only_node", 100}};
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(GetNodeIdentifyByWeight(weights, GetHash(i)), "only_node");
    }
}
```

### 2.3 Go Operator CI

```yaml
# .github/workflows/operator-ci.yml
name: Operator CI

on:
  push:
    paths:
      - 'operator/**'

jobs:
  lint:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.22' }
      - run: cd operator && golangci-lint run ./...

  test:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with: { go-version: '1.22' }
      - name: Unit tests
        run: cd operator && go test -v -race ./...
      - name: Test Reconcile 逻辑 (mock K8s client + mock etcd)
        run: cd operator && go test -v ./controllers/...

  e2e:
    runs-on: ubuntu-22.04
    needs: test
    steps:
      - uses: actions/checkout@v4

      - name: Start kind cluster
        run: |
          kind create cluster --config ci/kind-config.yaml
          kubectl wait --for=condition=Ready node --all --timeout=120s

      - name: Deploy etcd
        run: kubectl apply -f ci/etcd-e2e.yaml

      - name: Build & load Operator image
        run: |
          cd operator
          docker build -t thunder-operator:e2e .
          kind load docker-image thunder-operator:e2e

      - name: Deploy Operator + CRD
        run: |
          kubectl apply -f operator/config/crd/bases/thunder.io_grayreleases.yaml
          kubectl apply -f ci/operator-e2e-deploy.yaml
          kubectl wait --for=condition=Ready pod -l app=thunder-operator --timeout=60s

      - name: Apply GrayRelease CRD → 验证
        run: |
          kubectl apply -f ci/testdata/grayrelease-e2e.yaml
          sleep 5
          # 验证：Deployment 被创建
          kubectl get deploy LOGIC-v2 -n thunder
          # 验证：etcd 权重键被写入
          kubectl exec etcd-0 -- etcdctl get /thunder/canary/LOGIC/weights
          # 验证：CRD status 更新
          PHASE=$(kubectl get gr LOGIC-v2-canary -n thunder -o jsonpath='{.status.phase}')
          [ "$PHASE" = "Running" ] || (echo "CRD phase=$PHASE 非预期" && exit 1)
          echo "✅ e2e 通过"

      - name: Cleanup
        if: always()
        run: kind delete cluster
```

---

## 3. 镜像构建与推送

```yaml
# .github/workflows/build-image.yml
name: Build & Push Image

on:
  push:
    branches: ['v*']             # v2, v3 ...
    tags:   ['release-*']        # release-v2.0.1
  workflow_dispatch:
    inputs:
      version:
        description: '镜像版本号'
        required: true

env:
  REGISTRY: registry.thunder.io

jobs:
  build:
    runs-on: ubuntu-22.04

    steps:
      - uses: actions/checkout@v4

      - name: 确定版本号
        id: version
        run: |
          if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then
            echo "VERSION=${{ inputs.version }}" >> $GITHUB_OUTPUT
          else
            echo "VERSION=${{ github.ref_name }}" >> $GITHUB_OUTPUT
          fi

      - name: Build C++ binary
        run: |
          cmake -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j$(nproc) --target thunder_logic

      - name: Build & Push Docker image
        uses: docker/build-push-action@v5
        with:
          context: .
          file: docker/Dockerfile.logic
          push: true
          tags: |
            ${{ env.REGISTRY }}/thunder-logic:${{ steps.version.outputs.VERSION }}
            ${{ env.REGISTRY }}/thunder-logic:${{ steps.version.outputs.VERSION }}-${{ github.sha }}

      - name: 镜像签名 (Cosign)
        run: |
          cosign sign --key cosign.key \
            ${{ env.REGISTRY }}/thunder-logic:${{ steps.version.outputs.VERSION }}

      - name: 输出镜像地址
        run: |
          echo "镜像: ${{ env.REGISTRY }}/thunder-logic:${{ steps.version.outputs.VERSION }}"
```

### Dockerfile 示例

```dockerfile
# docker/Dockerfile.logic
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libprotobuf32 ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY build/thunder_logic /usr/local/bin/thunder_logic
COPY config/logic.yaml /etc/thunder/logic.yaml

# 从 K8s downward API 注入
ENV NODE_VERSION=v1

EXPOSE 16068

ENTRYPOINT ["/usr/local/bin/thunder_logic"]
CMD ["--config", "/etc/thunder/logic.yaml"]
```

---

## 4. 灰度发布 Pipeline（核心）

这是最核心的流水线。CI 通过操控 GrayRelease CRD（或直接写 etcd）来驱动灰度流程。

### 4.1 Pipeline 全貌

```yaml
# .github/workflows/canary-release.yml
name: Canary Release

on:
  workflow_dispatch:
    inputs:
      cluster:
        description: '目标集群 (staging / production)'
        required: true
        type: choice
        options: [staging, production]
      service:
        description: '目标服务 (LOGIC/HELLO_HTTP/INTERFACE)'
        required: true
        default: LOGIC
      new_version:
        description: '新版本 tag (如 v2.0)'
        required: true
      image:
        description: '镜像地址 (留空则自动推导)'
        required: false
      canary_steps:
        description: '灰度阶梯 (逗号分隔)'
        required: true
        default: '5,20,50,100'
      observe_seconds_per_step:
        description: '每步最少观察秒数'
        required: false
        default: '120'
      error_rate_threshold:
        description: '错误率阈值 (超过则自动回滚)'
        required: false
        default: '0.01'

env:
  GR_NAME: '${{ inputs.service }}-${{ inputs.new_version }}'
  NAMESPACE: thunder

jobs:
  canary:
    runs-on: self-hosted
    environment: ${{ inputs.cluster }}
    timeout-minutes: 60

    steps:
      # ── Step 1: 前置检查 ──────────────────────────
      - name: 前置检查
        run: |
          echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
          echo "  灰度发布: ${{ inputs.service }} → ${{ inputs.new_version }}"
          echo "  灰度阶梯: ${{ inputs.canary_steps }}"
          echo "  目标集群: ${{ inputs.cluster }}"
          echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

          kubectl cluster-info || (echo "❌ 无法连接集群" && exit 1)

          V1_PODS=$(kubectl get pods -n $NAMESPACE \
            -l app=${{ inputs.service }},version=v1 \
            --field-selector=status.phase=Running --no-headers | wc -l)
          if [ "$V1_PODS" -lt 1 ]; then
            echo "❌ v1 无运行中的 Pod"; exit 1
          fi
          echo "✅ v1 运行中: $V1_PODS 个 Pod"

          EXISTING=$(kubectl get gr $GR_NAME -n $NAMESPACE \
            -o jsonpath='{.status.phase}' 2>/dev/null || true)
          if [ "$EXISTING" = "Progressing" ] || [ "$EXISTING" = "Running" ]; then
            echo "❌ 已有进行中的灰度 (phase=$EXISTING)"; exit 1
          fi

      # ── Step 2: 部署新版本 (weight=0) ─────────────
      - name: 部署新版本 (cold deploy)
        run: |
          echo "→ 部署 ${{ inputs.service }} ${{ inputs.new_version }} (weight=0，先不走流量)"

          IMAGE=${{ inputs.image }}
          if [ -z "$IMAGE" ]; then
            IMAGE="registry.thunder.io/thunder-${{ inputs.service }}:${{ inputs.new_version }}"
          fi

          kubectl apply -f - <<EOF
          apiVersion: thunder.io/v1
          kind: GrayRelease
          metadata:
            name: $GR_NAME
            namespace: $NAMESPACE
          spec:
            service: ${{ inputs.service }}
            newVersion: ${{ inputs.new_version }}
            image: $IMAGE
            weight: 0
            steps: [$(echo ${{ inputs.canary_steps }} | tr ',' ',')]
            rollbackOnErrorRate: ${{ inputs.error_rate_threshold }}
          EOF

      # ── Step 3: 等待新 Pod Ready ──────────────────
      - name: 等待新版本 Pod Ready
        timeout-minutes: 5
        run: |
          echo "→ 等待 ${{ inputs.new_version }} Pod Ready..."

          kubectl wait --for=condition=Ready pod \
            -n $NAMESPACE \
            -l app=${{ inputs.service }},version=${{ inputs.new_version }} \
            --timeout=300s

          NEW_PODS=$(kubectl get pods -n $NAMESPACE \
            -l app=${{ inputs.service }},version=${{ inputs.new_version }} \
            --field-selector=status.phase=Running --no-headers | wc -l)
          echo "✅ ${{ inputs.new_version }} Ready: $NEW_PODS 个 Pod"

      # ── Step 4: 冒烟测试 (直连 v2) ────────────────
      - name: 冒烟测试
        run: |
          echo "→ 冒烟测试（直连 v2 Pod，不经过权重路由）"

          V2_POD=$(kubectl get pod -n $NAMESPACE \
            -l app=${{ inputs.service }},version=${{ inputs.new_version }} \
            -o jsonpath='{.items[0].metadata.name}')

          kubectl port-forward -n $NAMESPACE $V2_POD 16668:16068 &
          PF_PID=$!
          trap "kill $PF_PID 2>/dev/null" EXIT
          sleep 2

          # 健康检查
          curl -fsS --max-time 5 http://127.0.0.1:16668/health || {
            echo "❌ v2 健康检查失败"; exit 1
          }
          echo "  ✅ 健康检查通过"

          # 回归测试
          ./ci/smoke-tests.sh --target http://127.0.0.1:16668 || {
            echo "❌ 冒烟测试失败"; exit 1
          }
          echo "✅ 冒烟测试全部通过"

          kill $PF_PID 2>/dev/null
          trap - EXIT

      # ── Step 5: 逐步放量 ────────────────────────────
      - name: "灰度 5%"
        run: |
          echo "→ 5% 流量 → ${{ inputs.new_version }}"
          kubectl patch gr $GR_NAME -n $NAMESPACE \
            --type=merge -p '{"spec":{"weight":5}}'

      - name: 观察 5%
        timeout-minutes: 5
        run: |
          ./ci/observe-and-check.sh \
            --service ${{ inputs.service }} \
            --new-version ${{ inputs.new_version }} \
            --observe-seconds ${{ inputs.observe_seconds_per_step }} \
            --error-threshold ${{ inputs.error_rate_threshold }}

      - name: "灰度 20%"
        run: |
          echo "→ 20% 流量 → ${{ inputs.new_version }}"
          kubectl patch gr $GR_NAME -n $NAMESPACE \
            --type=merge -p '{"spec":{"weight":20}}'

      - name: 观察 20%
        timeout-minutes: 10
        run: |
          ./ci/observe-and-check.sh \
            --service ${{ inputs.service }} \
            --new-version ${{ inputs.new_version }} \
            --observe-seconds 180 \
            --error-threshold ${{ inputs.error_rate_threshold }}

      # ── 人工确认门禁 (production only) ─────────────
      - name: "⚠️ 人工确认：继续放量到 50%?"
        if: inputs.cluster == 'production'
        uses: trstringer/manual-approval@v1
        with:
          secret: ${{ github.token }}
          approvers: thunder-ops
          minimum-approvals: 1
          issue-title: "灰度确认: ${{ inputs.service }} → ${{ inputs.new_version }} 20% 通过，继续?"

      - name: "灰度 50%"
        run: |
          echo "→ 50% 流量 → ${{ inputs.new_version }}"
          kubectl patch gr $GR_NAME -n $NAMESPACE \
            --type=merge -p '{"spec":{"weight":50}}'

      - name: 观察 50%
        timeout-minutes: 15
        run: |
          ./ci/observe-and-check.sh \
            --service ${{ inputs.service }} \
            --new-version ${{ inputs.new_version }} \
            --observe-seconds 300 \
            --error-threshold ${{ inputs.error_rate_threshold }}

      - name: "⚠️ 人工确认：全量切换?"
        uses: trstringer/manual-approval@v1
        with:
          secret: ${{ github.token }}
          approvers: thunder-ops
          minimum-approvals: 1
          issue-title: "灰度确认: ${{ inputs.service }} → ${{ inputs.new_version }} 50% 通过，全量?"

      # ── Step 6: 全量 + 清理 ────────────────────────
      - name: "全量 100%"
        run: |
          echo "→ 100% 流量 → ${{ inputs.new_version }}"
          kubectl patch gr $GR_NAME -n $NAMESPACE \
            --type=merge -p '{"spec":{"weight":100}}'
          echo "✅ 全量切换完成"

      - name: 等待存量连接排空 + 旧版本缩容
        run: |
          echo "→ 等待存量连接排空 (30s)..."
          sleep 30

          PHASE=$(kubectl get gr $GR_NAME -n $NAMESPACE -o jsonpath='{.status.phase}')
          echo "GrayRelease status: $PHASE"

          echo "→ 旧版本 Deployment replicas → 0"
          kubectl scale deploy ${{ inputs.service }}-v1 -n $NAMESPACE --replicas=0 \
            || echo "⚠️ 缩容 v1 失败（可能已被 Operator 处理）"

      # ── 收尾 ───────────────────────────────────────
      - name: 发布摘要
        if: always()
        run: |
          echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
          echo "  灰度发布完成"
          echo "  服务: ${{ inputs.service }}"
          echo "  版本: v1 → ${{ inputs.new_version }}"
          echo "  集群: ${{ inputs.cluster }}"
          echo "  状态: ${{ job.status }}"
          echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

      # ── 回滚 (任何步骤失败时触发) ──────────────────
      - name: ⚠️ 自动回滚
        if: failure()
        run: |
          echo "🚨 灰度失败，执行自动回滚..."
          kubectl patch gr $GR_NAME -n $NAMESPACE \
            --type=merge -p '{"spec":{"weight":0}}' \
            || etcdctl put /thunder/canary/${{ inputs.service }}/weights \
                 '{"v1":100,"v2":0}'
          echo "✅ 回滚已触发（秒级生效，v2 Pod 保留不杀）"
          ./ci/notify.sh "🚨 灰度回滚: ${{ inputs.service }} → ${{ inputs.new_version }}"
```

### 4.2 指标检查脚本

```bash
#!/bin/bash
# ci/observe-and-check.sh
# 灰度观察 + 指标检查

set -e

SERVICE="${SERVICE:-LOGIC}"
NEW_VERSION="${NEW_VERSION:-v2}"
OBSERVE_SECONDS="${OBSERVE_SECONDS:-120}"
ERROR_THRESHOLD="${ERROR_THRESHOLD:-0.01}"
CHECK_INTERVAL=10

echo "→ 观察中，间隔 ${CHECK_INTERVAL}s，共 ${OBSERVE_SECONDS}s"
echo "  错误率阈值: ${ERROR_THRESHOLD}"

elapsed=0

while [ $elapsed -lt $OBSERVE_SECONDS ]; do
    sleep $CHECK_INTERVAL
    elapsed=$((elapsed + CHECK_INTERVAL))

    # ── 检查 1: CRD status（Operator 可能已自动回滚）───
    PHASE=$(kubectl get gr "${SERVICE}-${NEW_VERSION}" -n thunder \
        -o jsonpath='{.status.phase}' 2>/dev/null || echo "Unknown")

    if [ "$PHASE" = "RolledBack" ]; then
        echo "🚨 Operator 已自动回滚 (${elapsed}s)"
        kubectl get gr "${SERVICE}-${NEW_VERSION}" -n thunder \
            -o jsonpath='{.status.message}'
        echo ""
        exit 1
    fi

    if [ "$PHASE" = "Failed" ]; then
        echo "🚨 灰度失败 (${elapsed}s)"
        exit 1
    fi

    # ── 检查 2: Prometheus 错误率 ──────────────────────
    ERROR_RATE=$(curl -s --max-time 5 \
        "${PROMETHEUS_URL}/api/v1/query?query=rate(thunder_errors_total{version=\"${NEW_VERSION}\"}[1m])" \
        | jq -r '.data.result[0].value[1] // "0"')

    if (( $(echo "$ERROR_RATE > $ERROR_THRESHOLD" | bc -l) )); then
        echo "🚨 错误率超标: ${ERROR_RATE} > ${ERROR_THRESHOLD} (${elapsed}s)"
        exit 1
    fi

    # ── 检查 3: P99 延迟 ────────────────────────────────
    P99=$(curl -s --max-time 5 \
        "${PROMETHEUS_URL}/api/v1/query?query=histogram_quantile(0.99,rate(thunder_request_duration_ms_bucket{version=\"${NEW_VERSION}\"}[1m]))" \
        | jq -r '.data.result[0].value[1] // "0"')

    if [ -n "$P99_THRESHOLD" ] && (( $(echo "$P99 > $P99_THRESHOLD" | bc -l) )); then
        echo "🚨 P99 延迟超标: ${P99}ms > ${P99_THRESHOLD}ms (${elapsed}s)"
        exit 1
    fi

    echo "  [${elapsed}s/${OBSERVE_SECONDS}s] 错误率=${ERROR_RATE}, P99=${P99}ms, phase=${PHASE}"
done

echo "✅ 观察通过 (${OBSERVE_SECONDS}s)"
```

---

## 5. 回滚机制

三道防线，从快到慢：

```
┌─ 防线 1: Operator 自动回滚 (最快，~1s) ─────────────────────────────────┐
│                                                                          │
│  Operator reconcile 循环持续检查 Prometheus:                             │
│    rollbackOnErrorRate: 0.01                                             │
│    rollbackOnLatencyP99: "500ms"                                         │
│  超标 → etcdPut weight=0 → 秒级生效                                      │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘

┌─ 防线 2: CI Pipeline 检测回滚 (~10s) ───────────────────────────────────┐
│                                                                          │
│  CI 每个灰度步骤调用 observe-and-check.sh:                               │
│    → 查 Prometheus 指标                                                  │
│    → 超标 → kubectl patch weight=0 → exit 1                              │
│    → CRD phase=RolledBack → exit 1                                       │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘

┌─ 防线 3: 人工回滚 (手动) ───────────────────────────────────────────────┐
│                                                                          │
│  kubectl patch gr LOGIC-v2 -n thunder --type=merge \                    │
│    -p '{"spec":{"weight":0}}'                                            │
│                                                                          │
│  或直接操作 etcd (绕过 Operator):                                        │
│  etcdctl put /thunder/canary/LOGIC/weights '{"v1":100,"v2":0}'           │
│                                                                          │
│  ⚠️ 不杀 v2 Pod，weight=0 只是断流。随时 weight=10 就能恢复。            │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

回滚后恢复：

```bash
# 回滚后 v2 Pod 还在，只是不走流量。问题修好后直接恢复：
kubectl patch gr LOGIC-v2 -n thunder --type=merge -p '{"spec":{"weight":10}}'
```

---

## 6. 环境策略

```
┌─ dev 分支 push ─────────────────────────────────────────────────┐
│                                                                  │
│  ① C++ CI (lint + build + unit test)         ← 每次 push 必跑   │
│  ② 集成测试 (etcd + 权重路由)                 ← 每次 push 必跑   │
│  ③ 不构建镜像，不触发灰度                                        │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘

┌─ v* 分支 push / release tag ────────────────────────────────────┐
│                                                                  │
│  ① 所有 dev CI 流程                                              │
│  ② 构建镜像 → 推送 registry (tag: v2.0-{sha})                   │
│  ③ 自动部署到 staging: weight=0 (cold deploy)                   │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘

┌─ 手动触发 canary-release workflow ──────────────────────────────┐
│                                                                  │
│  Staging 集群先跑完整灰度 (5→20→50→100)                          │
│    → 自动放量 + 自动指标检查 (无人工门禁)                        │
│    → staging 通过 → 进入 production 候选                          │
│                                                                  │
│  Production 集群灰度:                                            │
│    → 5% → 自动观察                                               │
│    → 20% → 自动观察 + ⚠️ 人工确认                                │
│    → 50% → 自动观察 + ⚠️ 人工确认                                │
│    → 100% → 自动完成                                             │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 7. 过渡期方案（Operator 上线前）

在 Operator 开发完成之前，可以用轻量脚本 + `etcdctl` 实现灰度流程，CI 直接调用：

```bash
#!/bin/bash
# ci/canary-raw.sh — 过渡期灰度脚本（直写 etcd，不依赖 Operator）

set -e

SERVICE="$1"       # LOGIC
NEW_VERSION="$2"   # v2
WEIGHT="$3"        # 10
ACTION="$4"        # deploy | canary | full | rollback

NAMESPACE="${NAMESPACE:-thunder}"
ETCD_ENDPOINT="${ETCD_ENDPOINT:-etcd.thunder:2379}"

deploy_new_version() {
    echo "→ 部署 ${SERVICE} ${NEW_VERSION}"

    sed "s/{{VERSION}}/${NEW_VERSION}/g" k8s/${SERVICE}-deploy.yaml | \
    sed "s/{{IMAGE}}/registry.thunder.io/thunder-${SERVICE}:${NEW_VERSION}/g" | \
        kubectl apply -f -

    kubectl wait --for=condition=Ready pod \
        -n "$NAMESPACE" \
        -l "app=${SERVICE},version=${NEW_VERSION}" \
        --timeout=120s
}

set_weight() {
    local v1_weight=$((100 - $1))
    local v2_weight=$1
    echo "→ 设置权重: v1=${v1_weight}%, v2=${v2_weight}%"

    etcdctl --endpoints="$ETCD_ENDPOINT" \
        put "/thunder/canary/${SERVICE}/weights" \
        "{\"v1\":${v1_weight},\"v2\":${v2_weight}}"
}

rollback() {
    echo "→ 回滚: 100% → v1"
    etcdctl --endpoints="$ETCD_ENDPOINT" \
        put "/thunder/canary/${SERVICE}/weights" '{"v1":100,"v2":0}'
}

case "$ACTION" in
    deploy)   deploy_new_version; set_weight 0 ;;
    canary)   set_weight "$WEIGHT" ;;
    full)     set_weight 100 ;;
    rollback) rollback ;;
    *)
        echo "用法: $0 <SERVICE> <VERSION> <WEIGHT> <deploy|canary|full|rollback>"
        exit 1 ;;
esac
```

CI 调用：
```yaml
- name: 灰度 10%
  run: ./ci/canary-raw.sh LOGIC v2 10 canary
```

当 Operator 就绪后，把 `ci/canary-raw.sh` 替换为 `kubectl patch GrayRelease CRD`，业务语义完全一致。

---

## 8. GitOps 方案（后续演进）

当团队规模扩大、需要更严格的变更管控时，可升级为 GitOps 模式。

### 目录结构

```
thunder-deployments/                  ← 独立 Git 仓库
├── staging/
│   └── LOGIC/
│       ├── grayscale.yaml
│       └── kustomization.yaml
└── production/
    └── LOGIC/
        ├── grayscale.yaml
        └── kustomization.yaml
```

### 工作流

```
开发者               Git 仓库                 ArgoCD              K8s + Operator
  │                    │                       │                    │
  │  提 PR: weight=5   │                       │                    │
  │──────────────────→│                       │                    │
  │                    │  CI: 冒烟测试          │                    │
  │  Review + Merge    │                       │                    │
  │──────────────────→│                       │                    │
  │                    │  webhook/定时 sync    │                    │
  │                    │──────────────────────→│                    │
  │                    │                       │  kubectl apply     │
  │                    │                       │──────────────────→│
  │                    │                       │                    │  Reconcile:
  │                    │                       │                    │  建 Deployment
  │                    │                       │                    │  写 etcd weight
  │  观察监控          │                       │                    │
  │  提 PR: weight=20  │                       │                    │
  │──────────────────→│                       │                    │
  │  ... 反复直到 100  │                       │                    │
```

### Pipeline 驱动 vs GitOps 对比

| | Pipeline 驱动 | GitOps 驱动 |
|---|---|---|
| **触发方式** | GitHub Actions 手动触发 | PR merge → ArgoCD sync |
| **灰度进度** | CI 脚本按步执行 | 每次 PR 改一个 weight 值 |
| **回滚** | 脚本内 `kubectl patch weight=0` | `git revert` → ArgoCD sync |
| **权限管控** | GitHub Actions 手动审批 | PR review + CODEOWNERS |
| **审计** | GitHub Actions 日志 | Git history（每次变更都有 commit） |
| **集群访问** | CI Runner 需要 kubectl 权限 | 不需要（ArgoCD 在集群内） |
| **适合团队** | 小团队快速迭代 | 多人协作、合规要求高 |

---

## 9. 实现计划

| 阶段 | 内容 | 预估 | 依赖 |
|:---:|---|:---:|---|
| **P0** | C++ 路由层 CI（lint + unit test + etcd 集成测试） | 1d | 34 号文档路由实现 |
| **P1** | 镜像构建 CI（build + push + cosign） | 0.5d | Dockerfile |
| **P2** | 过渡期灰度脚本 `ci/canary-raw.sh` | 0.5d | 34 号文档路由实现 |
| **P3** | Pipeline 驱动灰度 workflow (`canary-release.yml`) | 2d | 35 号文档 Operator |
| **P4** | 指标检查脚本 `ci/observe-and-check.sh` | 1d | Prometheus |
| **P5** | GitOps（ArgoCD 集成） | 延后 | Operator 稳定 |

---

## 10. 文件清单

```
thunder/
├── .github/workflows/
│   ├── thunder-core-ci.yml          # C++ 路由层 CI
│   ├── operator-ci.yml              # Go Operator CI
│   ├── build-image.yml              # 镜像构建
│   └── canary-release.yml           # 灰度发布 Pipeline
│
├── ci/
│   ├── docker-compose.canary.yml    # etcd + Manager + Worker 集成测试环境
│   ├── kind-config.yaml             # kind 集群配置 (Operator e2e)
│   ├── testdata/
│   │   └── graylease-e2e.yaml      # e2e 测试用 CRD
│   ├── canary_test_harness          # 路由分布测试工具
│   ├── smoke-tests.sh               # v2 直连冒烟测试
│   ├── observe-and-check.sh         # 灰度观察 + 指标检查
│   ├── canary-raw.sh                # 过渡期直写 etcd 脚本
│   └── notify.sh                    # 通知脚本
│
├── test/
│   └── NodesCanaryTest.cpp          # 加权随机单元测试
│
├── k8s/
│   ├── LOGIC-deploy.yaml            # Deployment 模板
│   └── grayscale-template.yaml     # GrayRelease CRD 模板
│
└── operator/
    └── controllers/
        └── graylease_controller_test.go  # Reconcile 单元测试
```
