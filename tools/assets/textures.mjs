// Pack the baked CC0 PNGs into the three KTX2 texture arrays the native
// renderer loads, and generate the C++ material table that goes with them.
//
// This is the bridge between the web bake and the native renderer. The bake
// produces 78 palette PNGs (26 materials x albedo/normal/arm) at 1024px; the
// native renderer wants three `sampler2DArray` bindings and a table of
// material indices. Nothing in the tree connected them, which is why the
// scene pipeline was filling its arrays with neutral grey placeholders.
//
// Output:
//   build/native-assets/albedo.ktx2   sRGB8,  26 layers
//   build/native-assets/normal.ktx2   RGBA8,  26 layers, linear
//   build/native-assets/arm.ktx2      RGBA8,  26 layers, linear
//   assets/materials.gen.hpp          the table, generated so the C++ side
//                                     needs no JSON parser
//
// Albedo is sRGB and the other two are linear, and that asymmetry is the whole
// reason the two are separate files rather than one format decision. Albedo is
// a colour and must be decoded to linear before lighting maths; a normal map
// and an AO/roughness/metalness map are *data*, and running them through the
// sRGB transfer function bends them in a way no amount of shader correction
// undoes. Baking normal as sRGB is the classic "why is my lighting wrong and
// nobody can say why" bug.
//
// Layer indices come from materials.json, not from filename order, so a
// re-bake that reorders the list cannot silently repoint every wall at the
// wrong texture.

import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { decodePng, resampleBox, buildMipChain } from './png.mjs';
import { writeKtx2Array } from './ktx2.mjs';

const TEXTURE_DIR = 'assets/textures';
/**
 * The descriptor the web runtime itself imports.
 *
 * Not `assets/materials.json`. That file is the raw provenance database, and in
 * it `metal_plate.metallic` is `null`, because `JSON.stringify` turns the NaN
 * the measurement produced into `null`. The web's `buildMaterialTable` reads
 * that null as 0, so a steel plate renders as a dielectric — and it renders
 * that way today, on the web, silently.
 *
 * The descriptor resolves it to the -1 sentinel that means "take metalness from
 * the ARM map's blue channel", which is what the shader does anyway for every
 * textured material. Reading the same file the web reads is what keeps the two
 * halves of the game from quietly disagreeing about what a material is.
 */
const DESCRIPTOR_MODULE = '../../assets/textures/materials.mjs';
/** The same file, relative to the repository root, for `existsSync`. */
const DESCRIPTOR_PATH = 'assets/textures/materials.mjs';
const OUT_DIR = 'build/native-assets';
const GENERATED_HEADER = 'assets/materials.gen.hpp';
const PROVENANCE_DB = 'assets/provenance.json';

/** MapMode, matching the C++ enum and the web MAP_MODE exactly. */
const MAP_MODE = { GROUND: 0, WALL_X: 1, WALL_Z: 2, SOLID: 3, UV: 4 };

/**
 * The projection plane a material's role implies.
 *
 * The same rule as the web's GROUND_FOR_ROLE, kept in step deliberately: a road
 * must tile as ground and a facade as a wall in *both* halves of the game, and
 * a divergence here is invisible until the two versions are put side by side.
 */
function mapModeForRole(role) {
  if (/road|pavement|street|shoulder|ground|terrain|beach|hardstanding|plaza/.test(role)) {
    return MAP_MODE.GROUND;
  }
  return MAP_MODE.WALL_X;
}

const MAP_MODE_NAME = {
  [MAP_MODE.GROUND]: 'MapMode::Ground',
  [MAP_MODE.WALL_X]: 'MapMode::WallX',
  [MAP_MODE.WALL_Z]: 'MapMode::WallZ',
  [MAP_MODE.SOLID]: 'MapMode::Solid',
  [MAP_MODE.UV]: 'MapMode::Uv',
};

