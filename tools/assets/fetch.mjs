/**
 * Fetch the source textures named in the asset database.
 *
 *   node tools/assets/fetch.mjs            # only what is missing
 *   node tools/assets/fetch.mjs --force    # re-download everything
 *   node tools/assets/fetch.mjs --check    # validate against the API, download nothing
 *
 * This is the only tool in the repository that touches the network, and it is
 * deliberately paranoid about it:
 *
 *  - It validates every id against Poly Haven's API *before* downloading, so a
 *    typo in the database is a clear error rather than a 404 in the middle of a
 *    30-material run.
 *  - It refuses any material whose maps are not the three EMERGENT needs. A
 *    material with no normal map is not a material, it is a colour.
 *  - It writes a provenance record per asset, capturing the exact source URL,
 *    the author Poly Haven names, the licence, and the date. That record is
 *    what makes "these assets can legally ship" checkable later rather than a
 *    claim in a README.
 *  - It verifies the md5 Poly Haven publishes for each file, so a truncated or
 *    substituted download cannot enter the tree unnoticed.
 *
 * Source files are never modified. Everything downstream reads them read-only
 * and writes to `dist/`.
 */

import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { MATERIALS, MAPS, LICENSE, PROVIDER, TIER_SOURCE_RESOLUTION } from '../../assets/database.mjs';

const ROOT = path.resolve(import.meta.dirname, '..', '..');
const SOURCE_DIR = path.join(ROOT, 'assets', 'source');
const CACHE_DIR = path.join(ROOT, 'assets', '.cache');
const args = process.argv.slice(2);
const FORCE = args.includes('--force');
const CHECK_ONLY = args.includes('--check');

/** How large a source file to pull for a material, from its detail tier. */
const sourceResolution = (m) => TIER_SOURCE_RESOLUTION[m.tier] || '1k';

async function getJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText} for ${url}`);
  return res.json();
}

async function download(url, dest) {
  await mkdir(path.dirname(dest), { recursive: true });
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText} for ${url}`);
  const buf = Buffer.from(await res.arrayBuffer());
  await writeFile(dest, buf);
  return buf;
}

const md5 = (buf) => createHash('md5').update(buf).digest('hex');

/** Ask the API for a material's files and its authorship. */
async function describe(id) {
  const files = await getJson(`${PROVIDER.api}/files/${id}`);
  const info = await getJson(`${PROVIDER.api}/info/${id}`);
  return { files, info };
}

/**
 * Pick the URL for one map at one resolution.
 *
 * Prefers the published `md5` over the plain download: it is the only thing in
 * the chain that detects a corrupted or substituted file.
 */
function mapUrl(files, polyhavenMap, resolution) {
  const entry = files[polyhavenMap];
  if (!entry) return null;
  const atRes = entry[resolution];
  if (!atRes) {
    const available = Object.keys(entry).join(', ');
    throw new Error(`map '${polyhavenMap}' has no ${resolution} variant (has: ${available})`);
  }
  const file = atRes.jpg || atRes.png;
  if (!file || !file.url) return null;
  return { url: file.url, md5: file.md5, format: atRes.jpg ? 'jpg' : 'png' };
}

async function main() {
  await mkdir(SOURCE_DIR, { recursive: true });
  await mkdir(CACHE_DIR, { recursive: true });

  const report = [];
  const failures = [];
  let downloaded = 0, cached = 0;

  for (const material of MATERIALS) {
    const id = material.polyhaven;
    const record = {
      asset_id: material.id,
      source: PROVIDER.name,
      source_provider_id: PROVIDER.id,
      source_url: `https://polyhaven.com/a/${id}`,
      creator: null,
      license: LICENSE.id,
      license_name: LICENSE.name,
      license_url: LICENSE.url,
      original_format: 'jpeg',
      maps: {},
      fetched: null
    };

    let described;
    try {
      described = await describe(id);
    } catch (err) {
      failures.push(`${material.id}: cannot describe '${id}' — ${err.message}`);
      continue;
    }

    const author = Object.entries(described.info.authors || {}).map(([k, v]) => `${k} (${v})`).join(', ');
    record.creator = author || 'unattributed on Poly Haven';
    record.source_max_resolution = described.info.max_resolution;

    const resolution = sourceResolution(material);
    const dir = path.join(SOURCE_DIR, material.id);

    // Validate first: every map must exist, and the provider's checksum must be
    // published, before a single byte is written.
    const wanted = [];
    for (const map of MAPS) {
      let picked;
      try {
        picked = mapUrl(described.files, map.polyhaven, resolution);
      } catch (err) {
        failures.push(`${material.id}: ${err.message}`);
        continue;
      }
      if (!picked) {
        failures.push(`${material.id}: Poly Haven has no ${map.key} map ('${map.polyhaven}') at ${resolution}`);
        continue;
      }
      if (!picked.md5) {
        failures.push(`${material.id}: ${map.key} has no published md5, refusing to download unverifiable bytes`);
        continue;
      }
      wanted.push({ map, ...picked, dest: path.join(dir, `${material.id}_${map.key}_${resolution}.${picked.format}`) });
    }
    if (wanted.length !== MAPS.length) continue;

    record.maps = Object.fromEntries(wanted.map(w => [w.map.key, {
      source_url: w.url, file: path.relative(ROOT, w.dest), md5: w.md5, resolution, srgb: w.map.srgb
    }]));

    if (CHECK_ONLY) {
      report.push(record);
      continue;
    }

    let ok = true;
    for (const w of wanted) {
      if (!FORCE && existsSync(w.dest)) {
        const buf = await readFile(w.dest);
        if (md5(buf) === w.md5) { cached++; continue; }
        // A file that no longer matches its published checksum is not trusted;
        // it gets re-fetched rather than silently used.
        console.log(`  ${material.id}/${w.map.key}: local checksum differs, re-fetching`);
      }
      try {
        const buf = await download(w.url, w.dest);
        if (md5(buf) !== w.md5) {
          failures.push(`${material.id}/${w.map.key}: md5 mismatch after download (${w.url})`);
          ok = false;
          continue;
        }
        downloaded++;
      } catch (err) {
        failures.push(`${material.id}/${w.map.key}: ${err.message}`);
        ok = false;
      }
    }
    if (!ok) continue;

    record.fetched = new Date().toISOString().slice(0, 10);
    await writeFile(path.join(dir, 'provenance.json'), JSON.stringify(record, null, 2) + '\n');
    report.push(record);
    console.log(`✓ ${material.id.padEnd(24)} ${id.padEnd(22)} ${record.creator}`);
  }

  const manifest = {
    generated: new Date().toISOString(),
    provider: PROVIDER,
    license: LICENSE,
    count: report.length,
    assets: report
  };
  await writeFile(path.join(ROOT, 'assets', 'provenance.json'), JSON.stringify(manifest, null, 2) + '\n');

  console.log(`\n${report.length}/${MATERIALS.length} materials ${CHECK_ONLY ? 'validated' : 'resolved'}` +
    `${CHECK_ONLY ? '' : ` (${downloaded} downloaded, ${cached} already present)`}`);
  if (failures.length) {
    console.error(`\n${failures.length} problem(s):`);
    for (const f of failures) console.error(`  ✗ ${f}`);
    process.exit(1);
  }
}

main().catch(err => { console.error(err); process.exit(1); });
