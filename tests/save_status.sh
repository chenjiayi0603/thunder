#!/usr/bin/env bash
# 测试完跑这个，把结果写入 TEST_STATUS.md
# 用法: tests/save_status.sh [--quick]   --quick 跳过 E2E

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATUS="$ROOT/TEST_STATUS.md"
QUICK=${1:-}
DATE=$(date '+%Y-%m-%d %H:%M')
BRANCH=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")

echo "=== Thunder 测试状态更新 ==="
echo "分支: $BRANCH | 时间: $DATE"
echo ""

# ---- 构建 ----
echo "[1/3] 构建..."
BUILD_RESULT="❌"
BUILD_NOTE=""
if cd "$ROOT/build" && cmake --build . -j"$(nproc)" --quiet 2>&1 | tail -3; then
    BUILD_RESULT="✅ 0 error"
    BUILD_NOTE="RelWithDebInfo"
else
    BUILD_RESULT="❌ 构建失败"
fi
echo "  构建: $BUILD_RESULT"

# ---- C++ ctest ----
echo "[2/3] ctest..."
CTEST_RESULT="❌"
CTEST_NOTE=""
if cd "$ROOT/build" && ctest -j"$(nproc)" --output-on-failure -q 2>&1 > /tmp/ctest_out.txt; then
    PASS=$(grep -oP '\d+(?= tests passed)' /tmp/ctest_out.txt | tail -1)
    SKIP=$(grep -oP '\d+(?= tests? skipped)' /tmp/ctest_out.txt | tail -1 || echo "0")
    CTEST_RESULT="✅ **${PASS}/${PASS}**"
    CTEST_NOTE="${SKIP} skipped"
else
    FAIL=$(grep -oP '\d+(?= tests? failed)' /tmp/ctest_out.txt | tail -1 || echo "?")
    PASS=$(grep -oP '\d+(?= tests passed)' /tmp/ctest_out.txt | tail -1 || echo "?")
    CTEST_RESULT="❌ ${PASS} pass / ${FAIL} fail"
    CTEST_NOTE="$(tail -5 /tmp/ctest_out.txt)"
fi
echo "  ctest: $CTEST_RESULT"

# ---- Python unit ----
echo "[3a/3] Python 单元测试..."
PY_RESULT="❌"
if cd "$ROOT" && python3 -m pytest tests/unit/ -q 2>&1 > /tmp/pytest_out.txt; then
    PASS=$(grep -oP '\d+(?= passed)' /tmp/pytest_out.txt | head -1)
    SKIP=$(grep -oP '\d+(?= skipped)' /tmp/pytest_out.txt | head -1 || echo "0")
    PY_RESULT="✅ **${PASS}/${PASS}**，${SKIP} skipped"
else
    PY_RESULT="❌ $(tail -3 /tmp/pytest_out.txt)"
fi
echo "  pytest unit: $PY_RESULT"

# ---- E2E（可选）----
E2E_RESULT="⏭ 跳过（--quick 模式）"
E2E_DATE="(未运行)"
if [[ -z "$QUICK" ]]; then
    echo "[3b/3] E2E 测试（约 3-5 分钟）..."
    if cd "$ROOT" && ./deploy.sh test e2e 2>&1 > /tmp/e2e_out.txt; then
        PASS=$(grep -oP '\d+(?= passed)' /tmp/e2e_out.txt | tail -1)
        FAIL=$(grep -oP '\d+(?= failed)' /tmp/e2e_out.txt | tail -1 || echo "0")
        SKIP=$(grep -oP '\d+(?= skipped)' /tmp/e2e_out.txt | tail -1 || echo "0")
        E2E_RESULT="✅ **${PASS}** pass，${FAIL} fail，${SKIP} skip"
    else
        FAIL=$(grep -oP '\d+(?= failed)' /tmp/e2e_out.txt | tail -1 || echo "?")
        E2E_RESULT="❌ ${FAIL} 失败，见 /tmp/e2e_out.txt"
    fi
    E2E_DATE="$DATE"
    echo "  E2E: $E2E_RESULT"
fi

# ---- 更新 TEST_STATUS.md ----
echo ""
echo "写入 TEST_STATUS.md..."

# 用 Python 做精确替换，避免 sed 多行地狱
python3 - <<PYEOF
import re

with open("$STATUS", "r") as f:
    content = f.read()

# 更新头部信息表
content = re.sub(r'(\| 日期\s+\|)[^\n]+', r'\1 $DATE |', content)
content = re.sub(r'(\| 分支\s+\|)[^\n]+', r'\1 \`$BRANCH\` |', content)

# 更新构建状态
content = re.sub(
    r'(\| \`\./deploy\.sh build\`\s+\|)[^\n]+',
    r'\1 $BUILD_RESULT | $BUILD_NOTE |',
    content
)

# 更新 ctest
content = re.sub(
    r'(\| 上次运行\s+\| )[\d-]+ \|[^\n]*\n(\| 结果\s+\| )[^\n]+\n(\| 命令)',
    lambda m: m.group(1) + "$DATE |\n" + m.group(2) + "$CTEST_RESULT | $CTEST_NOTE |\n" + m.group(3),
    content,
    count=1
)

with open("$STATUS", "w") as f:
    f.write(content)

print("  TEST_STATUS.md 已更新")
PYEOF

echo ""
echo "=== 完成 ==="
echo "构建:      $BUILD_RESULT"
echo "ctest:     $CTEST_RESULT ($CTEST_NOTE)"
echo "pytest:    $PY_RESULT"
echo "E2E:       $E2E_RESULT"
echo ""
echo "查看完整状态: cat TEST_STATUS.md"
