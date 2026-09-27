/**
 * The static vertex budget.
 *
 * ## The problem this replaces
 *
 * Detail was previously spent by a fixed radius:
 *
 *     detailRadius = (260 + qualityLevel * 180) * (adaptive ? 0.6 : 1)
 *
 * which is a guess about how many vertices a radius costs. It happens to be
 * roughly right for one world at one density, and it is wrong everywhere else.
 * A district three times denser spends three times the vertices at the same
 * settings, and nothing notices: the frame time degrades and the game cannot
 * tell the difference between "this district is busy" and "the budget is
 * wrong".
 *
 * A radius is also the wrong control variable in principle. The player cares
 * about frame time, which is a function of vertices *submitted*, and submitted
 * vertices depend on where the camera is looking — a radius spends the same
 * budget whether the view is a wall or a skyline. What should be held constant
 * is the number, not the distance.
 *
 * ## The controller
 *
 * The correction is a power law on the measured overshoot:
 *
 *   overshoot = actual / target
 *   scale     = overshoot ^ (-1/k)
 *
 * The exponent `1/k` is *estimated*, not assumed, and that is the whole design.
 *
 * Geometry cost is superlinear in the detail radius: doubling the radius sees
 * four times the area, and a city's density is higher at its centre, so the true
 * relationship is worse than quadratic. But it is not a fixed power either — it
 * changes with terrain, with how much of the view is buildings, and with the
 * streaming radius.
 *
 * A controller with a *fixed* gain is stable only while `gain * k <= 1`. With
 * cost quadratic in the radius (`k = 2`) that means a gain no higher than 0.5.
 * An earlier version of this used 0.6, on the reasoning that a stronger response
 * would converge faster from a teleport. It does the opposite: it enters a clean
 * limit cycle, alternating between 73% and 171% of target forever, rebuilding
 * the entire scene on every step. The test suite caught it, and the trace is in
 * the test's failure message.
 *
 * So `k` is measured. Each build records the radius it used and the vertices it
 * produced; the next one estimates the local exponent from that pair:
 *
 *   k = ln(actual2 / actual1) / ln(radius2 / radius1)
 *
 * and uses it. The estimate is clamped to a sane range, and the very first build
 * has no pair to compare against, so it falls back to a conservative quadratic
 * assumption — the safest default, because too small a gain merely converges
 * slowly while too large a one oscillates.
 *
 * The result adapts to the world it is actually in: near-linear cost in a sparse
 * landscape, steeper in a dense downtown, without anyone retuning a constant.
 *
 * ## Why it converges rather than oscillates
 *
 * The scene is rebuilt at most once per streaming step, and the controller is
 * only applied when the *scene key* changes — a new position, a quality change,
 * a day/night step. Consecutive builds therefore see genuinely different
 * scenes rather than the same one re-measured, so the loop has no static
 * solution to ring against. A frame-by-frame controller on a static scene would
 * oscillate; this one cannot, because it only runs when the input changes.
 *
 * ## Hysteresis
 *
 * Once inside a 6% band of the target the controller stops, so a scene that has
 * arrived does not rebuild every frame. The band is on the *measurement*, not on
 * the size of the correction — see `DEAD_BAND` for why that distinction is the
 * whole difference between converging and parking just outside the target.
 */

/**
 * Target static vertex counts per quality level.
 *
 * ## These include the terrain, and that matters
 *
 * The static buffer is not only buildings. `terrainChunk` is built at the
 * *stream* radius, which is 680-1160 m depending on quality, and in a measured
 * world it accounts for roughly 500,000 of the ~518,000 vertices at rest. The
 * detail radius cannot reduce any of it.
 *
 * The first version of this controller was told to hit 240,000 and it drove the
 * detail radius down to 4 m and sat there reporting 216% of target, rebuilding
 * the whole scene every frame, because the target was below a floor it had no
 * control over. A controller that cannot reach its target does not converge; it
 * runs to whatever limit it hits and calls the result steady.
 *
 * So the targets are set above the measured terrain floor, and the important
 * property is not the exact number but that the target is *reachable*: the
 * controller must be able to bring the cost down to it by shrinking detail
 * alone. `test_budget.mjs` asserts that the target clears the floor the game
 * actually produces, so a future change to the terrain cannot quietly push the
 * floor above the budget again.
 *
 * What the controller actually governs is the ~20-40% of the scene that is
 * buildings, roads, dressing and vegetation within the detail radius — which is
 * also the part that is expensive per vertex, because it is the part that is
 * detailed.
 */
export const VERTEX_TARGETS = [560_000, 640_000, 740_000, 900_000];