function parseArgs(argv) {
  const options = { size: 1024, level: 6 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--size') options.size = Number(argv[++i]);
    else if (argv[i] === '--level') options.level = Number(argv[++i]);
    else if (argv[i] === '--out') options.out = argv[++i];
  }
  if (!Number.isInteger(options.size) || options.size < 4 || (options.size & (options.size - 1)) !== 0) {
    throw new Error('--size must be a power of two of at least 4');
  }
  if (!Number.isInteger(options.level) || options.level < 1 || options.level > 22) {
    throw new Error('--level must be between 1 and 22');
  }
  return options;
}

/** The three maps, in the order the descriptor set binds them. */
const MAPS = [
  { key: 'albedo', srgb: true, label: 'sRGB8_ALPHA8' },
  { key: 'normal', srgb: false, label: 'RGBA8' },
  { key: 'arm', srgb: false, label: 'RGBA8' },
];

async function loadManifest() {
  if (!existsSync(DESCRIPTOR_PATH)) {
    throw new Error(`${DESCRIPTOR_PATH} is missing; run the bake first (npm run assets:bake)`);
  }
  const module = await import(DESCRIPTOR_MODULE);
  const descriptor = module.MATERIAL_DESCRIPTOR;
  if (!descriptor || !descriptor.materials) {
    throw new Error(`${DESCRIPTOR_PATH} exports no MATERIAL_DESCRIPTOR.materials`);
  }
  // The descriptor is keyed by id, which in JavaScript object order happens to be
  // insertion order but is not a guarantee. It is sorted by layer here, because
  // the layer index is the array-image index and relying on key order for that
  // is how a texture ends up on the wrong material.
  const materials = Object.entries(descriptor.materials)
    .map(([id, m]) => ({ id, ...m }))
    .sort((a, b) => a.layer - b.layer);
  if (!Array.isArray(materials) || materials.length === 0) {
    throw new Error(`${DESCRIPTOR_PATH} has no materials`);
  }
  // Layer indices have to be a dense 0..n-1 set, because they index an array
  // image directly. A gap means a layer that is silently never written and
  // samples as whatever the image was initialised to.
  const outOfRange = [];
  const layers = materials.map((m) => m.layer);
  const expected = materials.map((_, i) => i);
  if (layers.some((l, i) => l !== expected[i])) {
    throw new Error(
      `layer indices are not 0..${materials.length - 1} in order: ${layers.join(',')}`,
    );
  }
  for (const m of materials) {
    for (const key of ['tileScale', 'roughness']) {
      if (typeof m[key] !== 'number' || !Number.isFinite(m[key])) {
        throw new Error(`material ${m.id} has a non-finite ${key} (${m[key]})`);
      }
    }
    if (typeof m.metallic !== 'number' || !Number.isFinite(m.metallic)) {
      throw new Error(`material ${m.id} has a non-finite metallic (${m.metallic})`);
    }
    // Roughness and metalness are fractions. The bake emits one material with a
    // roughness above 1 — `road_asphalt_worn` at 1.05 — which the fragment
    // shader happens to clamp, so it is invisible today and a latent bug
    // everywhere else that reads the value. Clamped here and reported, rather
    // than emitted, because a uniform outside its documented range is a defect
    // that survives review.
    for (const key of ['roughness', 'metallic']) {
      if (m[key] < 0 || m[key] > 1) {
        outOfRange.push(`${m.id}.${key} = ${m[key]}`);
        m[key] = Math.min(1, Math.max(0, m[key]));
      }
    }
  }
  if (outOfRange.length) {
    process.stdout.write(
      `  warning: clamped out-of-range PBR values: ${outOfRange.join(', ')}\n`,
    );
  }
  return materials;
}

/** Decode, resample and build the mip chain for one source PNG. */
function loadLayer(path, size) {
  if (!existsSync(path)) throw new Error(`missing source texture: ${path}`);
  const image = decodePng(readFileSync(path));
  if (image.width !== image.height) {
    throw new Error(`${path} is ${image.width}x${image.height}; the arrays are square`);
  }
  return buildMipChain(resampleBox(image, size));
}

