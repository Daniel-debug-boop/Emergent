/**
 * EMERGENT budget tests — the vertex budget controller.
 *
 * The controller's job is to hold a target vertex count as the world's density
 * and the camera's position change. That is a claim about *behaviour over
 * time*, so these tests assert convergence, clamping and hysteresis rather than
 * the value of the gain constant. A test that asserted the exact scale after
 * three builds would be a test of a number, and would break the moment the
 * constant was retuned without telling anyone whether the behaviour was still
 * correct.
 *
 * Run: `npm run test:budget`
 */
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
  createBudget, observe, retarget, baseDetailRadius, staticDetailFor,
  describe, VERTEX_TARGETS
} from './budget.mjs';

const root = dirname(fileURLToPath(import.meta.url));

/** The radius a fresh budget starts at, for calibrating the cost model. */
function budgetBaseRadius() {
  return baseDetailRadius(2, false);
}

// ---------------------------------------------------------------------------
// Minimal test runner
// ---------------------------------------------------------------------------

const results = [];
let currentTest = null;

async function test(name, fn) {
  currentTest = { name, checks: 0 };
  results.push(currentTest);
  try {
    await fn();
  } catch (err) {
    currentTest.error = err.stack || String(err);
  }
}

function assert(condition, message) {
  currentTest.checks++;
  if (!condition) throw new Error(message);
}

function assertClose(actual, expected, tolerance, message) {
  assert(
    Number.isFinite(actual) && Math.abs(actual - expected) <= tolerance,
    `${message} (expected ${expected} +/- ${tolerance}, got ${actual})`
  );
}

/**
 * A stand-in for the real relationship between detail radius and vertex count.
 *
 * Superlinear, because a bigger radius sees more buildings and buildings are
 * denser near the centre of a city. Deliberately has *no constant term*: a cost
 * that does not fall to zero as the radius shrinks models a world with a fixed
 * overhead the controller cannot spend its way out of, which is a different
 * problem — an unreachable target — masquerading as a control failure. An
 * earlier version of this file had `40000 + r^2 * 0.9` and reported the
 * controller as broken at high densities when the arithmetic showed the minimum
 * achievable cost was already 133% of target. No controller can hit that.
 */
function costFor(radius) {
  return Math.max(400, Math.round(radius * radius * 0.62));
}

/**
 * A cost model calibrated so that the *starting* radius lands on the budget's
 * own target, for whatever that target currently is.
 *
 * The three density tests below assert control behaviour, not absolute vertex
 * counts, and the targets changed once already — from 240k, when they were
 * written against a scene whose terrain floor had not yet been measured, to the
 * current values. Hard-coding 240,000 into the model meant the model produced a
 * scene already *under* budget, so the controller correctly did nothing and the
 * tests failed for a reason that had nothing to do with control.
 */
function costForTarget(radius, target) {
  // `budgetBaseRadius()` — called. Referenced bare it is a function object, and
  // dividing by one silently yields NaN, which `observe` then rejects as an
  // unusable measurement. That is the module working correctly: it refused a
  // nonsense number instead of folding it into the control loop.
  return Math.max(400, Math.round((radius * radius * 0.62 * target) / costFor(budgetBaseRadius())));
}

