#!/bin/bash
cd "$(dirname "$(readlink -f "$0")")"
PORT="${CDP_PORT:-9222}"
URL="${ADMIN_URL:-http://127.0.0.1:8090}"

# ---- 自动安装 ----
[ ! -d "node_modules" ] && echo "📦 安装依赖..." && npm install --silent 2>&1 | tail -1

CHROME=$(find ~/.cache/ms-playwright -name chrome -type f 2>/dev/null | head -1)
[ -z "$CHROME" ] && echo "🌐 安装 Playwright Chrome..." && npx playwright install chromium 2>&1 | tail -2

# ---- 自动启动后端 ----
if ! curl -s --connect-timeout 2 "$URL" >/dev/null 2>&1; then
  echo "🚀 启动 admin-web..."
  docker compose -f ~/thunder/docker/docker-compose.yml up -d admin-web 2>&1 | tail -1
  for i in $(seq 1 30); do
    curl -s --connect-timeout 1 "$URL" >/dev/null 2>&1 && break
    sleep 1
  done
fi

# ---- 连接显示器 ----
export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000

fuser -k $PORT/tcp 2>/dev/null || true
sleep 1

echo "🤖 脚本机器人 — 浏览器窗口实时操作"
echo ""

# 开可见浏览器
$CHROME --remote-debugging-port=$PORT --no-first-run --no-sandbox \
  --disable-gpu --disable-software-rasterizer \
  --ozone-platform=wayland --window-size=1280,800 "$URL" &
trap "fuser -k $PORT/tcp 2>/dev/null" EXIT
sleep 6

# 获取 CDP
WS=""
for i in $(seq 1 30); do
  WS=$(curl -s --max-time 1 http://127.0.0.1:$PORT/json/version 2>/dev/null | \
       python3 -c "import sys,json;print(json.load(sys.stdin).get('webSocketDebuggerUrl',''))" 2>/dev/null)
  [ -n "$WS" ] && break
  sleep 0.2
done

if [ -z "$WS" ]; then echo "❌ CDP 连不上"; exit 1; fi
echo "✅ 浏览器已连接"

# 跑测试
node -e "
const pw = require('@playwright/test');
(async () => {
  const b = await pw.chromium.connectOverCDP('$WS');
  const p = b.contexts()[0].pages()[0];
  let pass=0, fail=0;
  const T = async (n,fn) => { try{await fn();console.log('  ✅ '+n);pass++} catch(e){console.log('  ❌ '+n+' — '+e.message);fail++} };

  // 等 JS 异步加载完概览数据
  await p.waitForFunction(() => {
    const g=document.getElementById('overviewGrid');
    return g&&g.querySelectorAll('.card-stat').length>0;
  }, {timeout:20000}).catch(()=>{});
  console.log('📄 概览已加载');

  for (const t of ['overview','nodes','canary','config','lua','plugins','audit','minio','etcd'])
    await T(t, async () => { await p.click('#navTabs button[data-panel=\"'+t+'\"]'); await p.waitForTimeout(400); });

  await T('节点详情弹窗', async () => {
    await p.click('#navTabs button[data-panel=\"nodes\"]');
    await p.waitForSelector('#nodesGrid .node-card', {timeout:10000});
    await p.locator('#nodesGrid .node-card button:has-text(\"详情\")').first().click();
    await p.waitForSelector('.modal-overlay', {timeout:3000});
    await p.locator('.modal-overlay .confirm-btn').click();
  });

  await T('etcd搜索', async () => {
    await p.click('#navTabs button[data-panel=\"etcd\"]');
    await p.waitForTimeout(500);
    await p.fill('#etcdPrefix', '/thunder/canary/');
    await p.click('#etcdBrowse');
    await p.waitForTimeout(500);
    await p.fill('#etcdPrefix', '/thunder/');
    await p.click('#etcdBrowse');
  });

  await T('存储卡片', async () => {
    await p.click('#navTabs button[data-panel=\"overview\"]');
    await p.waitForSelector('#storageCard', {state:'visible',timeout:10000});
    const t=await p.locator('#storageTotal').textContent();
    if(t==='-'||!t)throw new Error('total='+t);
    console.log('  💾 '+t+' / 剩余 '+await p.locator('#storageFree').textContent());
  });

  console.log('');
  console.log('═══════════════════');
  console.log('  通过: '+pass+'  失败: '+fail);
  console.log('═══════════════════');
  await b.close();
  process.exit(fail?1:0);
})().catch(e=>{console.error(e.message);process.exit(1)});
"
RESULT=$?
kill $BROWSER 2>/dev/null
echo ""
[ $RESULT -eq 0 ] && echo "🎉 全部通过 ✅" || echo "❌ 有失败"
exit $RESULT