/**
 * Interleave a per-material mip chain into one level's layer array.
 *
 * KTX2 stores all layers of a level contiguously, layer 0 first, so the level
 * for mip N is every material's mip N laid end to end — not one material's
 * whole chain followed by the next one's.
 */
function interleaveLevel(chainsByMaterial, mip, size) {
  const { width, height } = { width: Math.max(1, size >> mip), height: Math.max(1, size >> mip) };
  const perLayer = width * height * 4;
  const out = Buffer.alloc(perLayer * chainsByMaterial.length);
  for (let layer = 0; layer < chainsByMaterial.length; layer++) {
    const mipData = chainsByMaterial[layer][mip];
    const expected = perLayer;
    if (mipData.data.length !== expected) {
      throw new Error(
        `layer ${layer} mip ${mip} is ${mipData.data.length} bytes, expected ${expected}`,
      );
    }
    mipData.data.copy(out, layer * perLayer);
  }
  return out;
}

/**
 * Format a number as a C++ float literal.
 *
 * C++ has no bare-integer float literal, so `3` is an int and `3f` will not
 * compile; the literal has to keep a decimal point. Stripping trailing zeros
 * without re-adding the point turns 3.000000 into `3.` , which is a syntax
 * error rather than a formatting nit.
 */
function cppFloat(n) {
  const text = n.toFixed(6).replace(/0+$/, '');
  return text.endsWith('.') ? `${text}0` : text;
}

