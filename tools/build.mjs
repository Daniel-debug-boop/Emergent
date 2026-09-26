/**
 * EMERGENT production build.
 *
 * The game has no bundler and no third-party runtime dependencies, so the build
 * is a manifest-driven copy into `dist/`. That is deliberate: the shipped
 * bundle is exactly the reviewed source, with no transform step that could
 * diverge from what the tests execute.
 *
 * Anything not listed in FILES is not shipped, so a stray file in the repo root
 * can never leak into a deploy.
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
const FILES = ['index.html', 'game3d.js', 'world.mjs', 'math3d.mjs', 'culling.mjs'];

/** Build-fatal requirement: the HTML must actually load the entry module. */
const ENTRY = /<script[^>]+src=["']game3d\.js["']/;

async function build() {
  const indexHtml = await readFile(path.join(ROOT, 'index.html'), 'utf8');
  if (!ENTRY.test(indexHtml)) {
    throw new Error('index.html does not load game3d.js as a module; the build would ship a blank page');
  }

  await rm(OUT, { recursive: true, force: true });
  await mkdir(OUT, { recursive: true });

  const emitted = [];
  for (const file of FILES) {
    await copyFile(path.join(ROOT, file), path.join(OUT, file));
    emitted.push(file);
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
