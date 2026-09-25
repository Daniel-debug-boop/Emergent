/**
 * Tests for the project's own tooling.
 *
 * The preview server and the production build are part of the shipped product:
 * a path-traversal hole or a build that ships a blank page would only ever show
 * up as "the site doesn't work" after deploy. They are cheap to test, so they
 * are tested.
 *
 * Run: `npm run test:tooling`
 */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFile, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { resolveFile, start, ROOT, MIME } from './tools/serve.mjs';
import { setupHeadless, resetHarness, pumpFrames, currentGL } from './tools/headless_runtime.mjs';

const ROOT_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)));
let checks = 0;
let failures = 0;

async function test(name, fn) {
  try {
    await fn();
    checks++;
    process.stdout.write(`✓ ${name}\n`);
  } catch (err) {
    failures++;
    process.stdout.write(`✗ ${name}\n  ${err.message}\n`);
  }
}

await test('resolves the site root to index.html', async () => {
  assert.equal(await resolveFile('/'), path.join(ROOT, 'index.html'));
});

await test('serves the game entry module with a JavaScript content type', async () => {
  const file = await resolveFile('/game3d.js');
  assert.equal(file, path.join(ROOT, 'game3d.js'));
  assert.match(MIME['.js'], /javascript/);
});

await test('rejects path traversal outside the project root', async () => {
  for (const attack of [
    '/../package.json',
    '/../../etc/passwd',
    '/%2e%2e/%2e%2e/etc/passwd',
    '/game3d.js/../../../../etc/passwd',
    '/....//....//etc/passwd'
  ]) {
    const file = await resolveFile(attack);
    assert.equal(file, null, `traversal must not resolve: ${attack}`);
  }
});

await test('returns null for files that do not exist', async () => {
  assert.equal(await resolveFile('/nope.js'), null);
  assert.equal(await resolveFile('/docs'), null, 'a directory without index.html is not served');
  assert.equal(await resolveFile('/dist'), path.join(ROOT, 'dist', 'index.html'),
    'a directory with an index.html is served from that index');
});

await test('the build emits exactly the runtime files the page needs', async () => {
  await new Promise((resolve, reject) => {
    const proc = spawn(process.execPath, [path.join(ROOT_DIR, 'tools', 'build.mjs')], { stdio: 'ignore' });
    proc.on('exit', code => (code === 0 ? resolve() : reject(new Error(`build exited ${code}`))));
    proc.on('error', reject);
  });
  const dist = path.join(ROOT, 'dist');
  for (const file of ['index.html', 'game3d.js', 'world.mjs', 'math3d.mjs', 'build.json']) {
    const info = await stat(path.join(dist, file));
    assert.ok(info.size > 0, `dist/${file} should not be empty`);
  }
  const html = await readFile(path.join(dist, 'index.html'), 'utf8');
  assert.match(html, /<script[^>]+src="game3d\.js"/, 'built HTML must load the entry module');
  const manifest = JSON.parse(await readFile(path.join(dist, 'build.json'), 'utf8'));
  assert.ok(Array.isArray(manifest.files) && manifest.files.includes('game3d.js'));
  assert.ok(manifest.builtAt, 'manifest should record when the build ran');
});

await test('the server returns the page and correct MIME types over HTTP', async () => {
  const server = await start(0, '127.0.0.1');
  const { port } = server.address();
  try {
    const page = await fetch(`http://127.0.0.1:${port}/`);
    assert.equal(page.status, 200);
    assert.match(page.headers.get('content-type'), /text\/html/);
    assert.match(await page.text(), /game-canvas/, 'the page must contain the game canvas');

    const module = await fetch(`http://127.0.0.1:${port}/math3d.mjs`);
    assert.equal(module.status, 200);
    assert.match(module.headers.get('content-type'), /javascript/);
    assert.match(await module.text(), /export function matrixPerspective/);

    const missing = await fetch(`http://127.0.0.1:${port}/does-not-exist.js`);
    assert.equal(missing.status, 404);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
});

await test('the built dist/ artifact boots and renders, not just copies files', async () => {
  // Guards the deploy path: a manifest that omits world.mjs or math3d.mjs would
  // produce a dist/ that copies cleanly and then shows a blank page in production.
  resetHarness();
  setupHeadless({ url: 'http://localhost:8765/index.html' });
  await import(pathToFileURL(path.join(ROOT, 'dist', 'game3d.js')).href + '?dist=1');
  await pumpFrames(45);
  const gl = currentGL();
  assert.ok(gl, 'the built game should create a WebGL2 context');
  assert.equal(gl.stats.errors.length, 0, `built game reported GL errors: ${gl.stats.errors[0]}`);
  assert.equal(gl.stats.nanUploads, 0, 'built game uploaded non-finite vertex data');
  assert.equal(gl.unmodelled.size, 0, `built game used unmodelled GL calls: ${[...gl.unmodelled]}`);
  assert.ok(gl.stats.drawCalls > 0, 'built game issued no draw calls');
  assert.ok(globalThis.EMERGENT.stats.staticVertexCount > 0, 'built game built no static geometry');
});

process.stdout.write(`\n${checks} tooling checks passed, ${failures} failing\n`);
process.exit(failures ? 1 : 0);