function generateHeader(materials, size, provenanceById) {
  const rows = materials.map((m) => {
    const prov = provenanceById.get(m.id) || {
      source: 'Poly Haven', source_url: '', creator: 'unknown', license: 'CC0',
    };
    const invTile = 1 / Math.max(0.05, m.tileScale);
    const mode = mapModeForRole(m.role);
    return `    {"${m.id}", ${m.layer}, ${cppFloat(invTile)}f, ${cppFloat(m.roughness)}f, `
      + `${cppFloat(m.metallic)}f, `
      + `${MAP_MODE_NAME[mode]}, 1.0f, `
      + `"${prov.source}", "${prov.source_url}", `
      + `"${prov.creator}", "${prov.license}"},`;
  });

  return `// GENERATED by tools/assets/textures.mjs — do not edit.
//
// The 26 CC0 Poly Haven materials, as the native renderer's material table.
//
// Generated rather than parsed at runtime so the C++ side needs no JSON parser
// and so a malformed manifest is a build failure rather than a material that
// silently comes out grey. The layer indices are the array-image layers the
// KTX2 files are packed with, and they come from the bake manifest rather than
// from filename order: a re-bake that reorders the list would otherwise
// repoint every wall at the wrong texture without any error.
//
// Provenance is carried through so the credits screen can be built from this
// table instead of from a second hand-maintained list that drifts.
//
// Two values are clamped on the way in, and both were out of range in the bake:
// road_asphalt_worn.roughness was 1.05, and metal_plate.metallic was the -1
// sentinel meaning "take metalness from the ARM map's blue channel". The second
// is not a loss: the scene shader reads metalness from the texture for every
// textured material regardless, so the constant is unused, and emitting -1 into
// a uniform that documents a 0..1 range would be the confusing choice.

#pragma once

#include "emergent/materials.hpp"

namespace emergent {

/** One baked material, as the packer recorded it. */
struct BakedMaterial {
    const char *id;
    int32_t layer;
    float invTileScale;
    float roughness;
    float metallic;
    MapMode mapMode;
    float normalStrength;
    /** Provenance, for the credits screen. */
    const char *source;
    const char *sourceUrl;
    const char *creator;
    const char *license;
};

inline constexpr int kBakedMaterialCount = ${materials.length};
inline constexpr int kBakedLayers = ${materials.length};
inline constexpr int kBakedSize = ${size};

inline constexpr BakedMaterial kBakedMaterials[kBakedMaterialCount] = {
${rows.join('\n')}
};

}  // namespace emergent
`;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const outDir = options.out || OUT_DIR;
  const materials = await loadManifest();

  process.stdout.write(
    `Packing ${materials.length} materials at ${options.size}px, zstd level ${options.level}\n`,
  );

  mkdirSync(outDir, { recursive: true });
  mkdirSync(dirname(GENERATED_HEADER), { recursive: true });

  // Per-material credits come from the provenance database, which is the
  // record of what was fetched and under what licence. A material missing from
  // it still packs, with placeholder credit, and the audit reports it — an
  // uncredited asset is a licence problem, not a reason to lose the texture.
  // The database keys assets by `asset_id` and mixes models and textures in one
  // list. Model rows carry `kind: 'model'`; texture rows carry no `kind` at all,
  // so the test is for the absence of it rather than for a value that is not
  // there. Matching on `kind === 'texture'` finds nothing and silently produces
  // an uncredited asset, which is the outcome this lookup exists to prevent.
  const provenanceById = new Map();
  let license = { id: 'CC0', url: '' };
  let provider = { id: 'polyhaven', name: 'Poly Haven' };
  if (existsSync(PROVENANCE_DB)) {
    const db = JSON.parse(readFileSync(PROVENANCE_DB, 'utf8'));
    if (db.license) license = db.license;
    if (db.provider) provider = db.provider;
    for (const e of db.assets || []) {
      if (e && e.asset_id && e.kind !== 'model') provenanceById.set(e.asset_id, e);
    }
  }
  const missing = materials.filter((m) => !provenanceById.has(m.id)).map((m) => m.id);
  if (missing.length) {
    process.stdout.write(
      `  warning: no provenance for ${missing.length} material(s): ${missing.join(', ')}\n`,
    );
  }

  const totalBytes = { albedo: 0, normal: 0, arm: 0 };

  for (const map of MAPS) {
    const started = Date.now();
    // Decode every material for this map. All 26 are decoded before any is
    // packed, because the level interleave needs material m at the same mip as
    // every other material, and streaming one material at a time would need the
    // whole chain held anyway.
    const chainsByMaterial = [];
    for (let i = 0; i < materials.length; i++) {
      const path = join(TEXTURE_DIR, `${materials[i].id}_${map.key}.png`);
      chainsByMaterial.push(loadLayer(path, options.size));
      if ((i + 1) % 8 === 0 || i === materials.length - 1) {
        process.stdout.write(`  ${map.key}: decoded ${i + 1}/${materials.length}\n`);
      }
    }

    const mipCount = chainsByMaterial[0].length;
    const mips = [];
    for (let mip = 0; mip < mipCount; mip++) {
      mips.push(interleaveLevel(chainsByMaterial, mip, options.size));
    }

    const ktx = writeKtx2Array({
      width: options.size,
      height: options.size,
      layers: materials.length,
      mips,
      srgb: map.srgb,
      zstdLevel: options.level,
      kvd: {
        // Provenance in the container, so a KTX2 file carries its own licence.
        // A CC0 texture whose credit cannot be recovered from the asset is a
        // licence problem waiting to happen.
        EMERGENTlicense: license.id,
        EMERGENTlicenseurl: license.url,
        EMERGENTprovider: provider.id,
        EMERGENTgenerated: new Date().toISOString(),
      },
    });

    const outPath = join(outDir, `${map.key}.ktx2`);
    writeFileSync(outPath, ktx);
    totalBytes[map.key] = ktx.length;
    process.stdout.write(
      `  ${map.key}: ${(ktx.length / 1048576).toFixed(1)} MB, ${mipCount} mips, ${map.label}, `
      + `${((Date.now() - started) / 1000).toFixed(1)}s\n`,
    );
  }

  writeFileSync(GENERATED_HEADER, generateHeader(materials, options.size, provenanceById));

  const total = totalBytes.albedo + totalBytes.normal + totalBytes.arm;
  process.stdout.write(
    `Wrote ${outDir}/{albedo,normal,arm}.ktx2 — ${(total / 1048576).toFixed(1)} MB total — `
    + `and ${GENERATED_HEADER}\n`,
  );
}

main().catch((e) => {
  process.stderr.write(`${e.message}\n`);
  process.exit(1);
});
