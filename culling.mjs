/**
 * EMERGENT culling — frustum extraction and visibility tests.
 *
 * Pure functions, no GL, no DOM, no world state. That is the whole point: the
 * arithmetic that decides whether geometry is drawn is the arithmetic most
 * likely to be subtly wrong, and a bug here is invisible in a screenshot. It
 * shows up as a building that pops out of existence at the edge of the screen,
 * or as a shadow that follows the camera. Keeping it pure means it is unit
 * tested directly rather than inferred from a rendered frame.
 *
 * A frustum is six planes. A point is inside when it is on the positive side
 * of all six. A volume is visible when *any part* of it is inside all six,
 * which for a box means testing only its far corner on each plane — the
 * standard "positive vertex" trick, and the reason this is cheap enough to run
 * per chunk per frame.
 *
 * Plane convention, matching the renderer's OpenGL-style clip space where
 * -w <= x,y,z <= w:
 *   left   =  row3 + row0
 *   right  =  row3 - row0
 *   bottom =  row3 + row1
 *   top    =  row3 - row1
 *   near   =  row3 + row2
 *   far    =  row3 - row2
 * where rowN is row N of the matrix. Each plane is stored normalised, with the
 * inside half-space on the positive side, and as [nx, ny, nz, d] so the test
 * is a single dot product plus an add.
 */

/** Number of planes in a frustum: left, right, bottom, top, near, far. */
export const PLANE_COUNT = 6;

/**
 * Extract the six frustum planes from a view-projection matrix.
 *
 * The matrix is column-major, matching math3d.mjs: `m[c * 4 + r]` is row r of
 * column c, which is the layout `mul` produces and `matrixPerspective` and
 * `lookAt` return.
 *
 * Planes come back normalised. Normalising matters for more than tidiness: the
 * near and far planes carry a scale proportional to the projection's focal
 * length, and an epsilon tuned for one plane would then be wrong for the
 * others. With unit normals a single world-space epsilon is meaningful on
 * every plane.
 *
 * @param {Float32Array} m 16-element column-major view-projection matrix.
 * @param {Float32Array} [out] Optional 24-element destination, written in
 *        place. Supplying one keeps a per-frame frustum allocation-free.
 * @returns {Float32Array} 24-element array of six [nx, ny, nz, d] planes.
 * @throws {RangeError} if the matrix is not 16 elements.
 */
export function extractFrustumPlanes(m, out) {
  if (!m || m.length !== 16) {
    throw new RangeError(`extractFrustumPlanes needs a 16-element matrix, got ${m ? m.length : 'nothing'}`);
  }
  const planes = out || new Float32Array(PLANE_COUNT * 4);
  if (planes.length < PLANE_COUNT * 4) {
    throw new RangeError(`extractFrustumPlanes output needs ${PLANE_COUNT * 4} elements, got ${planes.length}`);
  }

  // Rows of the matrix, pulled out of the column-major storage once so the six
  // plane combinations below read like the algebra they are.
  const r0 = m[0], r0y = m[4], r0z = m[8], r0w = m[12];
  const r1 = m[1], r1y = m[5], r1z = m[9], r1w = m[13];
  const r2 = m[2], r2y = m[6], r2z = m[10], r2w = m[14];
  const r3 = m[3], r3y = m[7], r3z = m[11], r3w = m[15];

  writePlane(planes, 0, r3 + r0, r3y + r0y, r3z + r0z, r3w + r0w);   // left
  writePlane(planes, 1, r3 - r0, r3y - r0y, r3z - r0z, r3w - r0w);   // right
  writePlane(planes, 2, r3 + r1, r3y + r1y, r3z + r1z, r3w + r1w);   // bottom
  writePlane(planes, 3, r3 - r1, r3y - r1y, r3z - r1z, r3w - r1w);   // top
  writePlane(planes, 4, r3 + r2, r3y + r2y, r3z + r2z, r3w + r2w);   // near
  writePlane(planes, 5, r3 - r2, r3y - r2y, r3z - r2z, r3w - r2w);   // far
  return planes;
}

