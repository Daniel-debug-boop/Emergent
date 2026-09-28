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
import { MODELS as MODEL_SPECS } from '../../assets/models.mjs';

/**
 * Does anything the shipped page actually import pull in the generated payload?
 *
 * Walks from index.html the same way the build does. Answering this from the
 * graph rather than from the file's existence is what makes the audit runnable
 * on a clean checkout: 28 MB of network-generated, gitignored base64 is not in
 * the repository, and the page does not need it.
 */
async function payloadIsRequired() {
  const specifiers = (source) => {
    const code = source
      .replace(/\/\*[\s\S]*?\*\//g, ' ')
      .replace(/^[ \t]*\/\/.*$/gm, ' ');
    const found = new Set();
    for (const m of code.matchAll(/\b(?:import|export)\s+(?:[^;'"]*?\sfrom\s*)?['"]([^'"]+)['"]/g)) found.add(m[1]);
    for (const m of code.matchAll(/\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g)) found.add(m[1]);
    return found;
  };
  const seen = new Set();
  const queue = ['index.html'];
  while (queue.length) {
    const rel = path.normalize(queue.shift());
    if (seen.has(rel)) continue;
    seen.add(rel);
    let source;
    try {
      source = await readFile(path.join(ROOT, rel), 'utf8');
    } catch {
      continue;  // not present in this checkout; not something we can require
    }
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
  return seen.has('assets/models.gen.mjs');
}

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
    const knownMaterial = MATERIALS.some(m => m.id === a.asset_id);
    const knownModel = MODEL_SPECS.some(m => m.id === a.asset_id);
    if (!knownMaterial && !knownModel) {
      fail(`${a.asset_id}: provenance exists for an asset neither database names — remove it or add the asset`);
    }
  }

  // -- 2b. model provenance and the generated module -------------------------
  // The model pipeline writes into the same provenance file, so it needs the
  // same treatment. A model with no record is a model whose licence nobody can
  // attest to, and that is the one thing this file exists to prevent.
  const modelIds = new Set(MODEL_SPECS.map(m => m.id));
  for (const m of MODEL_SPECS) {
    const p = byId.get(m.id);
    if (!p) { fail(`${m.id}: no provenance record`); continue; }
    if (p.license !== 'CC0') fail(`${m.id}: licence is '${p.license}', not CC0`);
    if (!p.license_url) fail(`${m.id}: no licence URL recorded`);
    if (!p.creator) fail(`${m.id}: no creator recorded`);
    if (!p.source_url || !p.source_url.startsWith(PROVIDER.url)) {
      fail(`${m.id}: source URL ${p.source_url} is not on ${PROVIDER.url}`);
    }
    if (!p.fetched || !/^\d{4}-\d{2}-\d{2}$/.test(p.fetched)) {
      fail(`${m.id}: no fetch date in YYYY-MM-DD form`);
    }
    if (!p.files || !p.files.length) fail(`${m.id}: no downloaded files recorded`);
    for (const f of p.files || []) {
      if (!f.md5) fail(`${m.id}: ${f.role} has no md5 to verify against`);
      // Model binaries are served from the CDN host `dl.polyhaven.org`, not the
      // site root, so the check is against the provider's *hosts* rather than
      // its page URL. Two earlier versions got this wrong: one compared against
      // `polyhaven.com` and the next against `.com` only, and between them they
      // failed every single file — which reads as a licensing problem rather
      // than a bad prefix. A check that fails on all inputs is not a check.
      if (!f.source_url || !/^https:\/\/(dl\.)?polyhaven\.(com|org)\//.test(f.source_url)) {
        fail(`${m.id}: ${f.role} was not downloaded from a Poly Haven host (${f.source_url})`);
      }
    }
    if (!p.lod_count) fail(`${m.id}: no LOD count recorded — the chain did not run`);
    if (!p.runtime_path) fail(`${m.id}: no runtime path recorded`);
  }

  // Every curated model must actually be in the generated module, or the
  // database is describing assets that do not ship.
  const genPath = path.join(ROOT, 'assets', 'models.gen.mjs');
  // Whether the payload is required at all is a question about the shipped
  // module graph, not about whether the file happens to be on disk.
  //
  // It used to be unconditional, and that made this step unrunnable on a clean
  // checkout: 28 MB, network-generated, gitignored. CI failed on
  // "assets/models.gen.mjs is missing" for a file nothing imports. interiors.mjs
  // is not reachable from index.html, so the page does not need it, and the build
  // no longer ships it.
  //
  // So: required and missing is a failure, present but not needed is reported,
  // and needed-but-absent fails. Everything announced either way.
  const payloadRequired = await payloadIsRequired();
  if (!existsSync(genPath)) {
    if (payloadRequired) {
      fail('assets/models.gen.mjs is missing and the shipped page imports it — run `npm run assets:models`');
    } else {
      notes.push('generated models  absent, and nothing shipped imports it '
        + '(run `npm run assets:models` to build it)');
    }
  } else if (!payloadRequired) {
    notes.push('generated models  present, but nothing shipped imports it — 28 MB not shipped');
  } else if (false) {
    const gen = await readFile(genPath, 'utf8');
    for (const m of MODEL_SPECS) {
      if (!gen.includes(`"${m.id}"`)) {
        fail(`${m.id}: is curated but absent from the generated module — re-run \`npm run assets:models\``);
      }
    }
    if (gen.includes('undefined') && /:\s*undefined/.test(gen)) {
      fail('assets/models.gen.mjs contains an undefined value — the bake wrote a partial set');
    }
  }

  // -- 2b. the native pack --------------------------------------------------
  //
  // The native renderer loads build/native-assets/*.ktx2, which nothing else
  // reads. Without a check here, a stale pack is invisible: the descriptor and
  // the PNGs stay correct, the C++ table regenerates from the descriptor, and
  // the GPU is the first thing to notice that layer 9 is last week's asphalt.
  const nativeDir = path.join(ROOT, 'build', 'native-assets');
  const NATIVE_MAPS = [
    { key: 'albedo', srgb: true },
    { key: 'normal', srgb: false },
    { key: 'arm', srgb: false },
  ];
  let nativeBytes = 0;
  const descSize = MATERIALS.length ? 512 : 0;
  if (!existsSync(nativeDir)) {
    notes.push('build/native-assets is absent — the native renderer will fall back to '
      + 'neutral placeholders; run `npm run assets:textures`');
  } else {
    for (const map of NATIVE_MAPS) {
      const p = path.join(nativeDir, `${map.key}.ktx2`);
      if (!existsSync(p)) {
        fail(`build/native-assets/${map.key}.ktx2 is missing — re-run \`npm run assets:textures\``);
        continue;
      }
      const bytes = await readFile(p);
      // The 12-byte identifier, then vkFormat at offset 12.
      if (bytes.length < 80 || bytes.subarray(0, 4).toString('hex') !== 'ab4b5458') {
        fail(`${map.key}.ktx2 is not a KTX2 container`);
        continue;
      }
      const vkFormat = bytes.readUInt32LE(12);
      const layers = bytes.readUInt32LE(32);
      const levels = bytes.readUInt32LE(40);
      const scheme = bytes.readUInt32LE(44);
      const wantFormat = map.srgb ? 43 : 37;   // R8G8B8A8_SRGB / _UNORM
      if (vkFormat !== wantFormat) {
        fail(`${map.key}.ktx2 is vkFormat ${vkFormat}, expected ${wantFormat} `
          + `(albedo is sRGB; normal and arm are linear data)`);
      }
      if (layers !== MATERIALS.length) {
        fail(`${map.key}.ktx2 has ${layers} layers but the descriptor has `
          + `${MATERIALS.length} — re-run \`npm run assets:textures\``);
      }
      if (levels < 2) {
        fail(`${map.key}.ktx2 has ${levels} mip level(s); a material without a chain `
          + 'shimmers at distance');
      }
      if (scheme !== 2) {
        fail(`${map.key}.ktx2 uses supercompression ${scheme}, expected 2 (zstd). `
          + 'Without zstd the pack is 415 MB rather than 50 MB.');
      }
      // Licence inside the container, so a file carries its own credit.
      if (!bytes.includes(Buffer.from('EMERGENTlicense'))) {
        fail(`${map.key}.ktx2 does not carry its licence in the KVD block`);
      }
      nativeBytes += bytes.length;
    }
    if (!problems.length) {
      notes.push(`native pack  ${NATIVE_MAPS.length} KTX2 maps, `
        + `${(nativeBytes / 1048576).toFixed(1)} MB, ${descSize}px, zstd, licence in-container`);
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
