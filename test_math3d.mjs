/**
 * Unit tests for math3d.mjs.
 *
 * These pin the projection/view math the WebGL2 renderer and the 2D mission
 * marker both depend on. The renderer previously carried this code inline,
 * which meant a sign error or a column-major slip could only be caught by
 * staring at a canvas; it is now checked as algebra.
 *
 * Run: `npm run test:math`
 */
import { clamp, matrixPerspective, lookAt, mul, transformPoint } from './math3d.mjs';

let checks = 0;
let failures = 0;
let currentTest = null;

function test(name, fn) {
  currentTest = { name, error: null };
  try {
    fn();
    process.stdout.write(`✓ ${name}\n`);
  } catch (err) {
    failures++;
    currentTest.error = err.message;
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

function assertMatrixClose(actual, expected, tol, msg) {
  checks++;
  if (actual.length !== expected.length) throw new Error(`${msg}: length ${actual.length}`);
  for (let i = 0; i < expected.length; i++) {
    if (!Number.isFinite(actual[i]) || Math.abs(actual[i] - expected[i]) > tol) {
      throw new Error(`${msg}: element ${i} expected ${expected[i]} +/- ${tol}, got ${actual[i]}`);
    }
  }
}

const IDENTITY = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];

/** Apply a column-major matrix to a point and divide through by w. */
function projectNDC(m, p) {
  const c = transformPoint(m, p);
  return [c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]];
}

test('clamp bounds a value to the inclusive range', () => {
  assert(clamp(5, 0, 1) === 1, 'above range clamps to b');
  assert(clamp(-5, 0, 1) === 0, 'below range clamps to a');
  assert(clamp(0.5, 0, 1) === 0.5, 'inside range is unchanged');
  assert(clamp(2, 2, 2) === 2, 'degenerate range is stable');
});

test('matrixPerspective rejects invalid parameters', () => {
  for (const args of [[0, 1, 0.1, 1000], [1, 0, 0.1, 1000], [1, 1, 0, 1000], [1, 1, 10, 1], [1, 1, 0.1, 0.1]]) {
    let threw = false;
    try { matrixPerspective(...args); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, `matrixPerspective(${args.join(', ')}) should throw RangeError`);
  }
});

test('matrixPerspective maps the near and far planes to the NDC depth range', () => {
  const near = 0.5, far = 900;
  const p = matrixPerspective(Math.PI / 3, 16 / 9, near, far);
  const atNear = projectNDC(p, [0, 0, -near]);
  const atFar = projectNDC(p, [0, 0, -far]);
  assertClose(atNear[2], -1, 1e-4, 'near plane maps to NDC z = -1');
  assertClose(atFar[2], 1, 1e-4, 'far plane maps to NDC z = +1');
  assertClose(atNear[3], near, 1e-4, 'clip w equals the view-space distance for RH depth');
  // Depth must be monotonically increasing in view-space distance.
  const mid = projectNDC(p, [0, 0, -(near + far) / 2]);
  assert(mid[2] > atNear[2] && mid[2] < atFar[2], 'midpoint depth is between the planes');
});

test('matrixPerspective produces a symmetric frustum', () => {
  const p = matrixPerspective(Math.PI / 3, 16 / 9, 0.5, 900);
  const left = projectNDC(p, [-1, 0, -10]);
  const right = projectNDC(p, [1, 0, -10]);
  assertClose(left[0], -right[0], 1e-5, 'frustum is left/right symmetric');
  const top = projectNDC(p, [0, 1, -10]);
  const bottom = projectNDC(p, [0, -1, -10]);
  assertClose(top[1], -bottom[1], 1e-5, 'frustum is top/bottom symmetric');
  assertClose(left[1], 0, 1e-6, 'a centre point projects to the vertical centre');
});

test('lookAt at the origin looking down -Z is the identity', () => {
  assertMatrixClose(lookAt([0, 0, 0], [0, 0, -1]), IDENTITY, 1e-6, 'canonical view matrix');
});

