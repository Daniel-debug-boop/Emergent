// A PNG decoder, limited to what this repository's bake actually emits.
//
// WHY NOT AN EXISTING ONE. The obvious answer is a dependency, and there are
// good ones. But the input here is completely known: 78 files, every one of
// them 1024x1024, 8-bit, colour type 3 (palette), produced by our own bake from
// Poly Haven sources. A general decoder would be several hundred lines of
// interlacing, Adam7, 16-bit and 1/2/4-bit paths that can never be reached and
// can therefore never be tested, which is worse than not having them: untested
// code in an asset path is an asset path that fails on the one file nobody
// tried.
//
// So this decodes the eight-bit non-interlaced colour types 0, 2, 3 and 6, and
// throws a named error on anything else rather than producing wrong pixels.
// The palette path is the one that matters here; the others are cheap and make
// the module usable on a hand-made fixture.
//
// Every filter is implemented. That is not optional: a decoder that handles
// filter 0 only still runs, still returns an image, and returns garbage for
// five out of six encoders, which is the same failure shape as the renderer's
// unit cubes.

import { inflateSync } from 'node:zlib';

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Channels per PNG colour type, for the eight-bit types we accept. */
const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

function paethPredictor(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

/**
 * Reverse the per-scanline filters, in place.
 *
 * PNG filters are predictive and depend on the *reconstructed* bytes of the
 * previous pixel and the previous scanline, not on the stored bytes, so this
 * has to run strictly left to right, top to bottom. Getting that order wrong
 * yields an image that looks almost right, which is the worst outcome.
 */
function unfilter(raw, width, height, bytesPerPixel, bytesPerRow) {
  const out = Buffer.alloc(bytesPerRow * height);
  let src = 0;
  for (let y = 0; y < height; y++) {
    const filter = raw[src++];
    const rowStart = y * bytesPerRow;
    const prevStart = rowStart - bytesPerRow;
    for (let x = 0; x < bytesPerRow; x++) {
      const value = raw[src + x];
      const left = x >= bytesPerPixel ? out[rowStart + x - bytesPerPixel] : 0;
      const up = y > 0 ? out[prevStart + x] : 0;
      const upLeft = y > 0 && x >= bytesPerPixel ? out[prevStart + x - bytesPerPixel] : 0;
      let result;
      switch (filter) {
        case 0: result = value; break;
        case 1: result = value + left; break;
        case 2: result = value + up; break;
        case 3: result = value + ((left + up) >> 1); break;
        case 4: result = value + paethPredictor(left, up, upLeft); break;
        default:
          throw new Error(`png: unknown scanline filter ${filter} on row ${y}`);
      }
      out[rowStart + x] = result & 0xff;
    }
    src += bytesPerRow;
  }
  return out;
}

/**
 * Decode a PNG into straight (non-premultiplied) 8-bit RGBA.
 *
 * @returns {{width: number, height: number, data: Buffer}} `data` is
 *   `width * height * 4` bytes, row-major, top row first.
 */
export function decodePng(buffer) {
  if (buffer.length < 8 || !buffer.subarray(0, 8).equals(PNG_SIGNATURE)) {
    throw new Error('png: not a PNG (bad signature)');
  }

  let offset = 8;
  let header = null;
  let palette = null;
  let transparency = null;
  const idat = [];

  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const body = buffer.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;

    if (type === 'IHDR') {
      if (length < 13) throw new Error('png: IHDR is too short');
      header = {
        width: body.readUInt32BE(0),
        height: body.readUInt32BE(4),
        bitDepth: body[8],
        colourType: body[9],
        interlace: body[12],
      };
    } else if (type === 'PLTE') {
      palette = Buffer.from(body);
    } else if (type === 'tRNS') {
      transparency = Buffer.from(body);
    } else if (type === 'IDAT') {
      idat.push(Buffer.from(body));
    } else if (type === 'IEND') {
      break;
    }
  }

  if (!header) throw new Error('png: no IHDR');
  if (header.interlace !== 0) {
    throw new Error('png: interlaced images are not supported (nothing in this bake emits them)');
  }
  if (header.bitDepth !== 8) {
    throw new Error(`png: bit depth ${header.bitDepth} is not supported (this bake emits 8)`);
  }
  const channels = CHANNELS[header.colourType];
  if (!channels) {
    throw new Error(`png: colour type ${header.colourType} is not supported`);
  }

  const { width, height } = header;
  if (!width || !height) throw new Error('png: zero-sized image');

  const bytesPerRow = width * channels;
  const raw = inflateSync(Buffer.concat(idat));
  const expected = (bytesPerRow + 1) * height;
  if (raw.length < expected) {
    // A short inflate is a truncated or corrupt file. Producing a partially
    // filled image here would be the classic "looks fine until you look at the
    // corner" texture bug.
    throw new Error(`png: inflated ${raw.length} bytes, expected ${expected}`);
  }
  const flat = unfilter(raw, width, height, channels, bytesPerRow);

  const data = Buffer.alloc(width * height * 4);
  for (let i = 0, n = width * height; i < n; i++) {
    const s = i * channels;
    const d = i * 4;
    switch (header.colourType) {
      case 0: // greyscale
        data[d] = data[d + 1] = data[d + 2] = flat[s];
        data[d + 3] = 255;
        break;
      case 2: // truecolour
        data[d] = flat[s];
        data[d + 1] = flat[s + 1];
        data[d + 2] = flat[s + 2];
        data[d + 3] = 255;
        break;
      case 3: { // palette
        if (!palette) throw new Error('png: colour type 3 with no PLTE');
        const index = flat[s];
        const p = index * 3;
        if (p + 2 >= palette.length) {
          throw new Error(`png: palette index ${index} is past the end of PLTE`);
        }
        data[d] = palette[p];
        data[d + 1] = palette[p + 1];
        data[d + 2] = palette[p + 2];
        // A palette entry with no tRNS entry is fully opaque. Defaulting it to
        // transparent instead is the bug that makes every alpha-cutout
        // material render as a hole.
        data[d + 3] = transparency && index < transparency.length ? transparency[index] : 255;
        break;
      }
      case 4: // greyscale + alpha
        data[d] = data[d + 1] = data[d + 2] = flat[s];
        data[d + 3] = flat[s + 1];
        break;
      case 6: // truecolour + alpha
        data[d] = flat[s];
        data[d + 1] = flat[s + 1];
        data[d + 2] = flat[s + 2];
        data[d + 3] = flat[s + 3];
        break;
      default:
        throw new Error(`png: colour type ${header.colourType} is not supported`);
    }
  }
  return { width, height, data };
}

