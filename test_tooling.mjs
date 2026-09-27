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
import { existsSync } from 'node:fs';
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
  // `docs/` is checked into the repo and has no index.html, so this holds on a
  // clean checkout. Deliberately not `dist/`: that directory only exists after
  // the build test runs, and asserting on it would make this order-dependent.
  assert.equal(await resolveFile('/docs'), null, 'a directory without index.html is not served');
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

/**
 * Walk a module graph, independently of the build's own walker.
 *
 * Deliberately a second implementation. If this imported the build's, a bug in
 * the walker would be invisible here, which is the failure mode this file is
 * specifically looking for.
 */
function specifiers(source) {
  const code = source
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/^[ \t]*\/\/.*$/gm, ' ');
  const found = new Set();
  for (const m of code.matchAll(/\b(?:import|export)\s+(?:[^;'"]*?\sfrom\s*)?['"]([^'"]+)['"]/g)) found.add(m[1]);
  for (const m of code.matchAll(/\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g)) found.add(m[1]);
  return found;
}

async function reachableFrom(baseDir, entry) {
  const shipped = new Set();
  const queue = [entry];
  while (queue.length) {
    const rel = path.normalize(queue.shift());
    if (shipped.has(rel)) continue;
    let source;
    try {
      source = await readFile(path.join(baseDir, rel), 'utf8');
    } catch {
      throw new Error(`${rel} is reachable from ${entry} but not present`);
    }
    shipped.add(rel);
    if (rel.endsWith('.html')) {
      for (const m of source.matchAll(/(?:src|href|import)=["']([^"']+)["']/g)) {
        if (/^(?:[a-z]+:|\/\/|#|data:)/i.test(m[1])) continue;
        queue.push(m[1].replace(/^\.\//, ''));
      }
      continue;
    }
    for (const spec of specifiers(source)) {
      if (spec.startsWith('.')) queue.push(spec.replace(/^\.\//, ''));
    }
  }
  return shipped;
}

await test('the shipped dist is self-contained: every import inside it resolves', async () => {
  // The failure this catches is a build that reports success and emits a
  // directory the browser cannot load. Nothing in the build used to notice,
  // because nothing checked the artifact against itself.
  const dist = path.join(ROOT, 'dist');
  const needed = await reachableFrom(dist, 'index.html');
  assert.ok(needed.size >= 12, `expected a real module graph, walked ${needed.size}`);
  for (const rel of needed) {
    const info = await stat(path.join(dist, rel));
    assert.ok(info.size > 0, `dist/${rel} is reachable and must not be empty`);
  }
});

await test('the build ships nothing the page cannot reach', async () => {
  const dist = path.join(ROOT, 'dist');
  const needed = await reachableFrom(dist, 'index.html');
  const manifest = JSON.parse(await readFile(path.join(dist, 'build.json'), 'utf8'));
  // assets/textures and build.json are copied as directories and as a stamp, so
  // they are legitimately not reachable through an import statement.
  const structural = new Set(['build.json', 'vendor/rapier.mjs']);
  const dead = [];
  for (const file of manifest.files) {
    if (structural.has(file) || file.startsWith('assets/textures/')) continue;
    if (!needed.has(file)) dead.push(file);
  }
  assert.deepEqual(dead, [], `unreachable files shipped: ${dead.join(', ')}`);
});

await test('the 28 MB model payload is not shipped for code nothing imports', async () => {
  // interiors.mjs is not reachable from the page, so the generated model set it
  // needs is not either. The build used to ship it unconditionally, which is 28
  // MB of base64 downloaded by every player for zero function.
  const dist = path.join(ROOT, 'dist');
  const manifest = JSON.parse(await readFile(path.join(dist, 'build.json'), 'utf8'));
  assert.ok(!manifest.files.includes('assets/models.gen.mjs'),
    'nothing imports the payload, so the build must not ship it');
  // The accessor stays committed, because the test suite and any future
  // interior system need the format without the 28 MB.
  const stat_ = await stat(path.join(ROOT, 'assets', 'models.index.mjs'));
  assert.ok(stat_.size > 0 && stat_.size < 64 * 1024, 'the committed accessor is small');
});

await test('a module the page needs but does not have fails the build', async () => {
  // The check is worthless unless it fails, so make it fail: rename a module the
  // graph needs and require a non-zero exit naming the file.
  const victim = path.join(ROOT, 'math3d.mjs');
  const hidden = `${victim}.hidden-for-test`;
  const { rename } = await import('node:fs/promises');
  await rename(victim, hidden);
  try {
    const code = await new Promise((resolve) => {
      const proc = spawn(process.execPath, [path.join(ROOT_DIR, 'tools', 'build.mjs')]);
      let out = '';
      proc.stdout.on('data', (d) => (out += d));
      proc.stderr.on('data', (d) => (out += d));
      proc.on('exit', resolve);
    });
    assert.notEqual(code, 0, 'the build must fail when a reachable module is missing');
  } finally {
    await rename(hidden, victim);
  }
  // And it must have left the previous artifact alone. It used to `rm -rf dist`
  // first, so the failure destroyed a working build and the next thing to look
  // at dist -- a test, a deploy, a developer -- found a broken tree and assumed
  // the build had produced it.
  const after = JSON.parse(await readFile(path.join(ROOT, 'dist', 'build.json'), 'utf8'));
  assert.ok(after.files.includes('game3d.js'),
    'the previous dist must survive a failed build, not be left half-written');
  assert.ok(!existsSync(path.join(ROOT, '.dist-staging')), 'no partial staging tree is left behind');
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
