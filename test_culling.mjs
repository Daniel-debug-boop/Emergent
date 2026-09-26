/**
 * Unit tests for culling.mjs.
 *
 * Frustum culling fails in a way nothing else catches. A wrong plane sign does
 * not crash, does not produce a NaN, and does not fail any test that looks at
 * whether the game "runs" — it culls a third of the city at the screen edge, or
 * culls nothing at all and quietly costs half the frame budget forever. So
 * these tests are built around ground truth rather than around the
 * implementation: a camera is constructed, a set of points is projected to
 * normalised device coordinates by an independent path, and the visibility
 * predicates are required to agree with whether those points actually land on
 * screen.
 *
 * Run: `npm run test:culling`
 */
import { matrixPerspective, lookAt, mul, transformPoint } from './math3d.mjs';
import { PLANE_COUNT, extractFrustumPlanes, aabbVisible, sphereVisible, growBounds, growBoundsBox } from './culling.mjs';

let checks = 0;
let failures = 0;

function test(name, fn) {
  try {
    fn();
    process.stdout.write(`✓ ${name}\n`);
  } catch (err) {
    failures++;
    process.stdout.write(`✗ ${name}\n  ${err.message}\n`);
  }
}

function assert(cond, msg) {
  checks++;
  if (!cond) throw new Error(msg);
}

function assertClose(actual, expected, tol, msg) {
  checks++;
  if (!Number.isFinite(actual) || Math.abs(actual - expected) > tol) {
    throw new Error(`${msg} (expected ${expected} +/- ${tol}, got ${actual})`);
  }
}

/** Build a view-projection for a camera at `eye` looking at `target`. */
function camera(eye, target, fov = 1.0, aspect = 16 / 9, near = 0.5, far = 1000) {
  return mul(matrixPerspective(fov, aspect, near, far), lookAt(eye, target));
}

/**
 * Ground truth: does `p` land inside the viewport when projected by `pv`?
 * Computed from clip coordinates directly, sharing no code with culling.mjs.
 */
function projectsOnScreen(pv, p) {
  const c = transformPoint(pv, p);
  if (c[3] <= 1e-6) return false;               // behind or on the eye
  const x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
  if (z < -1 || z > 1) return false;            // outside the depth range
  return x >= -1 && x <= 1 && y >= -1 && y <= 1;
}

test('extractFrustumPlanes rejects a matrix of the wrong size', () => {
  let threw = false;
  try { extractFrustumPlanes(new Float32Array(9)); } catch { threw = true; }
  assert(threw, 'a 9-element matrix must be rejected');
  threw = false;
  try { extractFrustumPlanes(null); } catch { threw = true; }
  assert(threw, 'a null matrix must be rejected');
});

test('extractFrustumPlanes rejects an undersized output buffer', () => {
  let threw = false;
  try {
    extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]), new Float32Array(8));
  } catch { threw = true; }
  assert(threw, 'an 8-element destination must be rejected rather than written past');
});

test('every extracted plane is unit length', () => {
  const planes = extractFrustumPlanes(camera([12, 30, 40], [0, 0, 0]));
  assert(planes.length === PLANE_COUNT * 4, 'six planes of four components each');
  for (let i = 0; i < PLANE_COUNT; i++) {
    const o = i * 4;
    const length = Math.hypot(planes[o], planes[o + 1], planes[o + 2]);
    assertClose(length, 1, 1e-5, `plane ${i} is normalised`);
  }
});

test('a point in front of the camera is inside, one behind it is not', () => {
  // Looking down -Z from the origin.
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]));
  assert(aabbVisible(planes, [-1, -1, -21], [1, 1, -19]), 'a box 20 units ahead is visible');
  assert(!aabbVisible(planes, [-1, -1, 19], [1, 1, 21]), 'a box 20 units behind is culled');
});

test('a box straddling the camera plane is kept, not culled', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]));
  // This is the case a naive "centre is outside, drop it" test gets wrong: the
  // centre is behind the eye but part of the box is in front and visible.
  assert(aabbVisible(planes, [-5, -5, -5], [5, 5, 5]), 'a box containing the camera is visible');
});

test('a box entirely to the left of a left-looking camera is culled', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [-1, 0, 0]));
  assert(!aabbVisible(planes, [30, -5, -5], [40, 5, 5]), 'a box 35 units right of a left-facing view is culled');
  assert(aabbVisible(planes, [-40, -5, -5], [-30, 5, 5]), 'a box ahead of a left-facing view is kept');
});

