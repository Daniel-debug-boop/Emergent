/**
 * Tileability and channel statistics for a raw RGBA image.
 *
 * Split out of `bake.mjs` and `audit.mjs` so the same measurement is used when
 * choosing a material, when baking it, and when auditing what shipped. A check
 * that exists in three copies is a check that eventually disagrees with itself.
 *
 * ## Why tileability is measured rather than assumed
 *
 * Poly Haven publishes most textures as tileable, but not all of them, and the
 * ones that are not are the expensive mistake: a road or a wall with a visible
 * grid across it reads as broken, and it reads as broken across the *entire*
 * surface, not in one corner where somebody might notice.
 *
 * The measurement compares the difference between the two pixels that end up
 * adjacent once the tile wraps, against the difference between typical
 * neighbouring pixels inside the tile. A seamless texture has no special edge,
 * so the ratio is about 1. A texture with a hard border has a wrap difference
 * many times the interior one.
 *
 * Raw RGBA, row-major, tightly packed. Interleaved because every caller has it
 * that way and unpacking to planar would allocate 4x the memory for no gain.
 */

/** Mean absolute RGB difference between two columns of a packed RGBA buffer. */
export function columnDifference(raw, width, height, a, b) {
  let sum = 0;
  for (let y = 0; y < height; y++) {
    const pa = (y * width + a) * 4, pb = (y * width + b) * 4;
    sum += Math.abs(raw[pa] - raw[pb]) + Math.abs(raw[pa + 1] - raw[pb + 1]) + Math.abs(raw[pa + 2] - raw[pb + 2]);
  }
  return sum / (height * 3);
}

/** Mean absolute RGB difference between two rows of a packed RGBA buffer. */
export function rowDifference(raw, width, height, a, b) {
  let sum = 0;
  for (let x = 0; x < width; x++) {
    const pa = (a * width + x) * 4, pb = (b * width + x) * 4;
    sum += Math.abs(raw[pa] - raw[pb]) + Math.abs(raw[pa + 1] - raw[pb + 1]) + Math.abs(raw[pa + 2] - raw[pb + 2]);
  }
  return sum / (width * 3);
}

/**
 * How much harder the wrap edge is than an ordinary neighbour.
 *
 * 1.0 means the tile has no visible border at all. The ratio is reported per
 * axis because a texture that tiles left-to-right often does not tile
 * top-to-bottom, and a builder that only checks one axis ships the other.
 */
export function seamRatio(raw, width, height, samples = 24) {
  const step = Math.max(1, Math.floor(width / samples));
  let interior = 0, n = 0;
  for (let x = 1; x < width - 2; x += step) {
    interior += columnDifference(raw, width, height, x, x + 1);
    n++;
  }
  const interiorMean = interior / Math.max(1, n);
  const hWrap = columnDifference(raw, width, height, 0, width - 1) / Math.max(1e-6, interiorMean);
  const vWrap = rowDifference(raw, width, height, 0, height - 1) / Math.max(1e-6, interiorMean);
  return { hWrap, vWrap, ratio: Math.max(hWrap, vWrap), interiorMean };
}

/**
 * Per-channel means, overall luma and luma deviation.
 *
 * Luma standard deviation is the degeneracy check: a texture that is one flat
 * colour has a standard deviation near zero and is a failed download or a
 * placeholder, and a provider will happily publish one.
 */
export function channelStats(raw, width, height) {
  const n = width * height;
  let r = 0, g = 0, b = 0, lsum = 0;
  for (let i = 0, p = 0; i < n; i++, p += 4) {
    r += raw[p]; g += raw[p + 1]; b += raw[p + 2];
    lsum += 0.2126 * raw[p] + 0.7152 * raw[p + 1] + 0.0722 * raw[p + 2];
  }
  const lmean = lsum / n;
  let lvar = 0;
  for (let i = 0, p = 0; i < n; i++, p += 4) {
    const d = (0.2126 * raw[p] + 0.7152 * raw[p + 1] + 0.0722 * raw[p + 2]) - lmean;
    lvar += d * d;
  }
  return {
    mean: { r: r / n / 255, g: g / n / 255, b: b / n / 255 },
    lmean: lmean,
    lstd: Math.sqrt(lvar / n)
  };
}