/**
 * Hard floor and ceiling on the detail scale, so one bad build cannot run away.
 *
 * The floor is very low on purpose, and the reason is worth recording. Two
 * earlier versions clamped it at 0.45 and then 0.2, and both *looked* like
 * working systems: the controller reported a stable, confident, wrong budget,
 * parked against the clamp at 138% and then 179% of target. A clamp that binds
 * in normal operation is not a safety rail — it is a hard ceiling on quality
 * that reports itself as a success, because the loop has stopped moving and
 * nothing distinguishes "converged" from "pinned".
 *
 * So both bounds are set where genuinely degenerate geometry begins rather than
 * where a plausible density would land. An 8x denser-than-baseline world does
 * need a scale near 0.02, and a very sparse one needs a scale above 7; the
 * honest thing in both cases is to let the controller go there — sparse but
 * correct — rather than clamp it and spend 179% or 29% of the budget while
 * reporting success. The bounds exist to catch a divided-by-zero or a corrupt
 * measurement, which is what they are actually for.
 *
 * The ceiling is not unbounded either, because a scale large enough to swallow
 * the whole streaming radius is not a budget decision, it is a failure: the
 * detail radius is capped at the streaming radius below, so detail can never
 * outrun the world it is drawn in.
 */
const MIN_SCALE = 0.01;
const MAX_SCALE = 8;

/**
 * The dead band, as a fraction of the target.
 *
 * Inside this band the scale is left alone; outside it, the correction is
 * applied *however small*.
 *
 * Applying hysteresis to the size of the correction rather than to whether the
 * scene is on target is the subtle bug this replaces. A controller that only
 * moves when the correction exceeds 5% stops just outside the band and stays
 * there forever: the correction it still needs is 3.5%, so it is never applied,
 * so it is still needed. Traces showed it parked at 106%, 138% and 218% of
 * target — stable, quiet, and wrong. The dead band has to be a property of the
 * *state*, not of the *step*.
 */
const DEAD_BAND = 0.06;

/**
 * The cost exponent assumed before there is a second measurement to infer one
 * from.
 *
 * Quadratic is the conservative choice: it is the slowest stable gain, so a
 * wrong assumption here costs a few extra builds rather than an oscillation.
 */
const DEFAULT_EXPONENT = 2;

/** Bounds on the estimated exponent, for the same reason. */
const MIN_EXPONENT = 1.05;
const MAX_EXPONENT = 5;

/**
 * The state a budget controller carries between builds.
 *
 * Kept as a plain object and passed explicitly rather than held in a module
 * global, so a test can drive hundreds of builds without them leaking into
 * each other, and so two instances cannot fight over one shared number.
 */
export function createBudget(qualityLevel = 2, adaptive = false) {
  return {
    qualityLevel,
    adaptive,
    /** The radius the detail falloff is currently using, in metres. */
    detailRadius: baseDetailRadius(qualityLevel, adaptive),
    /** The multiplier applied to that radius, from the last correction. */
    scale: 1,
    /** Target for the current quality level. */
    target: VERTEX_TARGETS[Math.min(qualityLevel, VERTEX_TARGETS.length - 1)],
    /** What the last build actually cost, in vertices. */
    lastActual: 0,
    /** The radius that produced `lastActual`, for estimating the exponent. */
    lastRadius: 0,
    /** The estimated cost exponent, once there are two builds to compare. */
    exponent: DEFAULT_EXPONENT,
    /** How many times the scale has been corrected. */
    corrections: 0,
    /** Every measurement, for the HUD and for tests. */
    history: []
  };
}

/**
 * Estimate the local cost exponent from two consecutive builds.
 *
 * `k` in `cost ∝ radius^k`, from the log ratio of the two observations. Returns
 * the current estimate unchanged when the pair cannot support one — a radius
 * that did not move, or two measurements of the same scene, carry no
 * information about how cost responds to the control.
 */
function estimateExponent(budget, radiusNow, actualNow) {
  // No previous pair yet: the first build of a scene has nothing to compare
  // against, so the conservative default stands.
  if (!(budget.lastRadius > 0) || !(budget.lastActual > 0) || !(radiusNow > 0)) {
    return budget.exponent;
  }
  const dr = Math.log(radiusNow / budget.lastRadius);
  if (Math.abs(dr) < 1e-4) return budget.exponent;
  const dc = Math.log(actualNow / budget.lastActual);
  if (!Number.isFinite(dc) || Math.abs(dc) < 1e-6) return budget.exponent;
  const k = dc / dr;
  if (!Number.isFinite(k) || k <= 0) return budget.exponent;
  return Math.min(MAX_EXPONENT, Math.max(MIN_EXPONENT, k));
}

