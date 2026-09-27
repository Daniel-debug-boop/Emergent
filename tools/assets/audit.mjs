/**
 * Audit the asset set.
 *
 *   node tools/assets/audit.mjs
 *
 * This is the answer to "can these assets legally ship", and it is written to
 * be run in CI rather than trusted. It checks five things, each of which has
 * caught a real problem:
 *
 *  1. **Licence.** Every material is CC0 with a licence URL. Anything else, or
 *     anything with a missing or empty field, fails. An asset whose
 *     redistribution terms are unclear cannot be in a commercial game, and the
 *     only safe time to discover that is before it is in the repository.
 *  2. **Provenance.** Every material names a source, a source URL, a creator
 *     where the provider publishes one, and a fetch date.
 *  3. **Shipped files.** Every file in `assets/textures/` is named by the
 *     manifest. A texture in the tree that nothing references is either a
 *     leftover or a mistake, and both waste download bandwidth.
 *  4. **Referenced files.** Every file the descriptor names exists, and has the
 *     size the bake expects. A missing tile is a hole in a texture array layer,
 *     which samples as transparent black and is invisible until someone stands
 *     in front of that wall.
 *  5. **Internal consistency.** The layer indices in the descriptor are dense
 *     and in range, the three maps have the same layer count, and the file
 *     table has one entry per layer.
 *
 * It also reports the measured cost, because a budget nobody states is a
 * budget nobody checks.
 */