/**
 * Box-filter a resample to `size` x `size`.
 *
 * A box filter rather than a bilinear one on purpose. These are tileable
 * materials, and a resample that does not wrap the edges breaks the tile: a
 * bilinear tap at x = width-1 reads the first pixel of the *same* row, which is
 * not the neighbouring texel of the wrap, and the seam appears as a bright or
 * dark hairline that the shader's box mapping then repeats across every wall.
 *
 * Averaging over the wrapped source range is what makes the output tileable at
 * the same period as the input.
 */
export function resampleBox(image, size) {
  const { width, height, data } = image;
  if (width === size && height === size) return image;

  const out = Buffer.alloc(size * size * 4);
  const xRatio = width / size;
  const yRatio = height / size;
  for (let y = 0; y < size; y++) {
    const y0 = Math.floor(y * yRatio);
    const y1 = Math.max(y0 + 1, Math.min(height, Math.ceil((y + 1) * yRatio)));
    for (let x = 0; x < size; x++) {
      const x0 = Math.floor(x * xRatio);
      const x1 = Math.max(x0 + 1, Math.min(width, Math.ceil((x + 1) * xRatio)));
      let r = 0, g = 0, b = 0, a = 0, n = 0;
      for (let sy = y0; sy < y1; sy++) {
        for (let sx = x0; sx < x1; sx++) {
          const s = (sy * width + sx) * 4;
          r += data[s]; g += data[s + 1]; b += data[s + 2]; a += data[s + 3];
          n++;
        }
      }
      const d = (y * size + x) * 4;
      out[d] = Math.round(r / n);
      out[d + 1] = Math.round(g / n);
      out[d + 2] = Math.round(b / n);
      out[d + 3] = Math.round(a / n);
    }
  }
  return { width: size, height: size, data: out };
}

/**
 * Halve an image for the next mip level.
 *
 * The 2x2 average, not a box resample at size/2: they agree, but the average is
 * the definition of the mip chain and using the same code for both would hide a
 * bug in one of them behind the other.
 */
export function halve(image) {
  const { width, height, data } = image;
  const w = Math.max(1, width >> 1);
  const h = Math.max(1, height >> 1);
  const out = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const x0 = Math.min(width - 1, x * 2);
      const y0 = Math.min(height - 1, y * 2);
      const x1 = Math.min(width - 1, x0 + 1);
      const y1 = Math.min(height - 1, y0 + 1);
      const a = (y0 * width + x0) * 4;
      const b = (y0 * width + x1) * 4;
      const c = (y1 * width + x0) * 4;
      const e = (y1 * width + x1) * 4;
      const d = (y * w + x) * 4;
      out[d] = Math.round((data[a] + data[b] + data[c] + data[e]) / 4);
      out[d + 1] = Math.round((data[a + 1] + data[b + 1] + data[c + 1] + data[e + 1]) / 4);
      out[d + 2] = Math.round((data[a + 2] + data[b + 2] + data[c + 2] + data[e + 2]) / 4);
      out[d + 3] = Math.round((data[a + 3] + data[b + 3] + data[c + 3] + data[e + 3]) / 4);
    }
  }
  return { width: w, height: h, data: out };
}

/** The full mip chain, largest first, down to 1x1. */
export function buildMipChain(image) {
  const chain = [image];
  let current = image;
  while (current.width > 1 || current.height > 1) {
    current = halve(current);
    chain.push(current);
  }
  return chain;
}
