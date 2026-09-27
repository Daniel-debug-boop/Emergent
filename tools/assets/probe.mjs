/**
 * Measure a candidate texture before committing it to the asset database.
 *
 *   node tools/assets/probe.mjs rustic_stone_wall weathered_plank_siding ...
 *   node tools/assets/probe.mjs --search stone   # every Poly Haven id matching
 *
 * Selection aid, not part of the build. It answers the two questions that
 * actually decide whether a texture belongs in EMERGENT — does it tile, and is
 * its channel layout what its name says — without downloading 2k source files
 * for the answer.
 *
 * It fetches the 1k jpg for `diff` and `arm` only, which is a few hundred
 * kilobytes per candidate, and reports the numbers the bake will later assert.
 */

import { mkdir, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import sharp from 'sharp';
import { seamRatio, channelStats } from './seam.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const CACHE = path.join(ROOT, 'assets', '.cache', 'probe');
const PROVIDER = 'https://api.polyhaven.com';
const FILES = 'https://dl.polyhaven.org/file/ph-assets';

// Measured at the resolution the bake actually produces. An earlier version
// sampled at 256 and cheerfully recommended textures that turned out to have a
// 12x seam at 512 — the selection tool has to answer the question the build
// will ask, or it is not a selection tool.
const SAMPLE = 512;

async function raw(id, map, res = '1k') {
  const dir = path.join(CACHE, id);
  await mkdir(dir, { recursive: true });
  const file = path.join(dir, `${id}_${map}_${res}.jpg`);
  const info = await (await fetch(`${PROVIDER}/files/${id}`)).json();
  const entry = info[map] && info[map][res];
  if (!entry || !entry.jpg) return null;
  if (!existsSync(file)) {
    const res2 = await fetch(entry.jpg.url);
    if (!res2.ok) return null;
    await writeFile(file, Buffer.from(await res2.arrayBuffer()));
  }
  // Kept as RGBA on purpose: the seam and channel maths read interleaved RGBA,
  // and a 3-channel buffer here would read past the end of every row and
  // produce NaN rather than an obviously wrong number.
  const { data, info: i } = await sharp(file)
    .resize(SAMPLE, SAMPLE, { fit: 'fill', kernel: 'lanczos3' })
    .ensureAlpha()
    .raw().toBuffer({ resolveWithObject: true });
  return { data, width: i.width, height: i.height };
}

async function main() {
  const args = process.argv.slice(2);
  const searchIdx = args.indexOf('--search');
  let ids;
  if (searchIdx >= 0) {
    const re = new RegExp(args[searchIdx + 1], 'i');
    const catalog = await (await fetch(`${PROVIDER}/assets?t=textures`)).json();
    ids = Object.keys(catalog).filter(k => re.test(k)).slice(0, 24);
    console.log(`${ids.length} candidate(s) matching /${args[searchIdx + 1]}/i\n`);
  } else {
    ids = args;
  }

  const rows = [];
  for (const id of ids) {
    try {
      const diff = await raw(id, 'Diffuse');
      const arm = await raw(id, 'arm');
      if (!diff) { rows.push({ id, verdict: 'no diffuse' }); continue; }
      const s = seamRatio(diff.data, diff.width, diff.height);
      const cs = channelStats(diff.data, diff.width, diff.height);
      const a = arm ? channelStats(arm.data, arm.width, arm.height) : null;
      rows.push({
        id, seam: s.ratio, luma: cs.lmean, std: cs.lstd,
        metal: a ? a.mean.b : null, ao: a ? a.mean.r : null, rough: a ? a.mean.g : null,
        verdict: s.ratio <= 3 ? 'TILEABLE' : 'SEAM'
      });
    } catch (err) {
      rows.push({ id, verdict: `error: ${err.message}` });
    }
  }

  console.log('id'.padEnd(30), 'seam'.padStart(6), 'luma'.padStart(6), 'std'.padStart(6), 'ao'.padStart(6), 'rgh'.padStart(6), 'met'.padStart(6), ' verdict');
  for (const r of rows) {
    console.log(
      String(r.id).padEnd(30),
      (r.seam != null ? r.seam.toFixed(2) : '-').padStart(6),
      (r.luma != null ? r.luma.toFixed(0) : '-').padStart(6),
      (r.std != null ? r.std.toFixed(1) : '-').padStart(6),
      (r.ao != null ? r.ao.toFixed(2) : '-').padStart(6),
      (r.rough != null ? r.rough.toFixed(2) : '-').padStart(6),
      (r.metal != null ? r.metal.toFixed(2) : '-').padStart(6),
      ' ' + r.verdict
    );
  }
}

main().catch(err => { console.error(err); process.exit(1); });
