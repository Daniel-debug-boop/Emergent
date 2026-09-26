/**
 * EMERGENT production build.
 *
 * The game has no bundler, so the build is a manifest-driven copy into `dist/`.
 * That is deliberate: the shipped bundle is exactly the reviewed source, with
 * no transform step that could diverge from what the tests execute.
 *
 * The one exception is third-party code. Rapier is a WASM physics engine
 * distributed as a single ES module with the WASM inlined as base64, so it is
 * copied verbatim under the URL the page's import map points at. It is not
 * modified, minified or re-bundled, which means the dependency the tests import
 * from `node_modules` and the dependency the browser loads are byte-identical.
 *
 * Anything not listed in FILES or VENDOR is not shipped, so a stray file in the
 * repo root can never leak into a deploy.
 *
 * Usage: `node tools/build.mjs`
 */
import { mkdir, copyFile, readFile, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, 'dist');

/** Runtime files copied verbatim into the build. */
// Every module the shipped page imports. Kept explicit rather than globbed: a
// glob would silently ship test files and scratch scripts, and a missing entry
// is a runtime 404 in the browser that no build step would report.
const FILES = ['index.html', 'game3d.js', 'world.mjs', 'math3d.mjs', 'culling.mjs', 'physics.mjs', 'missions.mjs'];

/**
 * Third-party modules, as [source relative to ROOT, destination in dist].
 *
 * Keyed by the exact path `index.html`'s import map resolves, so a change to one
 * without the other fails the import-map check below instead of shipping a 404
 * that only appears when a player tries to walk.
 */
const VENDOR = [
  ['node_modules/@dimforge/rapier3d-compat/dist/rapier.mjs', 'vendor/rapier.mjs']
];

/** Build-fatal requirement: the HTML must actually load the entry module. */
const ENTRY = /<script[^>]+src=["']game3d\.js["']/;

/** Build-fatal requirement: every vendored path must be mapped by the page. */
const IMPORT_MAP = /<script[^>]+type=["']importmap["']>([\s\S]*?)<\/script>/;

async function build() {
  const indexHtml = await readFile(path.join(ROOT, 'index.html'), 'utf8');
  if (!ENTRY.test(indexHtml)) {
    throw new Error('index.html does not load game3d.js as a module; the build would ship a blank page');
  }

  // A vendored file nothing maps to is a 404 on the first frame, in the
  // browser, on hardware nobody here has. Checked here instead.
  const mapMatch = IMPORT_MAP.exec(indexHtml);
  if (!mapMatch) {
    throw new Error('index.html has no import map, but the build ships vendored third-party modules');
  }
  let importMap;
  try {
    importMap = JSON.parse(mapMatch[1]);
  } catch (err) {
    throw new Error(`index.html import map is not valid JSON: ${err.message}`);
  }
  for (const [, dest] of VENDOR) {
    if (!Object.values(importMap.imports || {}).includes(`./${dest}`)) {
      throw new Error(`index.html import map does not map ${dest}; the browser would 404 on it`);
    }
  }

  await rm(OUT, { recursive: true, force: true });
  await mkdir(OUT, { recursive: true });

  const emitted = [];
  for (const file of FILES) {
    await copyFile(path.join(ROOT, file), path.join(OUT, file));
    emitted.push(file);
  }
  for (const [source, dest] of VENDOR) {
    const from = path.join(ROOT, source);
    const to = path.join(OUT, dest);
    await mkdir(path.dirname(to), { recursive: true });
    await copyFile(from, to);
    emitted.push(dest);
  }

  // A build stamp makes it obvious which artifact a deploy is serving.
  const manifest = {
    name: 'emergent',
    builtAt: new Date().toISOString(),
    files: emitted
  };
  await writeFile(path.join(OUT, 'build.json'), `${JSON.stringify(manifest, null, 2)}\n`);

  process.stdout.write(`EMERGENT build: ${emitted.length + 1} files -> dist/\n`);
  for (const file of [...emitted, 'build.json']) process.stdout.write(`  ${file}\n`);
}

build().catch(err => {
  process.stderr.write(`build failed: ${err.message}\n`);
  process.exit(1);
});