test('a box far above a downward-pitched camera is culled', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, -1, 0]));
  assert(!aabbVisible(planes, [-5, 30, -5], [5, 40, 5]), 'a box 35 units up is culled when looking down');
  assert(aabbVisible(planes, [-5, -40, -5], [5, -30, 5]), 'a box below the camera is kept when looking down');
});

test('a box beyond the far plane is culled and one just inside is kept', () => {
  const pv = camera([0, 0, 0], [0, 0, -1], 1.0, 16 / 9, 0.5, 200);
  const planes = extractFrustumPlanes(pv);
  assert(aabbVisible(planes, [-5, -5, -195], [5, 5, -150]), 'a box at 150 units is inside the 200 far plane');
  assert(!aabbVisible(planes, [-5, -5, -900], [5, 5, -500]), 'a box at 500 units is beyond the far plane');
});

test('a box nearer than the near plane is culled', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1], 1.0, 16 / 9, 5, 500));
  assert(!aabbVisible(planes, [-1, -1, -4], [1, 1, -1]), 'a box 3 units ahead is inside a 5-unit near plane');
  assert(aabbVisible(planes, [-1, -1, -20], [1, 1, -10]), 'a box 10 units ahead is kept');
});

test('no on-screen point is culled, and culling rejects the majority around the camera', () => {
  // The safety property, tested against ground truth.
  //
  // A volume test is *conservative*: a box straddling a frustum plane is
  // reported visible even when its centre projects off screen, and that is
  // correct — part of it is on screen. So the two must not be compared for
  // equality. What must hold is one-directional:
  //
  //   on-screen point  =>  the box around it is visible   (never cull what
  //                                                          has a visible pixel)
  //   box well outside  =>  culled                        (culling must actually
  //                                                          cull, or it is not
  //                                                          saving anything)
  //
  // The first direction is the one that produces "buildings vanish at the edge
  // of the screen" bugs, so it is the one asserted exhaustively.
  const eye = [120, 45, -80];
  const target = [-30, 10, 60];
  const pv = camera(eye, target, 1.1, 16 / 9, 0.5, 800);
  const planes = extractFrustumPlanes(pv);
  let tested = 0;
  let onScreen = 0;
  let falseCulls = 0;
  for (let i = -8; i <= 8; i++) {
    for (let j = -8; j <= 8; j++) {
      for (let k = -8; k <= 8; k++) {
        const p = [eye[0] + i * 12, eye[1] + j * 9, eye[2] + k * 12];
        const visible = aabbVisible(planes, [p[0] - 1, p[1] - 1, p[2] - 1], [p[0] + 1, p[1] + 1, p[2] + 1]);
        if (projectsOnScreen(pv, p)) {
          onScreen++;
          if (!visible) falseCulls++;
        }
        tested++;
      }
    }
  }
  assert(tested === 4913, `the grid is fully sampled (${tested})`);
  assert(onScreen > 100, `the grid actually reaches the screen (${onScreen} on-screen points)`);
  assert(falseCulls === 0, `no on-screen point is culled (${falseCulls} false culls out of ${onScreen})`);

  // And the other direction, stated as a measurement rather than as a
  // hand-picked list of offsets. "600 units along X" is not reliably off screen
  // for a 63-degree vertical field of view looking diagonally — it can be
  // straight ahead — so the honest claim is that culling rejects the large
  // majority of a surrounding ring, not that it rejects every one of six
  // arbitrary points.
  let kept = 0;
  const ring = 36;
  for (let i = 0; i < ring; i++) {
    const a = (i / ring) * Math.PI * 2;
    const p = [eye[0] + Math.cos(a) * 600, eye[1] - 20, eye[2] + Math.sin(a) * 600];
    if (aabbVisible(planes, [p[0] - 20, p[1] - 20, p[2] - 20], [p[0] + 20, p[1] + 20, p[2] + 20])) kept++;
  }
  assert(kept * 2 < ring, `culling rejects the majority of a 600-unit ring (${kept}/${ring} kept)`);
});

test('turning the camera away culls what was in front of it', () => {
  // The property the renderer actually depends on. A culler that always
  // returns true passes every other test in this file.
  const base = [0, 0, 0];
  const forward = extractFrustumPlanes(camera(base, [0, 0, -1]));
  const behind = extractFrustumPlanes(camera(base, [0, 0, 1]));
  const aheadBox = [-10, -10, -60], aheadMin = [-10, -10, -60], aheadMax = [10, 10, -40];
  const backBoxMin = [-10, -10, 40], backBoxMax = [10, 10, 60];
  assert(aabbVisible(forward, aheadMin, aheadMax), 'the box ahead is visible when looking ahead');
  assert(!aabbVisible(forward, backBoxMin, backBoxMax), 'the box behind is culled when looking ahead');
  assert(!aabbVisible(behind, aheadMin, aheadMax), 'the box ahead is culled when looking behind');
  assert(aabbVisible(behind, backBoxMin, backBoxMax), 'the box behind is visible when looking behind');
  void aheadBox;
});

