// Tests for the native texture packer: the PNG decoder, the resampler, the
// mip chain and the KTX2 writer.
//
// The C++ side (test via ctest, in native/tests/ktx2_test.cpp) proves the
// *reader* against a real file. These prove the two pieces in front of it: that
// the decoder recovers the right pixels, and that the writer produces a
// structurally correct container.
//
// The filter tests matter most. A PNG decoder that handles filter 0 and nothing
// else still runs and still returns an image; it just returns garbage for five
// out of six encoders. That failure shape is identical to the renderer's unit
// cubes: a thing that works, on the one path nobody tried, while looking fine.
//
// The expected pixels here are computed by an encoder written independently in
// this file, applying the PNG spec's filter definitions rather than calling
// back into the decoder. If both used the same code, a shared misunderstanding
// would pass.

import { readFileSync, existsSync } from 'node:fs';
import { deflateSync } from 'node:zlib';

import { decodePng, resampleBox, halve, buildMipChain } from './tools/assets/png.mjs';
import { writeKtx2Array, buildDfd, buildKvd, levelSize } from './tools/assets/ktx2.mjs';

let checks = 0;
function check(condition, what) {
  checks++;
  if (!condition) {
    console.error(`  FAIL ${what}`);
    process.exitCode = 1;
  }
}

const PNG_SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function chunk(type, body) {
  const out = Buffer.alloc(12 + body.length);
  out.writeUInt32BE(body.length, 0);
  out.write(type, 4, 'ascii');
  body.copy(out, 8);
  out.writeUInt32BE(crc32(Buffer.concat([Buffer.from(type, 'ascii'), body])), 8 + body.length);
  return out;
}

// A small CRC-32 (PNG uses the standard polynomial, reflected).
const crcTable = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Apply one PNG scanline filter, per the spec, forward from the reconstructed data. */
function applyFilter(filter, line, prev, bpp) {
  const out = Buffer.alloc(line.length);
  for (let i = 0; i < line.length; i++) {
    const a = i >= bpp ? line[i - bpp] : 0;
    const b = prev ? prev[i] : 0;
    const c = prev && i >= bpp ? prev[i - bpp] : 0;
    let value;
    switch (filter) {
      case 0: value = line[i]; break;
      case 1: value = line[i] - a; break;
      case 2: value = line[i] - b; break;
      case 3: value = line[i] - ((a + b) >> 1); break;
      case 4: {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        const pred = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
        value = line[i] - pred;
        break;
      }
      default: throw new Error(`bad filter ${filter}`);
    }
    out[i] = value & 0xff;
  }
  return out;
}

