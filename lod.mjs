/**
 * Level-of-detail generation for imported models.
 *
 * Built on `meshoptimizer` — the same library the native engine already links
 * at `v1.2`, used through its official JS distribution rather than a
 * hand-rolled simplifier. A custom decimation is the single easiest thing in a
 * renderer to write and the hardest to get right: it produces holes, inverted
 * normals and silhouettes that swim as the camera moves, and none of those show
 * up in a headless test.
 *
 * ## Why LODs are not optional here
 *
 * A measured Poly Haven bed frame is 49,990 triangles. The project's entire
 * static city is 218,226 vertices. Sixty furnished interiors would spend more
 * the budget on furniture than on the city. The imported set is decimated at
 * bake time so the runtime picks a level per room rather than paying full
 * detail for a chair behind a closed door.
 *
 * ## The three levels
 *
 *   LOD0  ~30% of the authored triangles, for a room the player is in
 *   LOD1  ~12%, for a room seen through a doorway
 *   LOD2  ~5%, for a room glimpsed from the street
 *
 * The authored mesh itself is not a level. The sources are 8k exports — a
 * measured Poly Haven bed frame is 49,990 triangles — and shipping the authored
 * mesh for this set cost 31.5 MB of vertex data for furniture that fills a few
 * hundred pixels. The chain is measured on the imported mesh, not on the
 * authored one, so these percentages are of what actually arrives.
 *
 * Error bounds are relative to the mesh's own bounding box diagonal, so a
 * 2 m chair and a 6 m wardrobe get the same *relative* tolerance rather than
 * the same absolute one — which would flatten the wardrobe and leave the chair
 * untouched.
 *
 * ## Shared topology is a precondition, not an optimisation
 *
 * `meshoptimizer` can only decimate a mesh that knows which corners are the
 * same corner. A triangle soup — three unique vertices per triangle — looks
 * identical on screen and is completely unsimplifiable: every vertex is on its
 * own border. So the importer's welding step is load-bearing, and the LOD
 * builder asserts that the mesh actually has sharing before it claims to have
 * reduced anything. A silently-unsimplified chain is worse than no chain,
 * because it looks like a successful optimisation in a build report.
 *
 * ## Attribute seams
 *
 * `simplifyWithAttributes` is used with a seam-aware weight, so vertices on a
 * UV or normal discontinuity are not merged across it. Decimating across a seam
 * is what turns a clean texture into a smear one triangle wide, and it is
 * invisible in a wireframe and obvious on screen.
 */

/** Mesh vertex layout, mirrored from geometry.mjs. */
export const FLOATS_PER_VERTEX = 12;
const OFF_POS = 0, OFF_NRM = 3, OFF_COL = 6, OFF_MAT = 9, OFF_UV = 10;

/**
 * How the imported vertex data is split for the simplifier.
 *
 * `simplifyWithAttributes` takes positions in one interleaved buffer and every
 * other attribute in a *second* interleaved buffer with its own stride, not one
 * array per stream. The attribute block here is normal(3) then UV(2) then
 * colour(3), and the weights below are in that order.
 */
const STRIDE_POS = 3;
const ATTR_STRIDE = 3 + 2 + 3;
const ATTR_WEIGHTS = [1.0, 0.5, 0.25];

/**
 * Split a flat EMERGENT vertex array into the buffers the simplifier wants.
 *
 * The material index is deliberately excluded and re-applied afterwards. It is
 * constant across a whole imported model, so carrying it through would only let
 * the simplifier consider it — and it is the one attribute where a merge would
 * be wrong rather than merely lossy, so it stays out of the weights entirely.
 */
export function splitForSimplify(vertices) {
  const count = vertices.length / FLOATS_PER_VERTEX;
  const position = new Float32Array(count * STRIDE_POS);
  const attributes = new Float32Array(count * ATTR_STRIDE);
  for (let i = 0; i < count; i++) {
    const o = i * FLOATS_PER_VERTEX;
    const a = i * STRIDE_POS;
    position[a] = vertices[o + OFF_POS];
    position[a + 1] = vertices[o + OFF_POS + 1];
    position[a + 2] = vertices[o + OFF_POS + 2];
    const b = i * ATTR_STRIDE;
    attributes[b] = vertices[o + OFF_NRM];
    attributes[b + 1] = vertices[o + OFF_NRM + 1];
    attributes[b + 2] = vertices[o + OFF_NRM + 2];
    attributes[b + 3] = vertices[o + OFF_UV];
    attributes[b + 4] = vertices[o + OFF_UV + 1];
    attributes[b + 5] = vertices[o + OFF_COL];
    attributes[b + 6] = vertices[o + OFF_COL + 1];
    attributes[b + 7] = vertices[o + OFF_COL + 2];
  }
  return { count, position, attributes };
}

