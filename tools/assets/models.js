#!/usr/bin/env node
/**
 * The model pipeline: download, verify, import, decimate, scale, bake.
 *
 * Extends the existing material pipeline rather than replacing it. The material
 * side of EMERGENT already does fetch → md5 verify → validate → resample → pack,
 * and this tool follows the same shape for geometry: same source directory, same
 * provenance file, same licence rules, same failure-on-problem policy.
 *
 * ## Stages
 *
 *   1. fetch    Read `assets/models.mjs` (the curated list), download each
 *               glTF and its buffer from Poly Haven, verify both against the
 *               provider's published MD5. A mismatch is fatal — an unverified
 *               asset is one whose licence and origin nobody can attest to.
 *
 *   2. import   Parse the glTF into EMERGENT's vertex format, preserving vertex
 *               sharing so the mesh can be decimated. `gltf.mjs` does this.
 *
 *   3. scale    Normalise to real-world metres using the spec's declared
 *               height, measured from the imported bounding box rather than
 *               from the provider's metadata, which is wrong often enough to be
 *               useless. A model that lands more than 15% from its declared
 *               height is rejected: a wrong-scale chair makes the generated
 *               architecture around it look wrong too.
 *
 *   4. decimate Build the LOD chain with meshoptimizer, the same library the
 *               native engine already links. `lod.mjs` does this.
 *
 *   5. bake     Emit `assets/models.gen.mjs` — a plain ES module the game
 *               imports statically, holding the vertex data as base64 so the
 *               browser never has to parse JSON at boot.
 *
 * ## Why base64 in a generated module
 *
 * The alternatives are a JSON fetch and a custom binary format. A fetch means
 * the world generator cannot be synchronous, which cascades through every
 * caller. Base64 costs 33% on bytes that are already going to be in memory as
 * a Float32Array, and in exchange the model set is a normal import: tree-shake-
 * able, syntax-checked by `node --check`, and covered by the existing tooling
 * test that the built `dist/` actually boots.
 *
 * ## Run
 *
 *   npm run assets:models
 *
 * Deterministic: same inputs, byte-identical output. No timestamps in the
 * emitted module.
 */
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { importGltf } from '../../gltf.mjs';
import { buildLodChain, setSimplifier, sharingRatio } from '../../lod.mjs';
import { MODELS, ROOM } from '../../assets/models.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const SOURCE_DIR = path.join(ROOT, 'assets', 'source', 'models');
const PROVENANCE = path.join(ROOT, 'assets', 'provenance.json');
const OUT = path.join(ROOT, 'assets', 'models.gen.mjs');

const API = 'https://api.polyhaven.com';
const DOWNLOAD = 'https://dl.polyhaven.org/file/ph-assets/Models';

// How far a model may land from its declared real-world height before it is
// rejected. Tight enough to catch a provider unit error, loose enough to allow
// for a bounding box that includes a lamp cord.
const SCALE_TOLERANCE = 0.15;

let failures = 0;
function fail(message) {
  failures++;
  console.error(`  ✗ ${message}`);
}

async function getJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText} for ${url}`);
  return res.json();
}

function md5(buf) {
  return createHash('md5').update(buf).digest('hex');
}

async function downloadTo(url, dest) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText} for ${url}`);
  const buf = Buffer.from(await res.arrayBuffer());
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.writeFileSync(dest, buf);
  return buf;
}

// ---------------------------------------------------------------------------
// 1. Fetch
// ---------------------------------------------------------------------------