function writePlane(planes, index, nx, ny, nz, d) {
  const length = Math.hypot(nx, ny, nz);
  const o = index * 4;
  if (length < 1e-20) {
    // A degenerate plane carries no information. Storing it as "everything is
    // inside" (0,0,0,1 gives a test of 1 >= 0, always true) is the safe
    // direction: a zero normal would make the dot product 0 and let geometry
    // through regardless of position, but a zero normal from a degenerate
    // matrix is a bug we want visible, not one that silently culls the world.
    planes[o] = 0; planes[o + 1] = 0; planes[o + 2] = 0; planes[o + 3] = 1;
    return;
  }
  planes[o] = nx / length;
  planes[o + 1] = ny / length;
  planes[o + 2] = nz / length;
  planes[o + 3] = d / length;
}

/**
 * True when the axis-aligned box is at least partly inside the frustum.
 *
 * Tests only the box corner furthest along each plane's normal. If even that
 * corner is outside, the whole box is; if it is inside, some of the box is.
 * This is conservative in the safe direction — it never culls something that
 * has a visible pixel.
 *
 * @param {Float32Array} planes 24-element plane array.
 * @param {number[]} min [x, y, z] minimum corner, world space.
 * @param {number[]} max [x, y, z] maximum corner, world space.
 * @returns {boolean}
 */
export function aabbVisible(planes, min, max) {
  for (let i = 0; i < PLANE_COUNT; i++) {
    const o = i * 4;
    const nx = planes[o], ny = planes[o + 1], nz = planes[o + 2];
    // The "positive vertex": pick max on a positive normal axis, min on a
    // negative one. That corner is the one most likely to be inside.
    const px = nx >= 0 ? max[0] : min[0];
    const py = ny >= 0 ? max[1] : min[1];
    const pz = nz >= 0 ? max[2] : min[2];
    if (nx * px + ny * py + nz * pz + planes[o + 3] < 0) return false;
  }
  return true;
}

/**
 * True when the sphere is at least partly inside the frustum.
 *
 * Cheaper than the box test and the right test for anything that is roughly
 * round: a character, a car, a tree canopy. Being conservative for a sphere
 * means using the *nearest* point on the sphere to the plane, hence `>= -r`
 * rather than `>= 0`.
 *
 * @param {Float32Array} planes 24-element plane array.
 * @param {number} x Sphere centre x.
 * @param {number} y Sphere centre y.
 * @param {number} z Sphere centre z.
 * @param {number} radius Sphere radius; must be finite and >= 0.
 * @returns {boolean}
 */
export function sphereVisible(planes, x, y, z, radius) {
  if (!(radius >= 0) || !Number.isFinite(radius)) {
    throw new RangeError(`sphereVisible needs a finite non-negative radius, got ${radius}`);
  }
  for (let i = 0; i < PLANE_COUNT; i++) {
    const o = i * 4;
    if (planes[o] * x + planes[o + 1] * y + planes[o + 2] * z + planes[o + 3] < -radius) return false;
  }
  return true;
}

/**
 * Grow `bounds` to include a point. Mutates in place so a builder can accumulate
 * a chunk's bounds without allocating per point.
 *
 * @param {{min:number[],max:number[]}|null} bounds Existing bounds, or null to
 *        start one at the point.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {{min:number[],max:number[]}} `bounds` when supplied, else a new one.
 */
export function growBounds(bounds, x, y, z) {
  if (!bounds) return { min: [x, y, z], max: [x, y, z] };
  if (x < bounds.min[0]) bounds.min[0] = x;
  if (y < bounds.min[1]) bounds.min[1] = y;
  if (z < bounds.min[2]) bounds.min[2] = z;
  if (x > bounds.max[0]) bounds.max[0] = x;
  if (y > bounds.max[1]) bounds.max[1] = y;
  if (z > bounds.max[2]) bounds.max[2] = z;
  return bounds;
}

/**
 * Grow `bounds` to include an axis-aligned box.
 *
 * @param {{min:number[],max:number[]}|null} bounds
 * @param {number} cx Box centre x.
 * @param {number} cy Box centre y.
 * @param {number} cz Box centre z.
 * @param {number} hx Half-size x.
 * @param {number} hy Half-size y.
 * @param {number} hz Half-size z.
 * @returns {{min:number[],max:number[]}}
 */
export function growBoundsBox(bounds, cx, cy, cz, hx, hy, hz) {
  // The return value of the first call has to be threaded into the second. A
  // null `bounds` makes growBounds allocate, and dropping that result leaves
  // the second call starting a *different* accumulator from the same point —
  // which silently yields a box containing only its maximum corner.
  return growBounds(growBounds(bounds, cx - hx, cy - hy, cz - hz), cx + hx, cy + hy, cz + hz);
}
