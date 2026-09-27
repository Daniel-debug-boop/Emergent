// A KTX2 writer.
//
// WHY WRITE A CONTAINER BY HAND. The alternative is vendoring KTX-Software, and
// that is a large C++ project with its own build system, its own CMake
// requirements and a tool dependency this repository does not otherwise have.
// For three uncompressed-RGBA8 array textures with a mip chain, the container
// is a 80-byte header, a level index, a data format descriptor, a key/value
// block, a supercompression block and the level data. That is a few hundred
// lines, it is fully specified, and it can be tested to byte level.
//
// WHAT IS NOT CLAIMED. Nothing here has been validated against KTX-Software,
// `ktx2check`, or the Khronos KTX parser, because none of them is available in
// this environment. What *is* claimed, and tested, is that this writer and the
// native reader agree exactly, and that the reader is defensive about every
// field it trusts. See docs/ENGINE_VERIFICATION.md.
//
// The supercompression scheme used is 2 (Zstandard), because the reader links
// the vendored libzstd, and because a 26-layer array of 1024² RGBA is 415 MB
// raw, which is not a thing to ship.

import { zstdCompressSync, constants as zlibConstants } from 'node:zlib';

/** The 12-byte file signature. */
const IDENTIFIER = Buffer.from([
  0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a,
]);

export const SUPERCOMPRESSION_NONE = 0;
export const SUPERCOMPRESSION_ZSTD = 2;

export const VK_FORMAT_R8G8B8A8_UNORM = 37;
export const VK_FORMAT_R8G8B8A8_SRGB = 43;

/** KHR_DF enums, spelled out so the bytes below can be read against the spec. */
const COLOR_MODEL_RGBSDA = 1;
const PRIMARIES_BT709 = 1;
const TRANSFER_LINEAR = 1;
const TRANSFER_SRGB = 2;
const SAMPLE_DATATYPE_LINEAR = 0x10;
const CHANNEL_R = 0;
const CHANNEL_G = 1;
const CHANNEL_B = 2;
const CHANNEL_A = 15;

/**
 * The basic data format descriptor for 8-bit RGBA.
 *
 * 28 bytes of block, then one 16-byte sample per channel: 92 bytes in total,
 * which is the 0x5C that a hex dump of a real KTX2 file shows. Getting 24 here
 * instead — by forgetting that bytesPlane0..7 is eight bytes rather than four —
 * overflows the buffer by exactly the four bytes of the last sample, so the
 * block size is not a cosmetic constant.
 *
 * `bitLength` is stored biased by one (63 means 64), which is a KTX2 quirk
 * rather than a typo.
 */
export function buildDfd(srgb) {
  const basicBlockBytes = 28;
  const sampleBytes = 16;
  const samples = 4;
  const blockSize = basicBlockBytes + samples * sampleBytes;

  const dfd = Buffer.alloc(blockSize);
  let o = 0;
  dfd.writeUInt32LE(blockSize, o); o += 4;          // dfdTotalSize
  dfd.writeUInt32LE(0, o); o += 4;                   // vendorId 0, descriptorType 0 (basic)
  dfd.writeUInt32LE((2 & 0xffff) | (blockSize << 16), o); o += 4; // version 2, blockSize
  dfd.writeUInt8(COLOR_MODEL_RGBSDA, o++);
  dfd.writeUInt8(PRIMARIES_BT709, o++);
  dfd.writeUInt8(srgb ? TRANSFER_SRGB : TRANSFER_LINEAR, o++);
  dfd.writeUInt8(0, o++);                            // flags: alpha straight
  dfd.writeUInt8(0, o++);                            // texelBlockDimension0
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(4, o++);                            // bytesPlane0: RGBA
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);
  dfd.writeUInt8(0, o++);

  const channelTypes = [CHANNEL_R, CHANNEL_G, CHANNEL_B, CHANNEL_A];
  for (const channelType of channelTypes) {
    dfd.writeUInt32LE(0 | (63 << 16) | ((channelType | SAMPLE_DATATYPE_LINEAR) << 24), o);
    o += 4;
    dfd.writeUInt32LE(0, o); o += 4;                 // sample positions
    dfd.writeUInt32LE(0, o); o += 4;
    dfd.writeUInt32LE(0, o); o += 4;
  }
  return dfd;
}

/**
 * The key/value block.
 *
 * Each entry is a length, a NUL-terminated key, the value, and padding to a
 * four-byte boundary. The length covers the key *including* its NUL and the
 * value, but not the length field itself — getting that off by four is the
 * single most common way a hand-written KVD block is rejected.
 */
export function buildKvd(entries) {
  const parts = [];
  for (const [key, value] of Object.entries(entries)) {
    const keyBytes = Buffer.from(key + '\0', 'utf8');
    const valueBytes = Buffer.from(value, 'utf8');
    const unpadded = keyBytes.length + valueBytes.length;
    const padded = (unpadded + 3) & ~3;
    const entry = Buffer.alloc(4 + padded);
    entry.writeUInt32LE(unpadded, 0);
    keyBytes.copy(entry, 4);
    valueBytes.copy(entry, 4 + keyBytes.length);
    parts.push(entry);
  }
  return Buffer.concat(parts);
}