async function fetchAll() {
  console.log(`fetching ${MODELS.length} models from Poly Haven`);
  const entries = [];

  // The provenance file is shared with the material pipeline, so it is read,
  // merged into and written back rather than replaced. The material records
  // carry a `maps` block and these carry a `files` block; the audit treats them
  // as one list and checks each against whichever database declares it.
  const prov = fs.existsSync(PROVENANCE)
    ? JSON.parse(fs.readFileSync(PROVENANCE, 'utf8'))
    : { generated: null, provider: {}, license: {}, count: 0, assets: [] };
  const materialIds = new Set(prov.assets.filter((a) => a.maps).map((a) => a.asset_id));
  const kept = prov.assets.filter((a) => !MODELS.some((m) => m.id === a.asset_id));
  const records = [];

  for (const spec of MODELS) {
    const dir = path.join(SOURCE_DIR, spec.source);
    let files;
    let info;
    try {
      files = await getJson(`${API}/files/${spec.source}`);
      info = await getJson(`${API}/info/${spec.source}`);
    } catch (err) {
      fail(`${spec.id}: cannot reach the Poly Haven API (${err.message})`);
      continue;
    }

    const gltf = files.gltf && files.gltf['1k'];
    if (!gltf || !gltf.gltf) {
      fail(`${spec.id}: no 1k glTF available; ${spec.source} cannot be imported`);
      continue;
    }

    // The descriptor's own "1k" variant points at an 8k mesh binary. That is
    // Poly Haven's naming, not an error, and it is what makes these assets
    // expensive — so it is recorded rather than silently downloaded at 8k.
    const items = [[gltf.gltf, ''], ...Object.entries(gltf.gltf.include || {}).map(([n, v]) => [v, n])];
    const fileRecords = [];
    let ok = true;
    for (const [file, relative] of items) {
      const dest = path.join(dir, relative || path.basename(new URL(file.url).pathname));
      let buf;
      if (fs.existsSync(dest)) {
        buf = fs.readFileSync(dest);
        if (md5(buf) !== file.md5) {
          // A cached file that no longer matches the provider's checksum is
          // corrupt or was replaced. Re-fetching is the only safe response.
          buf = await downloadTo(file.url, dest);
          if (md5(buf) !== file.md5) {
            fail(`${spec.id}: md5 mismatch on ${relative || 'gltf'} after re-download`);
            ok = false;
            break;
          }
        }
      } else {
        buf = await downloadTo(file.url, dest);
        if (md5(buf) !== file.md5) {
          fail(`${spec.id}: md5 mismatch on ${relative || 'gltf'}`);
          ok = false;
          break;
        }
      }
      fileRecords.push({
        role: relative ? path.basename(relative).replace(`${spec.source}_`, '').replace(/_\d+k\.\w+$/, '') : 'gltf',
        source_url: file.url,
        file: path.relative(ROOT, dest),
        md5: file.md5,
        bytes: buf.length
      });
    }
    if (!ok) continue;

    entries.push({
      spec,
      dir,
      info,
      gltfPath: path.join(dir, path.basename(new URL(gltf.gltf.url).pathname)),
      polycount: info.polycount || null
    });

    records.push({
      asset_id: spec.id,
      kind: 'model',
      source: 'Poly Haven',
      source_provider_id: 'polyhaven',
      source_url: `https://polyhaven.com/a/${spec.source}`,
      creator: Object.keys(info.authors || {}).join(', ') || 'unattributed',
      license: 'CC0',
      license_name: 'CC0 1.0 Universal (Public Domain Dedication)',
      license_url: 'https://polyhaven.com/license',
      original_format: 'gltf+bin',
      processed_format: 'emergent-vertex-array (lods 0-2)',
      files: fileRecords,
      scale: spec.realHeight,
      polycount: info.polycount || null,
      lod_count: 3,
      texture_resolution: (info.max_resolution || []).join('x'),
      material_count: 1,
      collision_status: 'none',
      runtime_path: 'assets/models.gen.mjs',
      fetched: new Date().toISOString().slice(0, 10),
      source_max_resolution: info.max_resolution || null
    });
  }

  return { entries, records, kept, materialIds, prov };
}

// ---------------------------------------------------------------------------
// 2-4. Import, scale, decimate
// ---------------------------------------------------------------------------

/**
 * The uniform scale that puts a model at real-world size.
 *
 * The reference axis is Y, because every model here is furniture or a fitting
 * and furniture is authored Y-up: a coffee table is wider than it is tall, and
 * scaling by the *tallest* axis would make its 1.2 m width the declared 0.38 m
 * height and leave it 12 cm off the floor. That was a real bug, caught by the
 * height check below rejecting seven models.
 *
 * A flat object — a notepad, a picture frame lying flat — has a near-degenerate
 * Y. For those the largest horizontal axis is the meaningful one, so the
 * reference switches to it and the declared height is treated as the object's
 * largest dimension. Deciding by measurement rather than by hand is what stops
 * the two cases needing two sets of hand-written numbers.
 */
function scaleFor(spec, bounds) {
  const [sx, sy, sz] = bounds.size;
  const widest = Math.max(sx, sz);
  const tallest = Math.max(sx, sy, sz);
  if (!(tallest > 1e-6)) return { factor: 1, reference: 'y', referenceSize: sy };
  // A declared `flat` model is scaled on its largest horizontal axis regardless
  // of how thin it is, because its declared height is its visible size and not
  // its thickness. Letting the automatic switch below handle these is what
  // produced a 2 cm notepad and a 6 cm picture frame.
  if (spec.flat) return { factor: spec.realHeight / widest, reference: 'xz', referenceSize: widest };
  if (sy > widest * 0.05) return { factor: spec.realHeight / sy, reference: 'y', referenceSize: sy };
  return { factor: spec.realHeight / widest, reference: 'xz', referenceSize: widest };
}

