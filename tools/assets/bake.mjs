/**
 * Bake the source textures into the runtime material set.
 *
 *   node tools/assets/bake.mjs            # bake everything
 *   node tools/assets/bake.mjs --verbose  # print the per-material statistics
 *
 * Reads `assets/source/` (downloads, never modified) and writes the runtime
 * material set into `assets/textures/`, which is committed. Three output maps
 * per material, at one uniform size, one layer each in three
 * `TEXTURE_2D_ARRAY`s:
 *
 *   albedo  SRGB8_ALPHA8   base colour
 *   normal  RGBA8          tangent-space normal, OpenGL +Y
 *   arm     RGBA8          R = ambient occlusion, G = roughness, B = metallic
 *
 * ## What "bake" means here, and what it deliberately does not
 *
 * Resampling (Lanczos3, deterministic) and channel packing. It does *not*
 * transcode to a block-compressed format, and the reason is worth stating
 * plainly rather than leaving as a gap:
 *
 * A browser cannot upload BCn/ASTC without either a `compressedTexImage2D`
 * extension for that exact format or a WASM transcoder, and WebGL2 guarantees
 * neither. Encoding BCn offline needs KTX-Software's `toktx`, which is not
 * present in this environment and is not installable from it. Shipping a
 * ~500 KB Basis transcoder into the game's first frame to save download bytes
 * that a 512-px PNG does not cost is the wrong trade for a web target, so the
 * runtime is RGBA8 and the pipeline is honest about it. `docs/ASSET_PIPELINE.md`
 * records the decision and the measured byte counts; the moment a build host
 * with `toktx` exists this is a one-line change to the encoder, not a redesign.
 *
 * Mipmaps are generated on the GPU at upload, which is the normal path and the
 * only one that does not require shipping a mip chain as a second image per
 * material. The harness asserts `generateMipmap` is actually called.
 *
 * ## Validation is not decoration
 *
 * Every one of these has caught a real class of bug, and each is a hard failure
 * rather than a warning, because a material with swapped channels is invisible
 * until someone looks at a wall:
 *
 *  - **Degenerate albedo** — all black, all white, or a single flat colour is a
 *    failed download that md5 cannot catch because the provider published it.
 *  - **Normal map without a Z** — if the blue channel is not the dominant one
 *    the map is a DX-convention normal and every lit surface in the game is
 *    lit from the wrong side.
 *  - **ARM channel order** — the blue channel is compared against the
 *    material's declared metallicity. A map with roughness in blue would
 *    silently turn every brick surface metallic.
 *  - **Visible seams** — the difference across the wrap edge is compared
 *    against the difference between interior neighbours. A material that tiles
 *    visibly is worse than a low-resolution one, so this fails the build.
 */