/** The level index: byteOffset, byteLength, uncompressedByteLength per level. */
function buildLevelIndex(levels) {
  const index = Buffer.alloc(levels.length * 24);
  let o = 0;
  for (const level of levels) {
    index.writeBigUInt64LE(BigInt(level.byteOffset), o); o += 8;
    index.writeBigUInt64LE(BigInt(level.byteLength), o); o += 8;
    index.writeBigUInt64LE(BigInt(level.uncompressedByteLength), o); o += 8;
  }
  return index;
}

/** Bytes of a mip level, largest first, matching the KTX2 floor rule. */
export function levelSize(width, height, level) {
  return {
    width: Math.max(1, Math.floor(width / 2 ** level)),
    height: Math.max(1, Math.floor(height / 2 ** level)),
  };
}

/**
 * Write a 2D array texture.
 *
 * @param {object} options
 * @param {number} options.width            texels, per layer
 * @param {number} options.height
 * @param {number} options.layers
 * @param {Buffer[]} options.mips            mip chain, largest first; each is
 *                                           `layers * w * h * 4` bytes
 * @param {boolean} [options.srgb]
 * @param {number}  [options.zstdLevel]      0 disables supercompression
 * @param {object}  [options.kvd]            extra key/value metadata
 * @returns {Buffer}
 */
export function writeKtx2Array(options) {
  const {
    width, height, layers, mips, srgb = true, zstdLevel = 9, kvd = {},
  } = options;

  if (!mips.length) throw new Error('ktx2: at least one mip level is required');
  if (mips.length > 12) throw new Error(`ktx2: ${mips.length} levels is more than the format allows`);
  for (let i = 0; i < mips.length; i++) {
    const { width: w, height: h } = levelSize(width, height, i);
    const expected = layers * w * h * 4;
    if (mips[i].length !== expected) {
      throw new Error(
        `ktx2: mip ${i} is ${w}x${h} so ${layers} layers need ${expected} bytes, got ${mips[i].length}`,
      );
    }
  }

  const useZstd = zstdLevel > 0;
  const scheme = useZstd ? SUPERCOMPRESSION_ZSTD : SUPERCOMPRESSION_NONE;
  const dfd = buildDfd(srgb);
  const kvdBlock = buildKvd({
    // Required by the spec. "rd" is right then down, which is the orientation
    // every Vulkan image upload path expects and the one a reader getting it
    // wrong would flip every normal map vertically.
    KTXorientation: 'rd',
    KTXwriter: 'EMERGENT tools/assets/ktx2.mjs',
    ...kvd,
  });
  const sgd = useZstd ? (() => {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(zstdLevel, 0);
    return b;
  })() : Buffer.alloc(0);

  // The content checksum is off by default, and that is not a size saving worth
  // making here. Without it zstd will happily decode a corrupted entropy-coded
  // frame and report success: flipping four bytes in the middle of a level was
  // accepted by the native reader before this was enabled, and the wall came
  // out with the wrong bricks and no error anywhere. With the flag, zstd
  // verifies the frame on decompression and the same corruption is rejected.
  // Four bytes per level, against a texture that is silently wrong.
  const encoded = mips.map((mip) => {
    if (!useZstd) return { data: mip, uncompressed: mip.length };
    const data = zstdCompressSync(mip, {
      params: { [zlibConstants.ZSTD_c_compressionLevel]: zstdLevel,
                [zlibConstants.ZSTD_c_checksumFlag]: 1 },
    });
    return { data, uncompressed: mip.length };
  });

  const levelIndexBytes = mips.length * 24;
  const dfdOffset = 80 + levelIndexBytes;
  const kvdOffset = dfdOffset + dfd.length;
  const sgdOffset = kvdOffset + kvdBlock.length;

  // Level data follows the metadata blocks, in level order.
  const levels = [];
  let cursor = sgdOffset + sgd.length;
  for (const { data, uncompressed } of encoded) {
    levels.push({ byteOffset: cursor, byteLength: data.length, uncompressedByteLength: uncompressed });
    cursor += data.length;
  }
  if (cursor > 0xffffffff) {
    throw new Error(`ktx2: file would be ${cursor} bytes, over the 4 GiB the index can address`);
  }

  const header = Buffer.alloc(80);
  IDENTIFIER.copy(header, 0);
  let o = 12;
  header.writeUInt32LE(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, o); o += 4;
  header.writeUInt32LE(1, o); o += 4;                 // typeSize, 1 byte per component
  header.writeUInt32LE(width, o); o += 4;
  header.writeUInt32LE(height, o); o += 4;
  header.writeUInt32LE(0, o); o += 4;                 // pixelDepth
  header.writeUInt32LE(layers, o); o += 4;
  header.writeUInt32LE(1, o); o += 4;                 // faceCount
  header.writeUInt32LE(mips.length, o); o += 4;
  header.writeUInt32LE(scheme, o); o += 4;
  header.writeUInt32LE(dfdOffset, o); o += 4;
  header.writeUInt32LE(dfd.length, o); o += 4;
  header.writeUInt32LE(kvdOffset, o); o += 4;
  header.writeUInt32LE(kvdBlock.length, o); o += 4;
  header.writeBigUInt64LE(BigInt(sgdOffset), o); o += 8;
  header.writeBigUInt64LE(BigInt(sgd.length), o); o += 8;

  // sgdOffset is by construction 80 + levelIndex + dfd + kvd, so the blocks are
  // already contiguous and no padding is needed between them.
  return Buffer.concat([
    header,
    buildLevelIndex(levels),
    dfd,
    kvdBlock,
    sgd,
    ...encoded.map((e) => e.data),
  ]);
}