/** Drive the controller to steady state against a fixed scene density. */
function settle(density = 1, steps = 24) {
  const budget = createBudget(2, false);
  const trace = [];
  for (let i = 0; i < steps; i++) {
    const actual = Math.round(costForTarget(budget.detailRadius, budget.target) * density);
    const r = observe(budget, actual);
    trace.push({ actual, ...r });
  }
  return { budget, trace };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

await test('a fresh budget starts at the old heuristic and takes no correction yet', () => {
  const b = createBudget(2, false);
  assertEqual(b.scale, 1, 'the scale starts neutral');
  assertEqual(b.corrections, 0, 'nothing has been measured');
  assertEqual(b.target, VERTEX_TARGETS[2], 'the target follows the quality level');
  assertClose(b.detailRadius, baseDetailRadius(2, false), 1e-9, 'and so does the starting radius');
  assertEqual(staticDetailFor(2, false), 0.94, 'the per-object detail multiplier is unchanged');
});

await test('overspending shrinks the detail radius and underspending grows it', () => {
  const b = createBudget(2, false);
  const before = b.detailRadius;
  const over = observe(b, b.target * 2);
  assert(over.overshoot > 1, 'a 2x build is an overshoot');
  assert(over.scale < 1, `overspending must shrink the radius, got x${over.scale}`);
  assert(b.detailRadius < before, 'and the radius it controls must actually fall');

  const b2 = createBudget(2, false);
  const before2 = b2.detailRadius;
  const under = observe(b2, b2.target * 0.5);
  assert(under.scale > 1, `underspending must grow the radius, got x${under.scale}`);
  assert(b2.detailRadius > before2, 'and the radius it controls must actually rise');
});

await test('the controller converges on its target rather than oscillating', () => {
  const { budget, trace } = settle(1.0, 24);
  const last = trace[trace.length - 1];
  const ratio = last.actual / budget.target;
  assert(
    ratio > 0.85 && ratio < 1.18,
    `should settle near the target, got ${(ratio * 100).toFixed(0)}% (${last.actual} vs ${budget.target})`
  );
  // Convergence means the *changes* stop, not just that the number is close.
  const lateChanges = trace.slice(-6).filter((t) => t.changed).length;
  assert(lateChanges <= 1, `should stop correcting once settled, still changed on ${lateChanges} of the last 6 builds`);
});

await test('it converges on a world three times denser than the one it was tuned on', () => {
  // This is the case the old fixed radius could not handle at all: the radius
  // was a guess about cost, and here the cost is 3x the guess.
  const { budget } = settle(3.0, 30);
  const ratio = budget.lastActual / budget.target;
  assert(
    ratio > 0.85 && ratio < 1.18,
    `a 3x denser world should still settle near target, got ${(ratio * 100).toFixed(0)}% (${budget.lastActual} vs ${budget.target})`
  );
  assert(budget.scale < 0.75, `it should have pulled the radius well in, got x${budget.scale.toFixed(2)}`);
});

await test('it converges on a sparse world too, in the other direction', () => {
  const { budget } = settle(0.35, 30);
  const ratio = budget.lastActual / budget.target;
  assert(
    ratio > 0.85 && ratio < 1.18,
    `a sparse world should still settle near target, got ${(ratio * 100).toFixed(0)}%`
  );
  assert(budget.scale > 1.2, `it should have pushed the radius out, got x${budget.scale.toFixed(2)}`);
});

await test('the targets are reachable, so the controller is never fighting a floor', () => {
  // The failure this prevents is specific and was observed: the terrain chunk is
  // built at the *stream* radius and accounts for ~500k vertices that the detail
  // radius cannot touch, so a target below that floor is unreachable. The
  // controller then drives the radius to its minimum and sits there reporting
  // 216% of target, rebuilding the scene every frame, looking stable.
  //
  // The floor itself is a property of the game, not of this module, so the
  // runtime suite measures it there. What is asserted here is that the targets
  // are ordered and plausible, so a future edit has to be deliberate.
  const base = baseDetailRadius(2, false);
  const costAtBase = costFor(base);
  assert(VERTEX_TARGETS[2] > costAtBase * 0.5,
    `the mid target (${VERTEX_TARGETS[2]}) must leave room above a scene built at the base radius`);
  for (const t of VERTEX_TARGETS) assert(t > 0, 'every target must be positive');
});

await test('one pathological build cannot collapse or explode the scene', () => {
  const b = createBudget(2, false);
  const huge = observe(b, b.target * 500);
  assert(huge.scale >= 0.01, `a 500x overshoot must be clamped, got x${huge.scale}`);
  const tiny = observe(createBudget(2, false), 1);
  assert(tiny.scale <= 8, `a near-empty build must be clamped, got x${tiny.scale}`);

  // And a clamped scale is still a *usable* radius, not a degenerate one.
  const b2 = createBudget(2, false);
  observe(b2, b2.target * 10_000);
  assert(b2.detailRadius > 0, `radius must stay positive, got ${b2.detailRadius.toFixed(0)}m`);
  assert(b2.detailRadius < 100_000, `radius must stay sane, got ${b2.detailRadius.toFixed(0)}m`);
});

await test('the scale bounds do not bind at any density a real world produces', () => {
  // A clamp that binds in normal operation is a hard ceiling on quality that
  // reports itself as a success. The earlier bounds of [0.45, 2.2] bound at 3x
  // and 0.1x density — both entirely plausible — and the controller sat pinned
  // and confidently wrong. This asserts the bounds are *reachable only at
  // extremes*, by checking convergence across the plausible range first and
  // then that the extremes still produce a bounded, sane radius.
  for (const density of [0.25, 0.5, 1, 2, 4, 6]) {
    const b = createBudget(2, false);
    for (let i = 0; i < 16; i++) observe(b, Math.round(costForTarget(b.detailRadius, b.target) * density));
    const ratio = b.lastActual / b.target;
    assert(
      ratio > 0.9 && ratio < 1.1,
      `density ${density}x should converge, got ${(ratio * 100).toFixed(0)}% — the clamp is binding`
    );
    assert(b.scale > 0.01, `density ${density}x must not sit on the floor, got x${b.scale.toFixed(3)}`);
    assert(b.scale < 8, `density ${density}x must not sit on the ceiling, got x${b.scale.toFixed(3)}`);
  }
});

await test('a build already on target does not trigger a rebuild', () => {
  // The worst outcome for a feedback controller is churning: a full scene
  // rebuild every frame for no visible change. Hysteresis is what prevents it.
  const b = createBudget(2, false);
  observe(b, b.target);
  let changes = 0;
  for (let i = 0; i < 12; i++) {
    // A scene that measures within a few percent of target, as real ones do.
    const actual = Math.round(b.target * (1 + (i % 2 ? 0.03 : -0.03)));
    if (observe(b, actual).changed) changes++;
  }
  assertEqual(changes, 0, 'a scene already on target must not be rebuilt');
});

await test('changing quality retargets and resets the fitted scale', () => {
  const b = createBudget(3, false);
  observe(b, b.target * 4);
  assert(b.scale < 0.6, 'precondition: the scale has been pulled in');
  retarget(b, 0, false);
  assertEqual(b.scale, 1, 'the scale is reset, not carried over');
  assertEqual(b.target, VERTEX_TARGETS[0], 'and the target follows the new level');
  assertClose(b.detailRadius, baseDetailRadius(0, false), 1e-9, 'as does the radius');
  // The low level really is a lower target, so carrying the old scale over
  // would be asking for a tenth of the geometry in a world that needs the same.
  assert(VERTEX_TARGETS[0] < VERTEX_TARGETS[3], 'quality levels must actually differ in target');
});

await test('the adaptive renderer mode is retargeted, not just the quality level', () => {
  const b = createBudget(2, false);
  const before = b.detailRadius;
  retarget(b, 2, true);
  assert(b.detailRadius < before, 'adaptive mode starts with a smaller radius');
  assertClose(b.detailRadius, baseDetailRadius(2, true), 1e-9, 'matching its own base');
});

await test('a nonsense measurement is an error, not a silent reset', () => {
  const b = createBudget(2, false);
  let threw = false;
  try { observe(b, 0); } catch { threw = true; }
  assert(threw, 'a zero-vertex build must be rejected');
  threw = false;
  try { observe(createBudget(2, false), Number.NaN); } catch { threw = true; }
  assert(threw, 'a NaN measurement must be rejected');
  threw = false;
  try { createBudget(0, false).target = 0; observe(createBudget(0, false), 100); } catch { threw = true; }
  assert(!threw, 'a valid measurement against a valid target is fine');
});

await test('the controller survives a teleport that changes the scene by 10x', () => {
  // The realistic worst case: the player crosses a boundary between a dense
  // downtown and open country in one step.
  //
  // The assertion is that the *radius* moves in the right direction and that
  // the budget is re-converged quickly. It is deliberately not "spends less" —
  // a controller that has converged spends exactly the target in both worlds,
  // so comparing absolute spend across the teleport measures the target, not
  // the recovery, and would pass or fail for reasons unrelated to the control.
  const b = createBudget(2, false);
  for (let i = 0; i < 20; i++) observe(b, Math.round(costForTarget(b.detailRadius, b.target) * 4));
  const radiusInDense = b.detailRadius;
  const scaleInDense = b.scale;

  let steps = 0;
  for (let i = 0; i < 40; i++) {
    observe(b, Math.round(costForTarget(b.detailRadius, b.target) * 0.4));
    steps++;
    if (Math.abs(b.lastActual / b.target - 1) < 0.12) break;
  }
  assert(steps < 25, `should recover from a teleport in a few builds, took ${steps}`);
  assert(b.scale > scaleInDense, 'a sparser world must push the radius out');
  assert(b.detailRadius > radiusInDense, 'and the radius it controls must actually grow');
  const ratio = b.lastActual / b.target;
  assert(ratio > 0.88 && ratio < 1.12, `and re-converge on the target, got ${(ratio * 100).toFixed(0)}%`);
});

await test('the exponent is measured from the scene, not assumed', () => {
  // The whole design rests on `k` being inferred. A fixed gain is stable only
  // while gain * k <= 1, so a controller tuned for a linear world limit-cycles
  // on a quadratic one — which is exactly what an earlier version did, at
  // alternating 73% and 171% of target, forever.
  //
  // The exponent is read *during* the transient, from a pair of builds whose
  // radii actually differed. A world that happens to start on target produces
  // no transient, no pair, and therefore no estimate — the default is held and
  // that is correct, because there is no evidence to override it with. Both
  // probes below are therefore deliberately started well off target.
  const linear = createBudget(2, false);
  for (let i = 0; i < 8; i++) observe(linear, Math.max(400, Math.round(linear.detailRadius * 60)));
  assert(linear.exponent < 1.4, `a linear cost curve should read near k=1, got ${linear.exponent.toFixed(2)}`);

  const quadratic = createBudget(2, false);
  for (let i = 0; i < 8; i++) {
    observe(quadratic, Math.max(400, Math.round(quadratic.detailRadius * quadratic.detailRadius * 0.6)));
  }
  assert(quadratic.exponent > 1.6, `a quadratic cost curve should read near k=2, got ${quadratic.exponent.toFixed(2)}`);
  assert(quadratic.exponent > linear.exponent + 0.4, 'the two worlds must not read the same exponent');
});

await test('a world that starts on target keeps the default exponent, having no evidence otherwise', () => {
  // The counterpart to the test above, and the reason it is worth stating: with
  // no radius movement there is no information about the cost curve, so the
  // controller must not invent one. An earlier version of the linear probe used
  // a cost that happened to land within the dead band immediately, never moved
  // the radius, and reported k=2.00 as if the controller had failed to learn.
  const b = createBudget(2, false);
  const before = b.detailRadius;
  // A cost that is exactly the target at the starting radius: a 3% wobble each
  // way stays inside the dead band, which is the situation the band exists for.
  for (let i = 0; i < 6; i++) observe(b, Math.round(b.target * (1 + (i % 2 ? 0.03 : -0.03))));
  assertEqual(b.detailRadius, before, 'an on-target world must not rebuild at all');
  assertEqual(b.corrections, 0, 'and must not have corrected');
  assertEqual(b.exponent, 2, 'so the default exponent stands, uncontradicted');
});

await test('the correction accumulates rather than resetting toward the base', () => {
  // `scale` is a position, not a step. Assigning the correction instead of
  // multiplying into it drags the radius back toward `base` on every build, and
  // the controller orbits the wrong point indefinitely — which is how an
  // earlier version sat at 106%, 138% and 214% while reporting success.
  //
  // Measured as a *product*: the converged scale must equal the product of every
  // correction applied, not the last one. That is the property, and it is
  // checkable without depending on how many builds it took to get there.
  const b = createBudget(2, false);
  for (let i = 0; i < 12; i++) observe(b, Math.round(costForTarget(b.detailRadius, b.target) * 3));
  const corrections = b.history.filter((h) => h.overshoot > 1.12).map((h) => Math.pow(h.overshoot, -1 / h.exponent));
  const product = corrections.reduce((a, c) => a * c, 1);
  assertClose(b.scale, product, Math.max(0.02, product * 0.15),
    'the scale must be the accumulated product of the corrections, not the last one');
  assert(b.scale < 0.75, `a 3x denser world must pull the radius in, got x${b.scale.toFixed(3)}`);
});

await test('every quality level has a target and the ordering is sensible', () => {
  assert(VERTEX_TARGETS.length === 4, 'four quality levels');
  for (let i = 1; i < VERTEX_TARGETS.length; i++) {
    assert(VERTEX_TARGETS[i] > VERTEX_TARGETS[i - 1], `level ${i} must allow more geometry than level ${i - 1}`);
  }
  for (let q = 0; q < 4; q++) {
    const b = createBudget(q, false);
    assertEqual(b.target, VERTEX_TARGETS[q], `level ${q} targets its own budget`);
  }
  // Out-of-range levels clamp rather than reading undefined, which would make
  // the target NaN and silently disable the controller.
  assertEqual(createBudget(99, false).target, VERTEX_TARGETS[3], 'an absurd level clamps to the highest target');
});

await test('the history records every measurement, and describes() reports it', () => {
  const b = createBudget(2, false);
  for (let i = 0; i < 5; i++) observe(b, b.target + i * 1000);
  assertEqual(b.history.length, 5, 'every measurement is kept');
  assert(b.history.every((h) => Number.isFinite(h.overshoot) && h.overshoot > 0), 'each has a usable ratio');
  const text = describe(b);
  assert(text.includes('static'), `the summary should be readable, got "${text}"`);
  assert(/\d+%/.test(text), `and should state the percentage, got "${text}"`);
});

function assertEqual(actual, expected, message) {
  assert(actual === expected, `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

let failures = 0;
let totalChecks = 0;
for (const t of results) {
  totalChecks += t.checks;
  if (t.error) {
    failures++;
    console.log(`✗ ${t.name}`);
    console.log(`    ${t.error.split('\n').slice(0, 4).join('\n    ')}`);
  } else {
    console.log(`✓ ${t.name} — ok (${t.checks} checks)`);
  }
}
console.log(`${results.length - failures}/${results.length} budget tests passed, ${totalChecks} assertions`);
process.exit(failures ? 1 : 0);