test('lookAt builds a right-handed basis pointing from eye to target', () => {
  const eye = [3, 4, 12];
  const target = [0, 1, 0];
  const v = lookAt(eye, target);
  // The camera origin must map to the view-space origin.
  const atEye = transformPoint(v, eye);
  assertClose(atEye[0], 0, 1e-5, 'eye maps to view x = 0');
  assertClose(atEye[1], 0, 1e-5, 'eye maps to view y = 0');
  assertClose(atEye[2], 0, 1e-5, 'eye maps to view z = 0');
  assertClose(atEye[3], 1, 1e-6, 'eye stays a point after the view transform');
  // The target sits on the -Z axis at its distance from the eye.
  const atTarget = transformPoint(v, target);
  const distance = Math.hypot(eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]);
  assertClose(atTarget[2], -distance, 1e-4, 'target lies at -distance on the view Z axis');
});

test('lookAt stays finite for a degenerate straight-down view', () => {
  const v = lookAt([0, 10, 0], [0, 0, 0]);
  for (let i = 0; i < 16; i++) {
    assert(Number.isFinite(v[i]), `element ${i} must be finite for a vertical view`);
  }
  const p = transformPoint(v, [0, 10, 0]);
  for (const c of p) assert(Number.isFinite(c), 'degenerate view still transforms points finitely');
});

test('mul is the identity when one operand is the identity', () => {
  const a = matrixPerspective(1.0, 1.7, 0.3, 500);
  const I = new Float32Array(IDENTITY);
  assertMatrixClose(mul(a, I), a, 1e-6, 'mul(a, I) === a');
  assertMatrixClose(mul(I, a), a, 1e-6, 'mul(I, a) === a');
});

test('mul is associative', () => {
  const a = matrixPerspective(1.1, 1.6, 0.4, 700);
  const b = lookAt([12, 30, -8], [0, 0, 0]);
  const c = lookAt([-5, 2, 40], [3, 1, 0]);
  const left = mul(mul(a, b), c);
  const right = mul(a, mul(b, c));
  assertMatrixClose(left, right, 1e-4, '(a*b)*c === a*(b*c)');
});

test('projection composed with a view transforms world points like a camera', () => {
  const near = 0.5, far = 900;
  const eye = [0, 1.7, 20];
  const target = [0, 1.7, 0];
  const pv = mul(matrixPerspective(Math.PI / 3, 16 / 9, near, far), lookAt(eye, target));

  // Straight ahead at 20m: centred horizontally, inside the frustum.
  const ahead = projectNDC(pv, [0, 1.7, 0]);
  assertClose(ahead[0], 0, 1e-5, 'a point straight ahead is horizontally centred');
  assertClose(ahead[1], 0, 1e-5, 'a point straight ahead is vertically centred');
  assert(ahead[2] > -1 && ahead[2] < 1, 'a point at 20m is inside the depth range');

  // Something behind the camera must produce w <= 0 so the mission-marker
  // projection test can reject it instead of drawing a mirrored marker.
  const behind = transformPoint(pv, [0, 1.7, 60]);
  assert(behind[3] < 0, 'a point behind the camera has negative clip w');

  // A point far to the right must land on the right of the screen.
  const right = projectNDC(pv, [8, 1.7, 0]);
  assert(right[0] > 0, 'a point to the world right projects to the screen right');
});

test('transformPoint preserves world coordinates under the identity', () => {
  const c = transformPoint(new Float32Array(IDENTITY), [3, -4, 5]);
  assert(c[0] === 3 && c[1] === -4 && c[2] === 5 && c[3] === 1, 'identity leaves points unchanged');
});

test('transformPoint matches a hand-computed translation', () => {
  const m = new Float32Array(IDENTITY);
  m[12] = 10; m[13] = -2; m[14] = 7;
  const c = transformPoint(m, [1, 1, 1]);
  assertClose(c[0], 11, 1e-6, 'translation applies to x');
  assertClose(c[1], -1, 1e-6, 'translation applies to y');
  assertClose(c[2], 8, 1e-6, 'translation applies to z');
  assertClose(c[3], 1, 1e-6, 'w is unchanged by translation');
});

process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