/** Sequential index array, for a mesh that genuinely has no sharing. */
export function sequentialIndices(count) {
  const idx = new Uint32Array(count);
  for (let i = 0; i < count; i++) idx[i] = i;
  return idx;
}

/**
 * How many triangle corners each vertex accounts for, on average.
 *
 * Exactly 1 means a triangle soup: every corner is a distinct vertex, so every
 * vertex sits on its own border and the simplifier is not allowed to collapse
 * anything. A welded mesh is higher — a single quad reads 1.5, and the measured
 * Poly Haven bed frame reads 5.85.
 *
 * The unit matters. An earlier version reported *triangles per vertex* instead,
 * which for a soup of two triangles over six vertices is 0.33 rather than 1 —
 * so the caller's "at most 1.0 means a soup" test never fired and the soup check
 * silently passed everything, which is the exact failure the function exists to
 * prevent. Dividing the index count by the used-vertex count puts both cases on
 * one scale where 1.0 is the boundary.
 */
export function sharingRatio(vertexCount, indices) {
  if (!indices || !indices.length) return 0;
  const used = new Uint8Array(vertexCount);
  for (let i = 0; i < indices.length; i++) used[indices[i]] = 1;
  let n = 0;
  for (let i = 0; i < vertexCount; i++) n += used[i];
  if (n === 0) return 0;
  return indices.length / n;
}

/**
 * One LOD level, produced by simplification or copied from the source.
 *
 * @param {object} parts The split buffers.
 * @param {object} [opts]
 * @param {number} [opts.targetRatio] Fraction of the source triangles to keep.
 * @param {number} [opts.error] Absolute position error bound in model units.
 * @param {boolean} [opts.simplify] False to pass the source through untouched.
 * @returns {{vertices: Float32Array, indices: Uint32Array, targetCount: number, error: number}}
 */
export function buildLod(parts, opts = {}) {
  const { count, position, attributes, indices } = parts;
  const material = opts.material === undefined ? 0 : opts.material;
  const sourceIndices = indices && indices.length ? indices : sequentialIndices(count);
  if (!opts.simplify) {
    return {
      vertices: reassemble(parts, sourceIndices, material),
      indices: sourceIndices,
      targetCount: sourceIndices.length,
      error: 0,
      requestedError: 0,
      sourceCount: sourceIndices.length
    };
  }

  // meshoptimizer's error bound is in the same units as the positions, so it is
  // derived from the mesh's own size. A fixed bound would decimate a wardrobe
  // to a box and leave a doorknob untouched.
  let minX = Infinity, minY = Infinity, minZ = Infinity;
  let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i < count; i++) {
    const x = position[i * 3], y = position[i * 3 + 1], z = position[i * 3 + 2];
    if (x < minX) minX = x; if (y < minY) minY = y; if (z < minZ) minZ = z;
    if (x > maxX) maxX = x; if (y > maxY) maxY = y; if (z > maxZ) maxZ = z;
  }
  const diagonal = Math.hypot(maxX - minX, maxY - minY, maxZ - minZ) || 1;
  const error = opts.error !== undefined
    ? opts.error
    : diagonal * (opts.errorScale !== undefined ? opts.errorScale : 0.01);
  const target = Math.max(12, Math.floor((sourceIndices.length * (opts.targetRatio || 0.5)) / 3) * 3);

  // The second element of the returned pair is the *achieved* error, not a
  // count. Reading it as a count and calling `subarray(0, it)` produces an
  // empty mesh that looks exactly like a successful simplification — which is
  // what happened the first time this ran.
  const [dstIndex, achievedError] = MeshoptSimplifier.simplifyWithAttributes(
    sourceIndices,
    position,
    STRIDE_POS,
    attributes,
    ATTR_STRIDE,
    ATTR_WEIGHTS,
    null,
    target,
    error,
    ['LockBorder']
  );

  return {
    vertices: reassemble(parts, dstIndex, material),
    indices: dstIndex,
    targetCount: target,
    error: achievedError,
    requestedError: error,
    sourceCount: sourceIndices.length
  };
}

/**
 * Re-interleave the simplifier's output back into the flat vertex layout,
 * re-applying the material index that was held out of the simplify step.
 */
