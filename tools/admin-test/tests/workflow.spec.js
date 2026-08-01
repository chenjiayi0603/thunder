import { test, expect } from '@playwright/test';

const BASE = process.env.ADMIN_URL ? '' : '';

// ===== 核心流程自动化 =====

test('流程1: 切换所有面板无报错', async ({ page }) => {
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto('/');
  const tabs = ['overview','nodes','canary','config','lua','plugins','audit','minio','etcd'];
  for (const tab of tabs) {
    await page.click('#navTabs button[data-panel="' + tab + '"]');
    await page.waitForTimeout(600);
  }
  expect(errors.filter(e => !e.includes('MinIO') && !e.includes('404'))).toEqual([]);
});

test('流程2: 概览 → 查看节点详情', async ({ page }) => {
  await page.goto('/');
  // 点第一个服务卡片 → 跳到节点页
  await page.waitForSelector('#overviewGrid .card-stat', { timeout: 10000 });
  await page.locator('#overviewGrid .card-stat').first().click();
  // 验证已切到节点页
  await expect(page.locator('#nodesGrid')).toBeVisible();
  await page.waitForSelector('#nodesGrid .node-card', { timeout: 10000 });
  // 点详情按钮
  const detailBtn = page.locator('#nodesGrid .node-card button:has-text("详情")').first();
  if (await detailBtn.isVisible()) {
    await detailBtn.click();
    await page.waitForSelector('.modal-overlay', { timeout: 3000 });
    await expect(page.locator('.modal-overlay')).toBeVisible();
    // 关闭弹窗
    await page.locator('.modal-overlay .confirm-btn').click();
  }
  await page.screenshot({ path: '/tmp/workflow-nodes.png', fullPage: false });
});

test('流程3: etcd 浏览器查询', async ({ page }) => {
  await page.goto('/');
  await page.click('#navTabs button[data-panel="etcd"]');
  await page.waitForSelector('#etcdTable tr', { timeout: 10000 });
  const rowsBefore = await page.locator('#etcdTable tr').count();
  // 换个前缀搜索
  await page.fill('#etcdPrefix', '/thunder/canary/');
  await page.click('#etcdBrowse');
  await page.waitForTimeout(1000);
  // 至少页面没崩
  await expect(page.locator('#etcdTable')).toBeVisible();
  // 恢复
  await page.fill('#etcdPrefix', '/thunder/');
  await page.click('#etcdBrowse');
  await page.screenshot({ path: '/tmp/workflow-etcd.png', fullPage: false });
});

test('流程4: 节点搜索过滤', async ({ page }) => {
  await page.goto('/');
  await page.click('#navTabs button[data-panel="nodes"]');
  await page.waitForSelector('#nodesGrid .node-card', { timeout: 10000 });
  // 用下拉筛选
  await page.selectOption('#nodeTypeSelect', { index: 1 });
  await page.waitForTimeout(1000);
  // 恢复
  await page.selectOption('#nodeTypeSelect', { index: 0 });
  await page.screenshot({ path: '/tmp/workflow-search.png', fullPage: false });
});

test('流程5: Lua 编辑 → 预览 → 取消', async ({ page }) => {
  await page.goto('/');
  await page.click('#navTabs button[data-panel="lua"]');
  await page.waitForSelector('#luaNodeType', { timeout: 5000 });
  // 选择服务
  const opts = await page.locator('#luaNodeType option').count();
  if (opts > 1) {
    await page.selectOption('#luaNodeType', { index: 1 });
    await page.waitForTimeout(1500);
  }
  // 上传区存在
  await expect(page.locator('#luaDrop')).toBeVisible();
  await page.screenshot({ path: '/tmp/workflow-lua.png', fullPage: false });
});

test('流程6: 存储卡片数据完整', async ({ page }) => {
  await page.goto('/');
  // 概览页
  await page.waitForSelector('#storageCard', { state: 'visible', timeout: 10000 });
  // 每个指标都有值
  for (const id of ['storageTotal','storageUsed','storageFree','storageObjCount']) {
    const text = await page.locator('#' + id).textContent();
    expect(text).not.toBe('-');
    expect(text).not.toBe('');
  }
  // 健康标签
  const health = await page.locator('#storageHealth').textContent();
  expect(health).toBe('正常');
  // 进度条
  const bar = page.locator('#storageBar');
  await expect(bar).toBeVisible();
  await page.screenshot({ path: '/tmp/workflow-storage.png', fullPage: false });
});