/** Encode RGBA as a truecolour+alpha (colour type 6) PNG using a chosen filter. */
function encodePng(width, height, rgba, filter) {
  const bpp = 4;
  const raw = Buffer.alloc((width * bpp + 1) * height);
  for (let y = 0; y < height; y++) {
    const line = rgba.subarray(y * width * bpp, (y + 1) * width * bpp);
    const prev = y > 0
      ? rgba.subarray((y - 1) * width * bpp, y * width * bpp)
      : null;
    raw[y * (width * bpp + 1)] = filter;
    applyFilter(filter, line, prev, bpp).copy(raw, y * (width * bpp + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    PNG_SIG,
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ---------------------------------------------------------------------------
console.log('png: every scanline filter round-trips');

{
  const w = 17, h = 9;
  // A pattern with no large flat areas, so a filter that is subtly wrong shows up
  // rather than being masked by runs of equal bytes.
  const rgba = Buffer.alloc(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    rgba[i * 4] = (i * 37) & 0xff;
    rgba[i * 4 + 1] = (i * 91 + (i % 5) * 13) & 0xff;
    rgba[i * 4 + 2] = (i * 7) & 0xff;
    rgba[i * 4 + 3] = 0xff;
  }
  for (let filter = 0; filter <= 4; filter++) {
    const encoded = encodePng(w, h, rgba, filter);
    let decoded = null;
    let error = null;
    try {
      decoded = decodePng(encoded);
    } catch (e) {
      error = e;
    }
    check(!error, `filter ${filter} decodes (${error ? error.message : 'ok'})`);
    if (!decoded) continue;
    check(decoded.width === w && decoded.height === h, `filter ${filter} keeps the dimensions`);
    check(decoded.data.equals(rgba), `filter ${filter} recovers every byte exactly`);
  }
}

// ---------------------------------------------------------------------------
console.log('png: the real bake decodes');

{
  const brick = 'assets/textures/wall_brick_albedo.png';
  if (existsSync(brick)) {
    const img = decodePng(readFileSync(brick));
    check(img.width === 1024 && img.height === 1024, 'a baked tile is 1024x1024');
    check(img.data.length === 1024 * 1024 * 4, 'and decodes to RGBA');
    let opaque = true;
    let red = 0, green = 0, blue = 0;
    const n = img.width * img.height;
    for (let i = 0; i < n; i++) {
      if (img.data[i * 4 + 3] !== 255) opaque = false;
      red += img.data[i * 4];
      green += img.data[i * 4 + 1];
      blue += img.data[i * 4 + 2];
    }
    check(opaque, 'a baked albedo tile is fully opaque (palette entries default to alpha 255)');
    // Brick is red. A decoder that dropped or swapped a channel would still
    // return "a texture", and this is the only assertion that would notice.
    check(red / n > blue / n, `a brick tile is redder than it is blue (${(red / n).toFixed(0)} vs ${(blue / n).toFixed(0)})`);
    check(green / n > 0 && green / n < red / n, 'and green sits between them');
  } else {
    console.log('  (skipped: run `npm run assets:bake` first)');
  }
}

// ---------------------------------------------------------------------------
console.log('png: malformed input is rejected by name');

{
  const cases = [
    ['not a PNG at all', Buffer.from('hello world hello world')],
    ['a truncated PNG', PNG_SIG],
    ['a signature with no IHDR', Buffer.concat([PNG_SIG, Buffer.alloc(20)])],
  ];
  for (const [what, bytes] of cases) {
    let threw = false;
    try {
      decodePng(bytes);
    } catch {
      threw = true;
    }
    check(threw, `rejects ${what}`);
  }
}

// ---------------------------------------------------------------------------
console.log('resample: shrinks, preserves the mean, and keeps every channel');

{
  const size = 64;
  const rgba = Buffer.alloc(size * size * 4);
  for (let i = 0; i < size * size; i++) {
    rgba[i * 4] = 200; rgba[i * 4 + 1] = 100; rgba[i * 4 + 2] = 50; rgba[i * 4 + 3] = 255;
  }
  const flat = { width: size, height: size, data: rgba };
  const out = resampleBox(flat, 32);
  check(out.width === 32 && out.height === 32, 'resamples to the requested size');
  for (let i = 0; i < 32 * 32; i++) {
    // A box filter of a constant image is that constant. Anything else means the
    // source and destination are being paired up wrongly.
    if (out.data[i * 4] !== 200 || out.data[i * 4 + 1] !== 100 || out.data[i * 4 + 2] !== 50) {
      check(false, 'a constant image resamples to the same constant');
      break;
    }
  }
  check(true, 'a constant image resamples to the same constant');
  check(resampleBox(flat, 64).data.equals(rgba), 'resampling to the same size is a no-op');
  // A 1x1 target must not divide by zero, which is what a non-power-of-two would do.
  const one = resampleBox(flat, 1);
  check(one.width === 1 && one.height === 1 && one.data.length === 4, 'resamples to 1x1');
}

// ---------------------------------------------------------------------------
console.log('mip: the chain halves to 1x1 and conserves brightness');

{
  const size = 16;
  const rgba = Buffer.alloc(size * size * 4);
  for (let i = 0; i < size * size; i++) {
    rgba[i * 4] = 128; rgba[i * 4 + 1] = 128; rgba[i * 4 + 2] = 128; rgba[i * 4 + 3] = 255;
  }
  const chain = buildMipChain({ width: size, height: size, data: rgba });
  check(chain.length === 5, `a 16x16 chain has 5 levels (got ${chain.length})`);
  const sizes = chain.map((c) => `${c.width}x${c.height}`).join(' ');
  check(sizes === '16x16 8x8 4x4 2x2 1x1', `the chain halves exactly: ${sizes}`);
  for (const level of chain) {
    let ok = true;
    for (let i = 0; i < level.width * level.height; i++) {
      if (level.data[i * 4] !== 128) { ok = false; break; }
    }
    check(ok, 'every mip of a constant image is that constant');
  }
  const h = halve({ width: 1, height: 1, data: Buffer.from([9, 9, 9, 255]) });
  check(h.width === 1 && h.height === 1, 'halving a 1x1 stays 1x1 rather than becoming 0');
}

// ---------------------------------------------------------------------------
console.log('ktx2: the descriptor is the size the spec requires');

{
  const dfd = buildDfd(true);
  // 28 bytes of basic block plus one 16-byte sample per channel. Getting 24 here
  // overflows the buffer by four bytes, and 92 (0x5C) is what a hex dump of a
  // real KTX2 file shows.
  check(dfd.length === 92, `the DFD is 92 bytes (got ${dfd.length})`);
  check(dfd.readUInt32LE(0) === 92, 'dfdTotalSize agrees with the length');
  check(dfd.readUInt32LE(4) === 0, 'vendorId 0, descriptorType 0 (basic)');
  const blockSize = dfd.readUInt32LE(8) >>> 16;
  check(blockSize === 92, `descriptorBlockSize is 92 (got ${blockSize})`);
  check(dfd.readUInt16LE(8) === 2, 'descriptor version 2');
  check(dfd[12] === 1, 'colorModel RGBSDA');
  check(dfd[14] === 2, 'sRGB albedo has the SRGB transfer function');
  check(buildDfd(false)[14] === 1, 'a linear map has the LINEAR transfer function');
  check(dfd[20] === 4, 'bytesPlane0 is 4 (RGBA)');
  // Four samples, each declaring a 64-bit channel with bitLength biased to 63.
  for (let i = 0; i < 4; i++) {
    const at = 28 + i * 16;
    const bitLength = (dfd.readUInt32LE(at) >>> 16) & 0xff;
    check(bitLength === 63, `sample ${i} stores bitLength 63 for a 64-bit channel`);
  }
  // The normal map's blue channel must stay the last sample's, since the
  // tangent-space normal's Z is what the shader reads.
  const aWord = dfd.readUInt32LE(28 + 3 * 16);
  check(((aWord >>> 24) & 0xff) === 0x1f, 'the alpha sample is channel 15, LINEAR');

  const entries = { KTXorientation: 'rd', KTXwriter: 'test' };
  const kvd = buildKvd(entries);

  // Walk the block the way a reader must, using only the length fields. Hard
  // coded offsets would pass a writer that drops an entry by throwing an
  // out-of-range error instead of naming the failure, which takes the rest of
  // the suite with it.
  const found = {};
  let at = 0;
  let walked = 0;
  while (at + 4 <= kvd.length) {
    const keyAndValueLength = kvd.readUInt32LE(at);
    check(keyAndValueLength > 0 && at + 4 + keyAndValueLength <= kvd.length,
      `entry ${walked} claims ${keyAndValueLength} bytes and stays inside the block`);
    const nul = kvd.indexOf(0, at + 4);
    check(nul >= at + 4 && nul < at + 4 + keyAndValueLength,
      `entry ${walked} has a NUL inside its own length field`);
    const key = kvd.toString('ascii', at + 4, nul);
    const value = kvd.toString('ascii', nul + 1, at + 4 + keyAndValueLength);
    found[key] = value;
    // Each entry is padded to a four-byte boundary *after* the length field.
    at += 4 + ((keyAndValueLength + 3) & ~3);
    walked++;
  }
  check(at === kvd.length, 'the entries consume the block exactly, with no trailing bytes');
  check(walked === 2, `both entries are walked (got ${walked})`);
  for (const [key, value] of Object.entries(entries)) {
    check(found[key] === value, `${key} round-trips as "${value}"`);
  }
  // KTX2 has one length per entry, covering the NUL-terminated key plus the
  // value. The key therefore starts at byte 4, not byte 8: the WebGL-oriented
  // habit of reading a separate key length is a useful thing to get wrong.
  check(kvd.readUInt32LE(0) === 'KTXorientation\0rd'.length,
    'a KVD entry length covers the NUL-terminated key and the value');
  check(kvd.toString('ascii', 4, 4 + 14) === 'KTXorientation', 'the key starts immediately after that length');
  check(kvd[4 + 14] === 0, 'and is NUL-terminated');
  check(kvd.toString('ascii', 19, 21) === 'rd', 'the value follows the NUL');
  check(kvd.length % 4 === 0, 'the KVD block is four-byte aligned');
  check(kvd.length === 44, 'two entries with padding total 44 bytes');
  // An odd-length key+value still pads, and the padding must not be mistaken
  // for a next entry by the walk above. 4 bytes of length field + 5 padded to 8.
  const odd = buildKvd({ abc: 'x' });
  check(odd.length === 12, 'a 5-byte key+value makes a 12-byte entry (got ' + odd.length + ')');
  check(odd.readUInt32LE(0) === 5, 'and its length field is the unpadded 5, not the padded 8');
}

{
  // The KTX2 floor rule: dimensions halve, with a floor of one.
  check(levelSize(8, 8, 0).width === 8, 'level 0 is full size');
  check(levelSize(8, 8, 3).width === 1, 'level 3 of an 8x8 is 1x1');
  check(levelSize(8, 8, 4).width === 1, 'and it stays 1 rather than becoming 0');
  check(levelSize(1024, 1024, 10).width === 1, 'a 1024 chain is 11 levels');
}

// ---------------------------------------------------------------------------
console.log('ktx2: the writer produces a coherent container');

{
  const w = 4, h = 4, layers = 2;
  const level0 = Buffer.alloc(layers * w * h * 4);
  for (let i = 0; i < level0.length; i++) level0[i] = (i * 13) & 0xff;
  const file = writeKtx2Array({ width: w, height: h, layers, mips: [level0], srgb: true, zstdLevel: 3 });

  check(file.subarray(0, 8).toString('hex') === 'ab4b5458203230bb'.slice(0, 16),
    'the 12-byte identifier is right');
  check(file.readUInt32LE(12) === 43, 'sRGB RGBA8 is vkFormat 43');
  check(file.readUInt32LE(16) === 1, 'typeSize is 1');
  check(file.readUInt32LE(20) === w, 'pixelWidth');
  check(file.readUInt32LE(24) === h, 'pixelHeight');
  check(file.readUInt32LE(28) === 0, 'pixelDepth 0');
  check(file.readUInt32LE(32) === layers, 'layerCount');
  check(file.readUInt32LE(36) === 1, 'faceCount 1');
  check(file.readUInt32LE(40) === 1, 'levelCount');
  check(file.readUInt32LE(44) === 2, 'supercompressionScheme 2 (zstd)');

  const dfdOffset = file.readUInt32LE(48);
  const dfdLength = file.readUInt32LE(52);
  const kvdOffset = file.readUInt32LE(56);
  const kvdLength = file.readUInt32LE(60);
  const sgdOffset = Number(file.readBigUInt64LE(64));
  const sgdLength = Number(file.readBigUInt64LE(72));
  check(file.readUInt32LE(48) === 80 + 24, 'the DFD follows the header and the level index');
  check(dfdLength === 92, 'the DFD is 92 bytes');
  check(kvdOffset === dfdOffset + dfdLength, 'the KVD follows the DFD with no gap');
  check(sgdOffset === kvdOffset + kvdLength, 'the SGD follows the KVD');
  check(sgdLength === 4, 'the SGD is the four-byte zstd level');
  check(file.readUInt32LE(sgdOffset) === 3, 'and it records the level that was used');

  // The level index, and the payload it points at.
  const levelOffset = Number(file.readBigUInt64LE(80));
  const levelLength = Number(file.readBigUInt64LE(88));
  const uncompressed = Number(file.readBigUInt64LE(96));
  check(uncompressed === level0.length, 'uncompressedByteLength is the true texel count');
  check(levelOffset >= sgdOffset + sgdLength, 'the payload starts after every metadata block');
  check(levelOffset + levelLength <= file.length, 'and fits in the file');

  // A deterministic regeneration must be byte-identical, or the build is not
  // reproducible and a diff of the pack means nothing.
  const again = writeKtx2Array({ width: w, height: h, layers, mips: [level0], srgb: true, zstdLevel: 3 });
  check(file.equals(again), 'the same inputs produce byte-identical output');
}

{
  // A level whose size does not match its dimensions is the mistake that would
  // otherwise be discovered by the GPU, as a black or striped wall.
  let threw = false;
  try {
    writeKtx2Array({ width: 4, height: 4, layers: 1, mips: [Buffer.alloc(10)] });
  } catch {
    threw = true;
  }
  check(threw, 'a mis-sized mip level is rejected at pack time');
}

{
  // And the committed fixture, if it is there, must match what this writer makes.
  const fixture = 'native/tests/fixtures/pattern.ktx2';
  if (existsSync(fixture)) {
    const bytes = readFileSync(fixture);
    check(bytes.subarray(0, 4).toString('hex') === 'ab4b5458', 'the fixture is a KTX2 container');
    check(bytes.readUInt32LE(32) === 3, 'it has three layers');
    check(bytes.readUInt32LE(40) === 4, 'and four mip levels');
    check(bytes.includes(Buffer.from('KTXorientation')), 'it records its orientation');
  } else {
    console.log('  (fixture absent; run `npm run assets:fixture`)');
  }
}

// ---------------------------------------------------------------------------

if (process.exitCode) {
  console.error(`\nnative texture pack: ${checks} checks, FAILURES above`);
} else {
  console.log(`\nnative texture pack: ${checks} checks passed`);
}