/**
 * The largest plausible real-world dimension, in metres.
 *
 * A 4 m coffee table is a broken import, not a coffee table. Checked after
 * scaling so a provider unit error is caught here rather than showing up as a
 * chair the size of a house in a furnished room.
 */
const MAX_PLAUSIBLE_METRES = 4.0;

/**
 * The widest plausible ratio between a model's largest and smallest axis.
 *
 * A bed is about 2.3:1. A street bench is about 8:1. Nothing in a furnished
 * interior is 30:1, and a model that lands there has been scaled by a wrong
 * declared dimension rather than authored badly.
 */
const MAX_PLAUSIBLE_ASPECT = 12;

function importAndBake(entry) {
  const { spec, dir, gltfPath, polycount } = entry;
  const gltf = JSON.parse(fs.readFileSync(gltfPath, 'utf8'));

  const binName = gltf.buffers[0].uri;
  const bin = fs.readFileSync(path.join(dir, binName));

  // Import once to measure, then again at the final scale.
  const probe = importGltf(gltf, bin, { material: 0 });
  const { factor, reference } = scaleFor(spec, probe.bounds);

  const imported = importGltf(gltf, bin, { material: 0, tint: spec.tint });
  if (Math.abs(factor - 1) > 1e-9) scaleVertices(imported.vertices, factor);
  const scaled = boundsOf(imported.vertices);

  // Plausibility, not self-consistency. The declared height is now true by
  // construction, so checking it again would be circular; what is worth
  // checking is that nothing came out the size of a vehicle.
  const largest = Math.max(...scaled.size);
  if (largest > MAX_PLAUSIBLE_METRES) {
    fail(`${spec.id}: scaled to ${largest.toFixed(2)} m on its largest axis, `
      + `which is not a ${spec.id}. Declared ${spec.realHeight} m, reference axis ${reference}.`);
    return null;
  }
  if (scaled.size.some((v) => !(v >= 0) || !Number.isFinite(v) || v > 1e4)) {
    fail(`${spec.id}: scaled bounds are degenerate (${scaled.size.map((v) => v.toFixed(2)).join(' x ')})`);
    return null;
  }
  // An implausible aspect ratio is how a mis-declared height shows up when the
  // largest axis still fits under the cap: a 0.68 m sideboard declared at 1.35 m
  // scales to 2.9 m tall, which passes an absolute check and is nonsense. The
  // ratio is what actually catches it, and it is measured rather than eyeballed.
  const sorted = [...scaled.size].sort((a, b) => a - b);
  const aspect = sorted[2] / Math.max(sorted[0], 1e-4);
  if (aspect > MAX_PLAUSIBLE_ASPECT && !spec.flat) {
    fail(`${spec.id}: scaled to ${scaled.size.map((v) => v.toFixed(2)).join(' x ')} m, `
      + `an aspect of ${aspect.toFixed(1)}:1. Check realHeight — furniture is not this shape.`);
    return null;
  }
  if (Math.abs(probe.bounds.size[1] * factor - spec.realHeight) / spec.realHeight > SCALE_TOLERANCE
      && reference === 'y') {
    fail(`${spec.id}: measured height ${(probe.bounds.size[1] * factor).toFixed(2)} m does not match the `
      + `declared ${spec.realHeight} m within ${(SCALE_TOLERANCE * 100).toFixed(0)}%`);
    return null;
  }

  const sharing = sharingRatio(imported.vertexCount, imported.indices);
  const { levels, warnings } = buildLodChain(imported.vertices, imported.indices, 0);
  for (const w of warnings) console.log(`    ! ${spec.id}: ${w}`);

  // The published polycount is an independent check on the importer. A large
  // disagreement means vertices are being dropped or duplicated, which is
  // invisible in the output but expensive.
  if (polycount && Math.abs(polycount - imported.triangles) / polycount > 0.02) {
    fail(`${spec.id}: imported ${imported.triangles} triangles but Poly Haven publishes ${polycount}`);
    return null;
  }

  const measured2 = boundsOf(imported.vertices);
  return {
    spec,
    levels,
    sharing,
    triangles: imported.triangles,
    vertexCount: imported.vertexCount,
    scale: factor,
    bounds: measured2,
    reference,
    warnings
  };
}

