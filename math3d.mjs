/**
 * EMERGENT math3d — column-major 4x4 matrix utilities for the WebGL2 renderer.
 *
 * Layout matches the renderer's uniform convention (WebGL/OpenGL column-major):
 *   m[c*4 + r] is row r of column c.
 * Functions are pure and allocation-explicit: each returns a fresh Float32Array
 * so callers can safely build view-procomposition chains per frame.
 */

/** Clamp v to the inclusive range [a, b]. */
export function clamp(v, a, b) {
  return Math.max(a, Math.min(b, v));
}

/**
 * Build a right-handed perspective projection matrix.
 *
 * @param {number} fovy   Vertical field of view in radians.
 * @param {number} aspect Width/height of the viewport (must be > 0).
 * @param {number} near   Near clip distance (> 0).
 * @param {number} far    Far clip distance (> near).
 * @returns {Float32Array} 16-element column-major matrix.
 */
export function matrixPerspective(fovy, aspect, near, far) {
  if (!(fovy > 0) || !(aspect > 0) || !(near > 0) || !(far > near)) {
    throw new RangeError(`invalid perspective parameters: fovy=${fovy} aspect=${aspect} near=${near} far=${far}`);
  }
  const f = 1 / Math.tan(fovy / 2);
  const nf = 1 / (near - far);
  return new Float32Array([
    f / aspect, 0, 0, 0,
    0, f, 0, 0,
    0, 0, (far + near) * nf, -1,
    0, 0, 2 * far * near * nf, 0
  ]);
}

/**
 * Build a right-handed view matrix looking from `eye` toward `target`.
 * Up is derived from a world up of (0,1,0) via the forward/right cross products,
 * matching the renderer's camera convention (yaw/pitch forward with no roll).
 *
 * @param {number[]} eye    [x, y, z] camera position.
 * @param {number[]} target [x, y, z] point to look at.
 * @returns {Float32Array} 16-element column-major view matrix.
 */
export function lookAt(eye, target) {
  let zx = eye[0] - target[0];
  let zy = eye[1] - target[1];
  let zz = eye[2] - target[2];
  let l = Math.hypot(zx, zy, zz) || 1;
  zx /= l; zy /= l; zz /= l;
  // Forward vector is nearly vertical (looking straight down/up): the
  // horizontal right vector degenerates, so normalize defensively and let the
  // cross-product basis below produce a well-defined (if arbitrary) roll.
  let xx = zz, xy = 0, xz = -zx;
  l = Math.hypot(xx, xz) || 1;
  xx /= l; xz /= l;
  const yx = zy * xz - zz * xy;
  const yy = zz * xx - zx * xz;
  const yz = zx * xy - zy * xx;
  return new Float32Array([
    xx, yx, zx, 0,
    xy, yy, zy, 0,
    xz, yz, zz, 0,
    -(xx * eye[0] + xy * eye[1] + xz * eye[2]),
    -(yx * eye[0] + yy * eye[1] + yz * eye[2]),
    -(zx * eye[0] + zy * eye[1] + zz * eye[2]),
    1
  ]);
}

/**
 * Multiply two column-major 4x4 matrices: returns a * b (apply b first).
 * The implementation is the unrolled standard 4x4 product; correctness is
 * pinned by unit tests (identity, associativity, projection composition).
 *
 * @param {Float32Array} a Left operand.
 * @param {Float32Array} b Right operand.
 * @returns {Float32Array} 16-element column-major product.
 */
export function mul(a, b) {
  const o = new Float32Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      o[c * 4 + r] =
        a[r] * b[c * 4] +
        a[4 + r] * b[c * 4 + 1] +
        a[8 + r] * b[c * 4 + 2] +
        a[12 + r] * b[c * 4 + 3];
    }
  }
  return o;
}

/**
 * Transform a world-space point by a 4x4 matrix, returning homogeneous
 * coordinates [x, y, z, w]. Used for projecting world positions (e.g. the
 * mission marker) into clip space.
 *
 * @param {Float32Array} m 16-element column-major matrix.
 * @param {number[]} p [x, y, z] world point.
 * @returns {number[]} [x, y, z, w] clip-space coordinates.
 */
export function transformPoint(m, p) {
  const x = p[0], y = p[1], z = p[2];
  return [
    m[0] * x + m[4] * y + m[8] * z + m[12],
    m[1] * x + m[5] * y + m[9] * z + m[13],
    m[2] * x + m[6] * y + m[10] * z + m[14],
    m[3] * x + m[7] * y + m[11] * z + m[15]
  ];
}