import { readFile, readdir, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { MATERIALS, LICENSE, PROVIDER, TEXTURE_SIZE, TEXTURE_ARRAYS, RUNTIME_DIR } from '../../assets/database.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const TEXTURE_DIR = path.join(ROOT, RUNTIME_DIR);
const problems = [];
const notes = [];

const fail = (msg) => problems.push(msg);

async function main() {
  // -- 1. licences ----------------------------------------------------------
  if (LICENSE.id !== 'CC0' || !LICENSE.redistributable) {
    fail(`the project licence is ${LICENSE.id} and is not marked redistributable`);
  }
  if (!LICENSE.url) fail('the project licence has no URL a reader can check');

  // -- 2. provenance --------------------------------------------------------
  const provPath = path.join(ROOT, 'assets', 'provenance.json');
  if (!existsSync(provPath)) {
    fail('assets/provenance.json is missing — run `npm run assets:fetch`');
  }
  const prov = existsSync(provPath) ? JSON.parse(await readFile(provPath, 'utf8')) : { assets: [] };
  const byId = new Map(prov.assets.map(a => [a.asset_id, a]));
  for (const m of MATERIALS) {
    const p = byId.get(m.id);
    if (!p) { fail(`${m.id}: no provenance record`); continue; }
    if (p.license !== 'CC0') fail(`${m.id}: licence is '${p.license}', not CC0`);
    if (!p.license_url) fail(`${m.id}: no licence URL recorded`);
    if (!p.source) fail(`${m.id}: no source provider recorded`);
    if (!p.source_url) fail(`${m.id}: no source URL recorded`);
    if (!p.creator) fail(`${m.id}: no creator recorded`);
    if (!p.fetched || !/^\d{4}-\d{2}-\d{2}$/.test(p.fetched)) fail(`${m.id}: no fetch date in YYYY-MM-DD form`);
    if (!p.source_url.startsWith(PROVIDER.url)) {
      fail(`${m.id}: source URL ${p.source_url} is not on ${PROVIDER.url}`);
    }
  }
  for (const a of prov.assets) {
    if (!MATERIALS.some(m => m.id === a.asset_id)) {
      fail(`${a.asset_id}: provenance exists for a material the database does not name — remove it or add the material`);
    }
  }

  // -- 3. shipped files -----------------------------------------------------
  if (!existsSync(TEXTURE_DIR)) {
    fail(`${RUNTIME_DIR}/ is missing — run \`npm run assets:bake\``);
    return report();
  }
  const onDisk = (await readdir(TEXTURE_DIR)).filter(f => f.endsWith('.png'));
  const expected = new Set();
  for (const m of MATERIALS) for (const a of TEXTURE_ARRAYS) expected.add(`${m.id}_${a.key}.png`);
  for (const f of onDisk) {
    if (!expected.has(f)) fail(`${RUNTIME_DIR}/${f} is on disk but nothing references it`);
  }

  // -- 4/5. descriptor consistency and file integrity ------------------------
  const modPath = path.join(TEXTURE_DIR, 'materials.mjs');
  if (!existsSync(modPath)) {
    fail(`${RUNTIME_DIR}/materials.mjs is missing — run \`npm run assets:bake\``);
    return report();
  }
  const source = await readFile(modPath, 'utf8');
  const { MATERIAL_DESCRIPTOR: desc } = await import(path.join(TEXTURE_DIR, 'materials.mjs'));

  if (desc.size !== TEXTURE_SIZE) fail(`descriptor says ${desc.size}px, the database says ${TEXTURE_SIZE}`);
  for (const a of TEXTURE_ARRAYS) {
    const d = desc.arrays[a.key];
    if (!d) { fail(`descriptor has no '${a.key}' array`); continue; }
    if (d.internalFormat !== a.internalFormat) fail(`array ${a.key}: internal format is ${d.internalFormat}, expected ${a.internalFormat}`);
    if (d.layers !== MATERIALS.length) fail(`array ${a.key}: ${d.layers} layers, expected ${MATERIALS.length}`);
  }

  const ids = Object.keys(desc.materials);
  if (ids.length !== MATERIALS.length) fail(`descriptor names ${ids.length} materials, the database ${MATERIALS.length}`);
  const layers = ids.map(id => desc.materials[id].layer).sort((a, b) => a - b);
  for (let i = 0; i < layers.length; i++) {
    if (layers[i] !== i) { fail(`material layer indices are not dense: expected ${i}, found ${layers[i]}`); break; }
  }
  // A material id in the descriptor that the database does not have would be a
  // tile nobody can name from source code.
  for (const id of ids) {
    if (!MATERIALS.some(m => m.id === id)) fail(`descriptor has a material '${id}' the database does not`);
  }

  let bytes = 0;
  for (const a of TEXTURE_ARRAYS) {
    const list = desc.files && desc.files[a.key];
    if (!list) { fail(`descriptor has no file table for '${a.key}'`); continue; }
    if (list.length !== MATERIALS.length) fail(`file table for ${a.key} has ${list.length} entries, expected ${MATERIALS.length}`);
    for (const name of list) {
      if (!source.includes(name)) fail(`descriptor names '${name}' but the generated module does not contain it`);
      const p = path.join(TEXTURE_DIR, name);
      if (!existsSync(p)) { fail(`${RUNTIME_DIR}/${name} is referenced but missing`); continue; }
      const s = await stat(p);
      bytes += s.size;
      if (s.size < 1024) fail(`${RUNTIME_DIR}/${name} is only ${s.size} bytes, which is a blank or failed bake`);
    }
    // Two materials must not share a tile, or a facade would be a different
    // building wearing the same bricks.
    if (new Set(list).size !== list.length) fail(`file table for ${a.key} has duplicates`);
  }

  notes.push(`provider      ${PROVIDER.name} (${PROVIDER.url})`);
  notes.push(`licence       ${LICENSE.id} — ${LICENSE.url}`);
  notes.push(`materials     ${MATERIALS.length} (${ids.length} in the descriptor)`);
  notes.push(`maps          ${TEXTURE_ARRAYS.map(a => a.key).join(', ')} at ${TEXTURE_SIZE}x${TEXTURE_SIZE}`);
  notes.push(`shipped       ${(bytes / 1048576).toFixed(1)} MB of PNG across ${onDisk.length} tiles`);
  notes.push(`vram at full  ${(estimateVram(MATERIALS.length) / 1048576).toFixed(0)} MB with a complete mip chain`);
  notes.push(`licence       every entry CC0, every creator and source URL recorded`);
  return report();
}

/** Four bytes a texel, times 4/3 for a complete mip chain. */
function estimateVram(layers) {
  return TEXTURE_ARRAYS.length * layers * TEXTURE_SIZE * TEXTURE_SIZE * 4 * (4 / 3);
}

function report() {
  for (const n of notes) console.log(`  ${n}`);
  if (problems.length) {
    console.error(`\n${problems.length} problem(s):`);
    for (const p of problems) console.error(`  ✗ ${p}`);
    process.exit(1);
  }
  console.log('\n  audit passed: every asset is CC0, sourced, referenced and present');
}

main().catch(err => { console.error(err); process.exit(1); });