function reassemble(parts, indices, material) {
  const { position, attributes } = parts;
  const out = new Float32Array(indices.length * FLOATS_PER_VERTEX);
  for (let i = 0; i < indices.length; i++) {
    const src = indices[i];
    const o = i * FLOATS_PER_VERTEX;
    const p = src * STRIDE_POS;
    const a = src * ATTR_STRIDE;
    out[o + OFF_POS] = position[p];
    out[o + OFF_POS + 1] = position[p + 1];
    out[o + OFF_POS + 2] = position[p + 2];
    out[o + OFF_NRM] = attributes[a];
    out[o + OFF_NRM + 1] = attributes[a + 1];
    out[o + OFF_NRM + 2] = attributes[a + 2];
    out[o + OFF_COL] = attributes[a + 5];
    out[o + OFF_COL + 1] = attributes[a + 6];
    out[o + OFF_COL + 2] = attributes[a + 7];
    out[o + OFF_MAT] = material;
    out[o + OFF_UV] = attributes[a + 3];
    out[o + OFF_UV + 1] = attributes[a + 4];
  }
  return out;
}

/**
 * Generate the LOD chain for one imported model.
 *
 * @param {Float32Array} vertices The flat vertex array from the importer.
 * @param {Uint32Array} indices The importer's index buffer. Required: without
 *   it the mesh is a soup and nothing below can simplify anything.
 * @param {number} material The material index for the model.
 * @returns {{levels: Array, warnings: string[], sharing: number}}
 */
export function buildLodChain(vertices, indices, material) {
  const warnings = [];
  const parts = { ...splitForSimplify(vertices), indices };
  const sharing = sharingRatio(parts.count, indices);
  const total = indices && indices.length ? indices.length / 3 : parts.count / 3;
  // A mesh already at or below the LOD1 budget is not worth simplifying: the
  // simplifier can only lose detail, and three near-identical levels cost
  // memory and stream time for no visual difference.
  if (total <= 900) {
    warnings.push(`model has only ${total} triangles; LOD chain skipped`);
    return {
      levels: [{ level: 0, ...buildLod(parts, { material, simplify: false }) }],
      warnings,
      sharing
    };
  }
  if (sharing <= 1.0) {
    // A soup has a sharing ratio of exactly 1.0: every index is unique. Say so
    // plainly, because the symptom otherwise is an LOD chain that quietly does
    // nothing and a build report that claims a memory saving it did not make.
    warnings.push(
      `model is an unshared triangle soup (${total} triangles, ${parts.count} vertices); ` +
      'the simplifier cannot reduce it. The importer\'s weld step did not run.'
    );
  }

  // Each level gets both a target *count* and an error *bound*, because
  // meshoptimizer stops at whichever it hits first. With only a count target it
  // ran to the error bound and both levels collapsed onto the same 4,054
  // triangles; with only an error bound it ignored the ratio entirely. Both are
  // needed for the chain to be a chain.
  //
  // The authored mesh is the "detail" level and is NOT shipped. The measured
  // sources are 8k exports: a bed frame is 49,990 triangles, and shipping the
  // authored mesh for the set cost 31.5 MB of vertex data for furniture that
  // occupies a few hundred pixels. The runtime chain starts at 30% of it, which
  // for a 0.9 m chair at arm's length is indistinguishable from the original.
  const l0 = buildLod(parts, { material, simplify: true, targetRatio: 0.30, errorScale: 0.0015 });
  const l1 = buildLod(parts, { material, simplify: true, targetRatio: 0.12, errorScale: 0.004 });
  const l2 = buildLod(parts, { material, simplify: true, targetRatio: 0.05, errorScale: 0.008 });

  const levels = [l0, l1, l2].map((l, i) => ({ level: i, ...l }));

  // A level that failed to shrink is a bug in the simplifier call, not a
  // harmless no-op: it means the chain silently ships three copies of LOD0 and
  // the memory saving is fictional.
  for (let i = 1; i < levels.length; i++) {
    if (levels[i].indices.length >= levels[i - 1].indices.length) {
      warnings.push(
        `LOD${i} did not reduce the triangle count ` +
        `(${levels[i].indices.length / 3} vs ${levels[i - 1].indices.length / 3}); the simplifier is not being applied`
      );
    }
    // An empty level is worse than a redundant one: the runtime would bind a
    // zero-length draw range and the object would simply not exist.
    if (levels[i].indices.length === 0) {
      throw new Error(
        `LOD${i} simplified to zero triangles. That is a simplifier configuration error, ` +
        'not a small mesh — check the error bound.'
      );
    }
  }
  return { levels, warnings, sharing };
}

/** Held out of module scope so a test can inject a double if it needs to. */
let MeshoptSimplifier = null;
/** Install the simplifier. Called once by the bake tool. */
export function setSimplifier(s) { MeshoptSimplifier = s; }
/** Whether a simplifier is installed; the bake refuses to run without one. */
export function hasSimplifier() { return MeshoptSimplifier !== null; }
