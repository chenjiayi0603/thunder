#!/usr/bin/env bash
# Thunder 测试 — 简单快速
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
SKIP_BUILD=false; SKIP_DOCKER=false
BUILD_DIR="${BUILD_DIR:-build}"; BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"

while [[ $# -gt 0 ]]; do
  case "$1" in --skip-build) SKIP_BUILD=true ;; --skip-docker) SKIP_DOCKER=true ;; *) echo "Usage: $0 [--skip-build] [--skip-docker]"; exit 1 ;; esac; shift
done

echo "===== Thunder $(date '+%H:%M') ====="

# ── 构建 ──
if [[ $SKIP_BUILD != true ]]; then
  echo "--- build ---"
  cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}" || { echo -e "${RED}build failed${NC}"; exit 1; }
  cmake --install "${BUILD_DIR}" || true
  cp -f build/lib/HelloHttp_ModuleLua.so deploy/HelloHttp/plugins/
  echo -e "${GREEN}  done${NC}"
fi

# ── Docker ──
if [[ $SKIP_DOCKER != true ]]; then
  echo "--- docker ---"
  docker compose -f docker/docker-compose.yml down 2>/dev/null || true
  docker compose -f docker/docker-compose.yml up -d 2>&1 | tail -1
  for i in $(seq 1 30); do
    curl -sf --max-time 1 http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}' 2>/dev/null | grep -q '"code":0' && break
    [[ $i -eq 30 ]] && { echo -e "${RED}timeout${NC}"; exit 1; }
    sleep 1
  done
  echo -e "${GREEN}  ready${NC}"
fi

# ── 测试 ──
echo "--- test ---"
# 先确认服务活着
curl -sf --max-time 3 http://127.0.0.1:27006/hello/hello -d '{"option":"Echo"}' | grep -q '"code":0' || { echo -e "${RED}server down${NC}"; exit 1; }

# unit (串行执行，避免构建和测试抢资源)
python3 -m pytest tests/unit/ -q 2>&1 | tail -1 | grep -q "passed" && echo -e "${GREEN}  ✅ python${NC}" || { echo -e "${RED}  ❌ python${NC}"; exit 1; }

# gtest
if [[ -d "${BUILD_DIR}/code/test" ]]; then
  ctest --test-dir "${BUILD_DIR}/code/test" -j1 2>&1 | grep -q "100% tests passed" && echo -e "${GREEN}  ✅ gtest${NC}" || { echo -e "${RED}  ❌ gtest${NC}"; exit 1; }
fi

# lua
for mode in fire_forget default; do
  curl -sf --max-time 5 "http://127.0.0.1:27006/hello/lua_node_type" -d "{\"mode\":\"$mode\"}" | grep -q '"code":0' || { echo -e "${RED}  ❌ lua $mode${NC}"; exit 1; }
done
echo -e "${GREEN}  ✅ lua${NC}"

# smoke
bash tests/test_smoke.sh --hello 2>&1 | grep -E "✅|PASS" | tail -10

# graceful restart
echo "  graceful restart..."
GR_OUT=$(bash tests/test_graceful_restart.sh --restart 2>&1) && \
  echo "$GR_OUT" | grep -q "全部通过" && \
  echo -e "${GREEN}  ✅ graceful restart${NC}" || {
    echo -e "${RED}  ❌ graceful restart${NC}"
    echo "$GR_OUT" | grep -E "✅|❌"
    exit 1
  }

echo -e "${GREEN}===== pass =====${NC}"