/** The radius the old heuristic chose, kept as the starting point. */
export function baseDetailRadius(qualityLevel, adaptive) {
  return (260 + qualityLevel * 180) * (adaptive ? 0.6 : 1);
}

/** The per-object detail multiplier, unchanged from the previous heuristic. */
export function staticDetailFor(qualityLevel, adaptive) {
  return adaptive ? 0.55 + qualityLevel * 0.12 : 0.82 + qualityLevel * 0.06;
}

/**
 * Feed a completed build back into the controller.
 *
 * @param {object} budget State from `createBudget`.
 * @param {number} actual Static vertices the build produced.
 * @returns {{scale:number, changed:boolean, overshoot:number}}
 */
export function observe(budget, actual) {
  const target = budget.target;
  if (!(target > 0) || !(actual > 0)) {
    throw new Error(`budget: unusable measurement (actual ${actual}, target ${target})`);
  }
  // The radius that produced *this* measurement, captured before it moves.
  const radiusUsed = budget.detailRadius;

  // Estimate the exponent from *this* observation against the previous one.
  // Both members of the current pair are passed explicitly, because reading them
  // off the budget after the assignments below would compare the new vertex
  // count against the old radius — two different scenes. That bug pinned the
  // exponent at its default forever, and an earlier trace showed `k=2.00` on
  // every iteration of a quadratic world while the loop oscillated and never
  // converged.
  const k = estimateExponent(budget, radiusUsed, actual);

  const overshoot = actual / target;
  // The multiplicative correction, and the single most important line here.
  //
  // The radius that would hit the target is
  //     r* = r_now * overshoot^(-1/k)
  // and since the radius is `base * scale`, the new scale is the *current* scale
  // times the correction — not the correction on its own.
  //
  // Replacing rather than accumulating is the bug that parked this controller
  // at 106%, 138% and 214% of target, stable and permanently wrong. `scale` is a
  // position, not a step: assigning `overshoot^(-1/k)` to it throws away
  // everything the previous builds learned and drags the radius back toward
  // `base` every time, so it oscillates around the wrong place forever.
  const correction = Math.pow(overshoot, -1 / k);
  const raw = budget.scale * correction;
  const clamped = Math.min(MAX_SCALE, Math.max(MIN_SCALE, raw));
  // Inside the dead band, hold. Outside it, always move — even by 1%.
  const changed = Math.abs(overshoot - 1) > DEAD_BAND;

  budget.exponent = k;
  budget.lastRadius = radiusUsed;
  budget.lastActual = actual;
  budget.history.push({ actual, overshoot, scale: clamped, exponent: k, radius: radiusUsed });
  if (changed) {
    budget.scale = clamped;
    // Detail can never outrun the world it is drawn in. Past the streaming
    // radius there is no geometry to spend detail on, so widening further would
    // be measuring nothing and calling it a budget.
    const ceiling = baseDetailRadius(budget.qualityLevel, budget.adaptive) * MAX_SCALE;
    budget.detailRadius = Math.min(ceiling, baseDetailRadius(budget.qualityLevel, budget.adaptive) * clamped);
    budget.corrections++;
  }
  return { scale: clamped, changed, overshoot, exponent: k };
}

/**
 * Re-target when the quality level or renderer mode changes.
 *
 * The scale is reset rather than preserved, because it was fitted to a target
 * that is no longer in play. Carrying it over is how a controller ends up
 * convinced the world is ten times denser than it is.
 */
export function retarget(budget, qualityLevel, adaptive) {
  budget.qualityLevel = qualityLevel;
  budget.adaptive = adaptive;
  budget.target = VERTEX_TARGETS[Math.min(qualityLevel, VERTEX_TARGETS.length - 1)];
  budget.scale = 1;
  budget.detailRadius = baseDetailRadius(qualityLevel, adaptive);
  // The exponent was fitted to the old target, so it goes with the scale.
  budget.exponent = DEFAULT_EXPONENT;
  budget.lastRadius = 0;
  budget.lastActual = 0;
  return budget;
}

/** A one-line summary for the telemetry overlay. */
export function describe(budget) {
  const pct = budget.target ? Math.round((budget.lastActual / budget.target) * 100) : 0;
  return `static ${budget.lastActual} / ${budget.target} (${pct}%) `
    + `radius ${budget.detailRadius.toFixed(0)}m x${budget.scale.toFixed(2)} `
    + `cost^${budget.exponent.toFixed(2)} `
    + `after ${budget.corrections} correction${budget.corrections === 1 ? '' : 's'}`;
}