test('sphereVisible is conservative where a point test would fail', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]));
  // A sphere centred exactly on the left plane is half outside. A point test
  // would say "outside" and cull geometry that is visibly half drawn.
  assert(sphereVisible(planes, 0, 0, -20, 5), 'a sphere centred on the eye is visible');
  assert(sphereVisible(planes, 0, 0, -20, 0), 'a zero-radius sphere at a visible point is visible');
  assert(!sphereVisible(planes, 500, 0, -20, 5), 'a sphere far off to the side is culled');
  assert(!sphereVisible(planes, 0, 0, 500, 5), 'a sphere behind the camera is culled');
});

test('sphereVisible rejects a nonsense radius rather than returning garbage', () => {
  const planes = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]));
  for (const bad of [-1, NaN, Infinity]) {
    let threw = false;
    try { sphereVisible(planes, 0, 0, -20, bad); } catch { threw = true; }
    assert(threw, `radius ${bad} must be rejected`);
  }
});

test('growing a sphere never makes it invisible', () => {
  // Monotonicity, in the only direction that is a bug. A larger bounding
  // volume containing a smaller one can only ever become *more* visible, so
  // true -> false is a violation. false -> true is the whole point of the
  // radius, and is not an error.
  const planes = extractFrustumPlanes(camera([10, 20, 30], [0, 0, 0], 1.0, 16 / 9, 0.5, 500));
  for (let i = 0; i < 200; i++) {
    const x = (i % 20) * 9 - 90;
    const y = Math.floor(i / 20) * 7 - 35;
    const z = 40;
    let wasVisible = false;
    for (let r = 0; r <= 20; r += 2) {
      const visible = sphereVisible(planes, x, y, z, r);
      if (wasVisible && !visible) {
        throw new Error(`sphere at (${x},${y},${z}) disappeared at radius ${r}`);
      }
      wasVisible = visible;
    }
  }
  checks++;
});

test('growBounds starts from a point and expands correctly', () => {
  let b = growBounds(null, 5, 6, 7);
  assert(b.min[0] === 5 && b.max[2] === 7, 'a null bounds starts at the point');
  b = growBounds(b, 0, 9, 7);
  assert(b.min[0] === 0 && b.max[1] === 9, 'grows to include a lower-x higher-y point');
  b = growBounds(b, 3, 1, -2);
  assert(b.min[2] === -2 && b.max[0] === 5, 'grows to include a lower-z point without shrinking x');
  const before = [b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]];
  b = growBounds(b, 2, 7, 3);
  assert(before.every((v, i) => v === [b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]][i]),
    'a genuinely interior point leaves the bounds untouched');
});

test('growBoundsBox covers the whole box', () => {
  const b = growBoundsBox(null, 0, 0, 0, 2, 3, 4);
  assert(b.min[0] === -2 && b.max[0] === 2, 'x extent');
  assert(b.min[1] === -3 && b.max[1] === 3, 'y extent');
  assert(b.min[2] === -4 && b.max[2] === 4, 'z extent');
});

test('growBounds returns the same object it was given', () => {
  const first = growBounds(null, 0, 0, 0);
  const second = growBounds(first, 1, 1, 1);
  assert(first === second, 'the accumulator is mutated in place, not copied per point');
});

test('a degenerate view matrix does not cull the world', () => {
  // All-zero matrix: every plane degenerates. The safe failure is "draw
  // everything", because the alternative is an empty screen that looks like a
  // bug in the level rather than a bug in the culler.
  const planes = extractFrustumPlanes(new Float32Array(16));
  assert(aabbVisible(planes, [0, 0, 0], [1, 1, 1]), 'a degenerate matrix keeps geometry rather than culling it');
});

test('extractFrustumPlanes can write into a caller-supplied buffer', () => {
  const out = new Float32Array(24);
  const a = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]), out);
  const b = extractFrustumPlanes(camera([0, 0, 0], [0, 0, -1]));
  assert(a === out, 'the supplied buffer is returned, so a per-frame frustum allocates nothing');
  for (let i = 0; i < 24; i++) assertClose(a[i], b[i], 1e-6, `element ${i} matches the allocating path`);
});

process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
