// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { readFile, writeFile, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';

const root = new URL('../../', import.meta.url);
const directory = new URL('.tmp/onchip-beta-wifi/', root);
const { chromium } = await import(new URL('browser-tools/node_modules/playwright-core/index.mjs', directory));
const address = JSON.parse(await readFile(new URL('mast-online.json', directory))).ip;
const passwordPath = new URL('.tmp/onchip-beta-lab/password', root);
assert.equal((await stat(passwordPath)).mode & 0o077, 0);
const password = await readFile(passwordPath, 'utf8');
const browser = await chromium.launch({
  executablePath: '/usr/bin/google-chrome',
  headless: true,
  args: ['--disable-background-networking', '--disable-component-update', '--no-proxy-server'],
});
let page;
let original;
let changed = false;
let failure;
const digest = source => createHash('sha256').update(source).digest('hex');
const command = async text => page.evaluate(text => cmd(text), text);
const waitActive = async expected => {
  for (let attempt = 0; attempt < 30; attempt++) {
    const status = await command('source status');
    assert(!status.includes('Error:'), 'source activation failed: ' + status);
    if (status.includes('durably saved and active') &&
        (await command('source hash')).startsWith(`SHA256 ${expected} `)) return status;
    await delay(500);
  }
  throw Error('On-device activation/hash did not reach expected state');
};
try {
  const context = await browser.newContext({ serviceWorkers: 'block' });
  await context.route('**/*', route => {
    if (new URL(route.request().url()).hostname === address) return route.continue();
    return route.abort();
  });
  page = await context.newPage();
  const response = await page.goto(`http://${address}/admin`, { waitUntil: 'domcontentloaded' });
  assert.equal(response.status(), 200);
  assert((await response.headerValue('content-security-policy')).includes("connect-src 'self'"));
  const rejected = await page.evaluate(async () => {
    const response = await fetch('/admin/command', { method: 'POST', body: 'status' });
    return response.status;
  });
  assert.equal(rejected, 403);
  await page.locator('#password').fill(password);
  await page.locator('#login').click();
  await page.waitForFunction(() => document.querySelector('#result').textContent.startsWith('roles applied='),
                             null, { timeout: 15000 });
  assert.equal(await page.locator('#password').inputValue(), '');
  await page.locator('#read').click();
  await page.waitForFunction(() => document.querySelector('#source').value.length > 0,
                             null, { timeout: 20000 });
  original = await page.locator('#source').inputValue();
  const initialHash = await command('source hash');
  assert(initialHash.startsWith(`SHA256 ${digest(original)} `));
  await page.locator('#hello').click();
  const edited = await page.locator('#source').inputValue();
  assert.equal(edited, "function hello(name) reply('Hello '..name) end");
  assert.notEqual(edited, original);
  await page.locator('#source').fill(edited);
  changed = true;
  await page.locator('#install').click();
  await page.waitForFunction(() => document.querySelector('#result').textContent.includes('durably saved and active') ||
                                  document.querySelector('#result').textContent.includes('Error:'),
                             null, { timeout: 30000 });
  const installed = await waitActive(digest(edited));
  await page.locator('#source').fill('');
  await page.locator('#read').click();
  await page.waitForFunction(() => document.querySelector('#source').value.length > 0,
                             null, { timeout: 20000 });
  assert.equal(await page.locator('#source').inputValue(), edited);
  const rf = await promisify(execFile)('make', ['-s', '-C', fileURLToPath(new URL('./', import.meta.url)),
                                               'beta-hello-test'], { timeout: 45000 });
  assert(rf.stdout.includes('PASS physical installed hello'));
  await page.locator('[data-command="source rollback"]').click();
  const rolledBack = await waitActive(digest(original));
  changed = false;
  await page.locator('#logout').click();
  await page.waitForFunction(() => document.querySelector('#result').textContent === 'Logged out');
  assert.equal(await page.evaluate(async () => (await fetch('/admin/command',
    { method: 'POST', body: 'status' })).status), 403);
  await writeFile(new URL('web-results.json', directory), JSON.stringify({
    browser: 'system Chrome headless, actual mast /admin page',
    unauthenticated: 403, login: true, password_field_cleared: true,
    source_bytes: Buffer.byteLength(edited), source_sha256: digest(edited),
    installed, exact_readback: true, hello_rf_reply: true, rolledBack, logout: true,
  }, null, 2) + '\n', { mode: 0o600 });
  console.log('PASS physical Chrome /admin: login, named hello install, exact readback, RF reply, rollback and logout');
} catch (error) {
  failure = error;
  await writeFile(new URL('web-failure.json', directory), JSON.stringify({
    stage: 'physical on-device editor', error: error.message,
  }) + '\n', { mode: 0o600 });
  throw error;
} finally {
  try {
    if (changed && original && page) {
      try {
        if (!(await command('source hash')).startsWith(`SHA256 ${digest(original)} `))
          await command('source rollback');
        await waitActive(digest(original));
      } catch (error) {
        if (!failure) throw error;
        console.error('Additional source restoration failure: ' + error.message);
      }
    }
    if (page) await page.evaluate(async () => {
      if (token) await fetch('/admin/logout', { method: 'POST', headers: { 'X-Mast-Session': token } });
    });
  } finally {
    await browser.close();
  }
}