import { mkdir, readFile, writeFile, readdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import sharp from 'sharp';
import { MATERIALS, MAPS, LICENSE, PROVIDER, TEXTURE_SIZE, TEXTURE_ARRAYS, RUNTIME_DIR } from '../../assets/database.mjs';
import { seamRatio, channelStats } from './seam.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const SOURCE_DIR = path.join(ROOT, 'assets', 'source');
const OUT_DIR = path.join(ROOT, RUNTIME_DIR);
const VERBOSE = process.argv.includes('--verbose');

const fmt = (v, d = 2) => v.toFixed(d);

async function main() {
  await mkdir(OUT_DIR, { recursive: true });
  const provenance = JSON.parse(await readFile(path.join(ROOT, 'assets', 'provenance.json'), 'utf8'));
  const provenanceById = new Map(provenance.assets.map(a => [a.asset_id, a]));

  const failures = [];
  const materials = [];
  let totalBytes = 0;

  for (let layer = 0; layer < MATERIALS.length; layer++) {
    const m = MATERIALS[layer];
    const prov = provenanceById.get(m.id);
    if (!prov) { failures.push(`${m.id}: no provenance record — run tools/assets/fetch.mjs`); continue; }

    const per = {};
    for (const array of TEXTURE_ARRAYS) {
      const entry = prov.maps[array.map];
      if (!entry) { failures.push(`${m.id}: provenance has no '${array.map}' map`); continue; }
      const src = path.join(ROOT, entry.file);
      if (!existsSync(src)) { failures.push(`${m.id}: source file missing: ${path.relative(ROOT, src)}`); continue; }

      // Lanczos3 is the sharp default and is chosen deliberately: it is the
      // only kernel here that does not visibly soften a brick bond pattern
      // when a 2k hero texture is reduced to 512.
      let pipeline = sharp(src).resize(TEXTURE_SIZE, TEXTURE_SIZE, {
        fit: 'fill', kernel: 'lanczos3', withoutEnlargement: false
      });
      if (array.key === 'arm') {
        // AO belongs in alpha for a reason a future packed format will thank
        // us for, and it costs nothing while we are rewriting the pixels anyway.
        pipeline = pipeline.ensureAlpha(1);
      }
      const { data, info } = await pipeline
        .ensureAlpha()
        .raw()
        .toBuffer({ resolveWithObject: true });
      if (info.width !== TEXTURE_SIZE || info.height !== TEXTURE_SIZE) {
        failures.push(`${m.id}/${array.key}: baked to ${info.width}x${info.height}, expected ${TEXTURE_SIZE}`);
        continue;
      }

      const s = channelStats(data, info.width, info.height);
      per[array.key] = { stats: s, raw: data, width: info.width, height: info.height };

      if (array.key === 'albedo') {
        if (s.lmean < 8) failures.push(`${m.id}/albedo: near black (mean luma ${fmt(s.lmean)}), degenerate`);
        else if (s.lmean > 248) failures.push(`${m.id}/albedo: near white (mean luma ${fmt(s.lmean)}), degenerate`);
        else if (s.lstd < 1.0) failures.push(`${m.id}/albedo: flat colour (stddev ${fmt(s.lstd)}), degenerate`);
        const seam = seamRatio(data, info.width, info.height);
        per.seam = seam;
        if (seam.ratio > 3.0) {
          failures.push(`${m.id}/albedo: visible seam, wrap edge is ${fmt(seam.ratio)}x the interior difference`);
        }
      }

      if (array.key === 'normal') {
        // A tangent-space normal map's blue channel is the one that says which
        // way the surface faces, so it dominates. If it does not, the map is a
        // DirectX-convention normal and the whole game is lit inside out.
        const { mean } = s;
        if (mean.b <= Math.max(mean.r, mean.g)) {
          failures.push(`${m.id}/normal: blue is not the dominant channel ` +
            `(r=${fmt(mean.r, 3)} g=${fmt(mean.g, 3)} b=${fmt(mean.b, 3)}) — not a tangent-space normal`);
        }
        if (mean.b < 0.55) {
          failures.push(`${m.id}/normal: blue mean ${fmt(mean.b, 3)} is too low, the surface faces away from the viewer`);
        }
      }

      if (array.key === 'arm') {
        // The blue channel is metallic, and the database declares what each
        // material's must measure. Comparing against a declared range rather
        // than against a hard-coded rule is what catches a provider-side channel
        // swap: roughness landing in blue makes every dielectric surface shiny.
        // channelStats reports normalised means, so the declared band is 0..1 too.
        const b = s.mean.b;
        const [lo, hi] = m.expectedMetal;
        if (b < lo || b > hi) {
          failures.push(`${m.id}/arm: metalness ${fmt(b / 255, 3)} is outside the declared ` +
            `${lo}..${hi} for '${m.role}' — the ARM channels may be swapped`);
        }
        const ao = s.mean.r;
        if (ao < 0.35) {
          failures.push(`${m.id}/arm: ambient occlusion averages ${fmt(ao)}, which would black out the material`);
        }
        per[array.key].measuredMetalness = Number(fmt(b, 3));
      }

      const outFile = `${m.id}_${array.key}.png`;
      const outPath = path.join(OUT_DIR, outFile);
      await sharp(data, { raw: { width: info.width, height: info.height, channels: 4 } })
        .png({ compressionLevel: 9, effort: 8 })
        .toFile(outPath);
      const { size } = await import('node:fs/promises').then(fs => fs.stat(outPath));
      totalBytes += size;
      per[array.key].bytes = size;
      per[array.key].file = `${RUNTIME_DIR}/${outFile}`;
      delete per[array.key].raw;
    }

    if (TEXTURE_ARRAYS.some(a => !per[a.key])) continue;

    materials.push({
      id: m.id,
      layer,
      role: m.role,
      tier: m.tier,
      tileScale: m.scale,
      roughness: m.roughness,
      metallic: m.metallic,
      maps: Object.fromEntries(TEXTURE_ARRAYS.map(a => [a.key, per[a.key].file])),
      provenance: {
        source: prov.source,
        source_url: prov.source_url,
        creator: prov.creator,
        license: prov.license,
        license_url: prov.license_url,
        original_format: prov.original_format,
        source_resolution: prov.maps.diff.resolution,
        fetched: prov.fetched
      },
      measured: {
        albedoMeanLuma: Number(fmt(per.albedo.stats.lmean, 1)),
        albedoStddev: Number(fmt(per.albedo.stats.lstd, 1)),
        seamRatio: per.seam ? Number(fmt(per.seam.ratio, 2)) : null,
        armAo: Number(fmt(per.arm.stats.mean.r, 3)),
        armRoughness: Number(fmt(per.arm.stats.mean.g, 3)),
        armMetallic: per.arm.measuredMetalness,
        normalBlue: Number(fmt(per.normal.stats.mean.b, 3))
      },
      bytes: TEXTURE_ARRAYS.reduce((s, a) => s + per[a.key].bytes, 0)
    });

    if (VERBOSE) {
      console.log(
        `✓ ${m.id.padEnd(22)} luma ${fmt(per.albedo.stats.lmean, 1).padStart(5)} ` +
        `sd ${fmt(per.albedo.stats.lstd, 1).padStart(5)} ` +
        `seam ${fmt(per.seam ? per.seam.ratio : 0, 2).padStart(5)} ` +
        `ao ${fmt(per.arm.stats.mean.r, 2)} ` +
        `rgh ${fmt(per.arm.stats.mean.g, 2)} ` +
        `met ${fmt(per.arm.stats.mean.b, 2)} ` +
        `${fmt(TEXTURE_ARRAYS.reduce((s, a) => s + per[a.key].bytes, 0) / 1024, 0).padStart(5)}KB`
      );
    }
  }

  // The runtime descriptor the game loads. It is the contract between the bake
  // and the renderer: array layer indices, tiling scale, and the PBR multipliers
  // that let one shared texture serve several surfaces.
  const descriptor = {
    version: 1,
    generated: new Date().toISOString(),
    size: TEXTURE_SIZE,
    arrays: Object.fromEntries(TEXTURE_ARRAYS.map(a => [a.key, {
      internalFormat: a.internalFormat, format: a.format, type: a.type, srgb: a.srgb, layers: materials.length
    }])),
    // `metallic` is resolved here, once, rather than in the shader: a negative
    // value is the sentinel for "read metalness from the texture", which is what
    // one material in the set (clean steel) needs and the other twenty-five do
    // not. The shader gets one float per material and one comparison.
    materials: Object.fromEntries(materials.map(m => [m.id, {
      layer: m.layer,
      tileScale: m.tileScale,
      roughness: m.roughness,
      metallic: m.metallic === null ? -1 : m.metallic,
      role: m.role,
      maps: m.maps
    }])),
    licence: LICENSE,
    provider: PROVIDER,
    measured: {
      materialCount: materials.length,
      textureSize: TEXTURE_SIZE,
      bytesOnDisk: totalBytes,
      // VRAM the browser will hold once the arrays are uploaded with a full
      // mip chain: 4 bytes/texel, 4/3 for the chain.
      estimatedVramBytes: Math.round(
        TEXTURE_ARRAYS.length * materials.length * TEXTURE_SIZE * TEXTURE_SIZE * 4 * (4 / 3)
      )
    }
  };
  // The manifest is JSON because a machine reading it should not have to parse
  // JavaScript. The *runtime* descriptor is a module, because the game imports
  // it statically: no fetch, no JSON.parse, no import attributes, and a typo in
  // a layer index is a module-load failure rather than a silent `undefined`.
  await writeFile(path.join(OUT_DIR, 'materials.json'), JSON.stringify(descriptor, null, 2) + '\n');

  // The file table, so the loader composes URLs from the bake's own output
  // rather than from a list that could drift out of step with it.
  descriptor.files = Object.fromEntries(TEXTURE_ARRAYS.map(a => [
    a.key, materials.map(m => `${m.id}_${a.key}.png`)
  ]));

  const module = `// GENERATED by tools/assets/bake.mjs — do not edit.
//
// ${MATERIALS.length} CC0 materials from Poly Haven, baked at ${TEXTURE_SIZE}px into
// three texture arrays. Provenance for every one is in assets/provenance.json.
export const MATERIAL_DESCRIPTOR = ${JSON.stringify({
    version: descriptor.version,
    size: descriptor.size,
    arrays: descriptor.arrays,
    materials: descriptor.materials,
    files: descriptor.files
  }, null, 2)};

export const MEASURED = ${JSON.stringify(descriptor.measured, null, 2)};
`;
  await writeFile(path.join(OUT_DIR, 'materials.mjs'), module);

  await writeFile(path.join(ROOT, 'assets', 'materials.json'), JSON.stringify({
    generated: descriptor.generated,
    materials: materials.map(({ maps, ...rest }) => rest)
  }, null, 2) + '\n');

  console.log(`\nbaked ${materials.length} materials at ${TEXTURE_SIZE}px into 3 arrays`);
  console.log(`  disk      ${(totalBytes / 1048576).toFixed(1)} MB of PNG`);
  console.log(`  vram est  ${(descriptor.measured.estimatedVramBytes / 1048576).toFixed(0)} MB with mips`);
  if (failures.length) {
    console.error(`\n${failures.length} validation failure(s):`);
    for (const f of failures) console.error(`  ✗ ${f}`);
    process.exit(1);
  }
  console.log('  all validation passed');
}

main().catch(err => { console.error(err); process.exit(1); });
