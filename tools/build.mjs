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
import { mkdir, copyFile, readFile, readdir, rename, rm, writeFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// The build stages into a scratch directory and swaps it into place only once
// it has finished. It used to `rm -rf dist` first, so any failure partway
// through left a half-written artifact where a working one had been -- and the
// next thing to touch it, a test or a deploy, would find a broken dist and
// assume the build had produced it.
const OUT = path.join(ROOT, '.dist-staging');

/**
 * Extra runtime files to ship, on top of what the graph walk finds.
 *
 * A hand-maintained list of "every module the page imports" was here, kept
 * explicit rather than globbed so a stray test file could not ship. It rotted:
 * it listed gltf.mjs, lod.mjs and interiors.mjs, none of which anything imports
 * from index.html, so the build was shipping three dead modules and -- because
 * ASSET_FILES was declared but never read -- 28 MB of model payload for them.
 * The walk below derives the list instead, and an entry here that the walk does
 * not reach is reported rather than trusted.
 */
const EXTRA_FILES = [];

/**
 * Directories copied wholesale, as [source relative to ROOT, destination].
 *
 * The baked material set is a directory rather than a list of 79 names
 * precisely so that adding a material does not mean editing the build: the
 * manifest is derived from what the bake produced, and the dist-boot test fails
 * if the descriptor the game imports is not among the files that shipped.
 */
// The generated model set is listed by name rather than swept: it is a single
// 28 MB file, and a glob that picked up its neighbours would ship the whole
// 78 MB of raw source geometry with it. A missing entry is a 404 on the first
// frame that tries to furnish a room, so the build checks for it explicitly.
// There is no ASSET_FILES list any more. There was one, and it was declared but
// never read -- the build reported success while omitting the file, and a later
// copy loop made it ship 28 MB of model payload that no reachable module
// imports. walkModuleGraph covers both directions: a module the page needs and
// does not have fails the build, and a module nothing needs is not shipped.
/**
 * The specifiers a source file imports.
 *
 * Deliberately a little over-eager: a false positive ships a file that nothing
 * loads, while a false negative omits one the page needs, and only one of those
 * is a bug that reaches a player. So the patterns are broad -- static imports,
 * re-exports, side-effect imports and `import(...)` -- rather than a full
 * parser, which for a tree with no bundler and no transform step is a lot of
 * machinery to own.
 */
function specifiersOf(source) {
  // Comments come out first, and that is not a nicety. A doc comment reading
  // `nothing distinguishes "converged" from "pinned"` contains the word "from"
  // followed by a quoted string, which is exactly the shape of an import
  // statement, and matching it turned budget.mjs into a parse error about a
  // bare specifier named "pinned". Prose that reads like syntax is the hazard;
  // deleting the prose is cheaper than teaching the matcher about it.
  const code = source
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/^[ \t]*\/\/.*$/gm, ' ');

  const found = new Set();
  const patterns = [
    // `import 'x'`, `import a from 'x'`, `import {a,\n b} from 'x'`,
    // `export ... from 'x'`. The clause may not cross a statement end, a quote
    // or a newline followed by a comment marker, so it cannot run away.
    /\b(?:import|export)\s+(?:[^;'"]*?\sfrom\s*)?['"]([^'"]+)['"]/g,
    /\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g,
  ];
  for (const pattern of patterns) {
    for (const m of code.matchAll(pattern)) found.add(m[1]);
  }
  return found;
}

/**
 * Walk the module graph from index.html and return every project file it needs.
 *
 * This is what the build ships, and it replaces a hand-maintained list that had
 * rotted in both directions at once: it named three modules nothing imports
 * (gltf.mjs, lod.mjs, interiors.mjs), and because a separate list named the
 * 28 MB model payload, the build shipped all of it for code that never loads.
 *
 * A specifier the graph cannot resolve is a hard error, not a warning. A missing
 * module is a blank page in the browser, discovered by a player rather than by
 * a build.
 */
function walkModuleGraph(entry, importMap) {
  const shipped = new Set();
  const queue = [entry];
  // Keys, not values: `import '@dimforge/...'` is resolved by looking the
  // specifier up in the map, and the entry it finds is the path to copy.
  const mapped = new Set(Object.keys((importMap && importMap.imports) || {}));

  while (queue.length) {
    const rel = path.normalize(queue.shift());
    if (shipped.has(rel)) continue;
    const abs = path.join(ROOT, rel);
    let source;
    try {
      source = readFileSync(abs, 'utf8');
    } catch (err) {
      // Only ENOENT is "the file is missing". A catch-all here once reported a
      // typo in this very function as a missing index.html, which sends the
      // reader looking in the repository instead of at the build script.
      if (err.code !== 'ENOENT') throw err;
      throw new Error(
        `the shipped page needs ${rel}, which does not exist. ` +
        `A missing module is a blank page at runtime, not a build warning.`
      );
    }
    shipped.add(rel);

    if (rel.endsWith('.html')) {
      // Not just './'-prefixed. The page's own entry is `src="game3d.js"`, and
      // an earlier version of this required a './', which found exactly one file
      // reachable and shipped a dist with no game in it -- while still reporting
      // success, which is the whole failure this walk exists to prevent.
      for (const m of source.matchAll(/(?:src|href|import)=["']([^"']+)["']/g)) {
        const target = m[1];
        if (/^(?:[a-z]+:|\/\/|#|data:)/i.test(target)) continue; // absolute or off-page
        queue.push(target.replace(/^\.\//, ''));
      }
      continue;
    }
    for (const spec of specifiersOf(source)) {
      if (spec.startsWith('.')) {
        queue.push(spec.replace(/^\.\//, ''));
      } else if (spec.startsWith('node:')) {
        continue;
      } else if (mapped.has(spec)) {
        continue; // Resolved by the import map; VENDOR copies it.
      } else {
        throw new Error(
          `${rel} imports the bare specifier "${spec}", which the import map does not ` +
          `resolve. Browsers cannot load one without a mapping.`
        );
      }
    }
  }
  return shipped;
}

const ASSET_DIRS = [['assets/textures', 'assets/textures']];

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

  // Derived, not declared: see walkModuleGraph. An EXTRA_FILES entry the walk
  // never reaches is almost always a module that died and left its name behind,
  // so it is a warning rather than a silent copy.
  const graph = walkModuleGraph('index.html', importMap);
  for (const extra of EXTRA_FILES) {
    if (!graph.has(extra)) {
      console.warn(`warning: EXTRA_FILES entry ${extra} is not reachable from index.html`);
    }
  }
  const files = [...graph, ...EXTRA_FILES.filter((f) => graph.has(f))].sort();

  const emitted = [];
  for (const file of files) {
    const to = path.join(OUT, file);
    // The graph reaches into assets/textures, so the destination can be nested
    // under a directory the copy of that directory has not made yet.
    await mkdir(path.dirname(to), { recursive: true });
    await copyFile(path.join(ROOT, file), to);
    emitted.push(file);
  }
  console.log(`module graph: ${files.length} files reachable from index.html`);
  for (const [from, to] of ASSET_DIRS) {
    const source = path.join(ROOT, from);
    const names = await readdir(source);
    if (!names.includes('materials.mjs')) {
      throw new Error(`${from} has no materials.mjs — the game's static import of the descriptor would 404`);
    }
    await mkdir(path.join(OUT, to), { recursive: true });
    for (const name of names) {
      await copyFile(path.join(source, name), path.join(OUT, to, name));
      emitted.push(`${to}/${name}`);
    }
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

  // Everything is written and the build has not thrown. Swap.
  //
  // rename() onto an existing directory fails with ENOTEMPTY on Linux, so the
  // old artifact is moved aside first and only removed once the new one is in
  // place. Between those two renames there is no dist/ at all, which is why the
  // staging directory is in the same parent: rename is only atomic within a
  // filesystem, and a copy would be atomic nowhere.
  const FINAL = path.join(ROOT, 'dist');
  const PREVIOUS = path.join(ROOT, '.dist-previous');
  await rm(PREVIOUS, { recursive: true, force: true });
  let hadPrevious = false;
  try {
    await rename(FINAL, PREVIOUS);
    hadPrevious = true;
  } catch (err) {
    if (err.code !== 'ENOENT') throw err; // no dist/ yet: a first build
  }
  await rename(OUT, FINAL);
  if (hadPrevious) await rm(PREVIOUS, { recursive: true, force: true });

  process.stdout.write(`EMERGENT build: ${emitted.length + 1} files -> dist/\n`);
  for (const file of [...emitted, 'build.json']) process.stdout.write(`  ${file}\n`);
}

build().catch(async err => {
  process.stderr.write(`build failed: ${err.message}\n`);
  // Drop the partial staging directory, so the next run starts clean and no
  // half-written tree is left lying around looking like an artifact.
  await rm(path.join(ROOT, '.dist-staging'), { recursive: true, force: true });
  process.exit(1);
});
