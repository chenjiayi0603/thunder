import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  snapshotDir: './snapshots',
  expect: { timeout: 10000 },
  reporter: [
    ['html', { outputFolder: './report', open: 'never' }],
    ['list'],
  ],
  use: {
    baseURL: process.env.ADMIN_URL || 'http://127.0.0.1:8090',
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
    launchOptions: {
      args: ['--ozone-platform=wayland', '--no-sandbox'],
    },
  },

  projects: [
    { name: 'chromium', use: { headless: true } },
    { name: 'headed',  use: { headless: false } },
  ],
});

process.env.WAYLAND_DISPLAY = process.env.WAYLAND_DISPLAY || 'wayland-0';
process.env.XDG_RUNTIME_DIR = process.env.XDG_RUNTIME_DIR || '/run/user/1000';