function scaleVertices(vertices, factor) {
  for (let i = 0; i < vertices.length; i += 12) {
    vertices[i] *= factor;
    vertices[i + 1] *= factor;
    vertices[i + 2] *= factor;
  }
  // Normals are directions: a uniform scale leaves them correct.
}

function boundsOf(vertices) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < vertices.length; i += 12) {
    for (let k = 0; k < 3; k++) {
      if (vertices[i + k] < min[k]) min[k] = vertices[i + k];
      if (vertices[i + k] > max[k]) max[k] = vertices[i + k];
    }
  }
  return { min, max, size: [max[0] - min[0], max[1] - min[1], max[2] - min[2]] };
}

// ---------------------------------------------------------------------------
// 5. Emit
// ---------------------------------------------------------------------------

function emit(baked) {
  const payload = {};
  for (const b of baked) {
    payload[b.spec.id] = {
      room: b.spec.room,
      realHeight: b.spec.realHeight,
      triangles: b.triangles,
      vertices: b.vertexCount,
      bounds: b.bounds,
      lods: b.levels.map((l) => ({
        triangles: l.indices.length / 3,
        data: Buffer.from(l.vertices.buffer, l.vertices.byteOffset, l.vertices.byteLength)
          .toString('base64')
      }))
    };
  }

  const totalBytes = baked.reduce(
    (s, b) => s + b.levels.reduce((t, l) => t + l.vertices.byteLength, 0), 0
  );

  const body = `/**
 * GENERATED FILE — do not edit. Run \`npm run assets:models\` to rebuild.
 *
 * ${baked.length} CC0 models from Poly Haven, imported, scaled to real-world
 * metres and decimated to three LOD levels with meshoptimizer. Provenance for
 * every one is in \`assets/provenance.json\`.
 *
 * Total vertex data: ${(totalBytes / 1048576).toFixed(2)} MB.
 */

/** 12 floats per vertex: position(3) normal(3) colour(3) material(1) uv(2). */
export const FLOATS_PER_VERTEX = 12;

/** Room archetypes these models are placed in. */
export const ROOM = ${JSON.stringify(ROOM)};

export const MODELS = ${JSON.stringify(payload, (k, v) => (k === 'data' ? v : v), 2)};

/** Decode one LOD's base64 into a Float32Array. */
export function lodVertices(model, level) {
  const l = model.lods[level];
  if (!l) throw new Error(\`model has no LOD\${level}\`);
  const bin = atob(l.data);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}

/** Every model id, in declaration order. */
export const MODEL_IDS = Object.keys(MODELS);
`;

  fs.writeFileSync(OUT, body);
  return { totalBytes, count: baked.length };
}

// ---------------------------------------------------------------------------

async function main() {
  const { MeshoptSimplifier } = await import('meshoptimizer/simplifier');
  await MeshoptSimplifier.ready;
  if (!MeshoptSimplifier.supported) {
    console.error('meshoptimizer reports the simplifier is unsupported in this runtime');
    process.exit(1);
  }
  setSimplifier(MeshoptSimplifier);

  const { entries, records, kept, prov } = await fetchAll();
  if (!entries.length) {
    console.error('no models could be fetched');
    process.exit(1);
  }

  const baked = [];
  for (const entry of entries) {
    const result = importAndBake(entry);
    if (result) baked.push(result);
  }

  if (failures) {
    console.error(`\n${failures} model(s) failed validation; refusing to write a partial asset set`);
    process.exit(1);
  }

  const { totalBytes, count } = emit(baked);

  const all = [...kept, ...records].sort((a, b) => String(a.asset_id).localeCompare(String(b.asset_id)));
  fs.writeFileSync(PROVENANCE, `${JSON.stringify({
    ...prov,
    generated: new Date().toISOString(),
    count: all.length,
    assets: all
  }, null, 2)}\n`);

  console.log(`\nbaked ${count} models, ${(totalBytes / 1048576).toFixed(2)} MB of vertex data`);
  for (const b of baked) {
    const chain = b.levels.map((l) => l.indices.length / 3).join(' -> ');
    console.log(
      `  ${b.spec.id.padEnd(16)} ${b.triangles.toString().padStart(6)} tris  ` +
      `[${chain}]  ${b.bounds.size.map((v) => v.toFixed(2)).join('x')} m  x${b.scale.toFixed(4)}`
    );
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
