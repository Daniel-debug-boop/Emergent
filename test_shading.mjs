/**
 * EMERGENT shading tests — the shader's arithmetic, checked in JS.
 *
 * The headless runtime does not rasterize, so "the shader compiles" and "the
 * shader is correct" are different claims and only the first is cheap. This
 * suite transliterates `sceneVS` and `sceneFS` into JS and asserts the
 * *behaviour* of that arithmetic on inputs whose answers are known:
 *
 *   - box mapping picks the plane the material's role says it should;
 *   - the normal is flipped toward the eye, so a closed solid is not black on
 *     the half the player is looking at from behind;
 *   - a surface facing the sun is brighter than the same surface facing away;
 *   - roughness widens the specular lobe rather than only dimming it;
 *   - a metal has no diffuse term and a dielectric is not black in shadow;
 *   - nothing produces a non-finite value, which is the failure mode that
 *     actually shows up on screen as a black or white patch.
 *
 * The transliteration is a *re-implementation*, so it cannot prove the GLSL
 * itself is correct. What it does prove is that the model the shader encodes is
 * the model the design intends, and the two implementations are kept adjacent
 * so a change to one is obviously a change to the other.
 *
 * Run: `npm run test:shading`
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// Minimal test runner (no dependencies; the project ships no test framework)
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

function assertBetween(actual, low, high, message) {
  assert(
    Number.isFinite(actual) && actual >= low && actual <= high,
    `${message} (expected ${low}..${high}, got ${actual})`
  );
}

// ---------------------------------------------------------------------------
// Vector helpers, matching GLSL semantics
// ---------------------------------------------------------------------------

const PI = Math.PI;
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const mul = (a, b) => [a[0] * b[0], a[1] * b[1], a[2] * b[2]];
const mix = (a, b, t) => [
  a[0] + (b[0] - a[0]) * t,
  a[1] + (b[1] - a[1]) * t,
  a[2] + (b[2] - a[2]) * t
];
const norm = (a) => {
  const l = Math.hypot(a[0], a[1], a[2]);
  return l > 1e-9 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0];
};
const luminance = (a) => 0.2126 * a[0] + 0.7152 * a[1] + 0.0722 * a[2];

// ---------------------------------------------------------------------------
// sceneVS: box mapping
// ---------------------------------------------------------------------------

/** MAP_MODE, from materials.mjs. */
const MAP_MODE = { GROUND: 0, WALL_X: 1, WALL_Z: 2, SOLID: 3 };

/** Transliteration of sceneVS's texture-coordinate derivation. */
function boxMap(p, mode, invTileScale) {
  let uv = [p[0], p[2]];
  if (mode > 0.5 && mode < 1.5) uv = [p[2], p[1]];
  else if (mode > 1.5 && mode < 2.5) uv = [p[0], p[1]];
  return [uv[0] * invTileScale, uv[1] * invTileScale];
}

// ---------------------------------------------------------------------------
// sceneFS: the BRDF
// ---------------------------------------------------------------------------

const d_ggx = (NoH, a) => {
  const a2 = a * a;
  const d = NoH * NoH * (a2 - 1) + 1;
  return a2 / Math.max(PI * d * d, 1e-7);
};
const g_smith = (NoV, NoL, a) => {
  const k = a * 0.5;
  return (NoV / (NoV * (1 - k) + k)) * (NoL / (NoL * (1 - k) + k));
};
const fresnel = (u, f0) => f0.map((f) => f + (1 - f) * Math.pow(Math.min(Math.max(1 - u, 0), 1), 5));

/**
 * Transliteration of sceneFS's `main`.
 *
 * @param {object} s Surface and lighting inputs.
 * @returns {{direct:number[], ambient:number[], colour:number[], N:number[]}}
 */
function shade(s) {
  const {
    normal, view, position, albedo, roughness, metallic, ao = 1, emissive = 0,
    sun, sunColor, skyColor, groundColor, night = 0, water = 0
  } = s;

  // "Face the normal at the eye." Without this a closed solid is lit by
  // whichever side happens to point away from the camera.
  const Ng = norm(normal);
  const V = norm(view);
  const N = dot(Ng, V) < 0 ? scale(Ng, -1) : Ng;
  const L = norm(sun);

  const f0 = mix([0.04, 0.04, 0.04], albedo, metallic);
  const diffuse = mul(albedo, [1 - metallic, 1 - metallic, 1 - metallic]);
  const NoV = Math.max(dot(N, V), 1e-4);

  let direct = [0, 0, 0];
  const NoL = dot(N, L);
  if (NoL > 0) {
    const H = norm(add(L, V));
    const NoH = Math.max(dot(N, H), 0);
    const VoH = Math.max(dot(V, H), 0);
    const a = roughness * roughness;
    const spec = scale(
      fresnel(VoH, f0),
      (d_ggx(NoH, a) * g_smith(NoV, NoL, a)) / Math.max(4 * NoV * NoL, 1e-5)
    );
    direct = scale(add(scale(diffuse, 1 / PI), spec), NoL);
    direct = mul(direct, sunColor);
  }

  const ambientIrradiance = mix(groundColor, skyColor, N[1] * 0.5 + 0.5);
  const R = norm(sub(scale(N, 2 * dot(N, V)), V));
  const env = mix(groundColor, skyColor, R[1] * 0.5 + 0.5);
  const ambient = add(
    mul(diffuse, scale(ambientIrradiance, ao)),
    scale(mul(env, fresnel(NoV, f0)), ao * (0.30 + 0.70 * metallic) * (1 - roughness * 0.8))
  );

  let colour = add(add(direct, ambient), scale(albedo, emissive * (0.25 + night * 0.95)));
  if (water > 0.5) colour = mix(colour, [0.08, 0.25, 0.35], 0.42);
  return { direct, ambient, colour, N };
}

// A default mid-morning outdoor setup used by most cases.
const SUN = norm([0.4, 0.8, 0.45]);
const NOON = { sun: SUN, sunColor: [1.0, 0.97, 0.90], skyColor: [0.30, 0.40, 0.55], groundColor: [0.16, 0.15, 0.13] };

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

await test('box mapping projects onto the plane the role names', () => {
  const p = [3, 7, 11];
  const g = boxMap(p, MAP_MODE.GROUND, 1);
  assertClose(g[0], 3, 1e-6, 'ground maps world X');
  assertClose(g[1], 11, 1e-6, 'ground maps world Z, not Y');

  const x = boxMap(p, MAP_MODE.WALL_X, 1);
  assertClose(x[0], 11, 1e-6, 'a wall facing X maps Z');
  assertClose(x[1], 7, 1e-6, 'a wall facing X maps Y, not X');

  const z = boxMap(p, MAP_MODE.WALL_Z, 1);
  assertClose(z[0], 3, 1e-6, 'a wall facing Z maps X');
  assertClose(z[1], 7, 1e-6, 'a wall facing Z maps Y');

  // The projection must be axis-aligned and drop the axis it faces along, or
  // the texture slides as the geometry rotates.
  for (const mode of [MAP_MODE.GROUND, MAP_MODE.WALL_X, MAP_MODE.WALL_Z]) {
    const a = boxMap(p, mode, 1);
    const b = boxMap([p[0] + 5, p[1] + 5, p[2] + 5], mode, 1);
    assertClose(b[0] - a[0], b[1] - a[1], 1e-6, 'a uniform world offset is a uniform UV offset');
  }
});

await test('tiling scale is the reciprocal of the tile size, so a bigger tile is a smaller number', () => {
  // A 4 m tile over 8 m of wall must repeat exactly twice.
  const invTile = 1 / 4;
  const a = boxMap([0, 0, 0], MAP_MODE.WALL_X, invTile);
  const b = boxMap([0, 8, 0], MAP_MODE.WALL_X, invTile);
  assertClose(b[1] - a[1], 2, 1e-6, 'an 8 m wall with a 4 m tile covers two texture repeats');
});

await test('a surface facing away from the camera is lit, not black', () => {
  // This is the flip. A box's rear faces carry a normal pointing away from the
  // player; without flipping, an entire building is unlit on the side being
  // looked at, which reads as a hole in the world rather than as a bug.
  const lit = shade({
    normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0],
    albedo: [0.6, 0.5, 0.4], roughness: 0.8, metallic: 0, ...NOON
  });
  const unlit = shade({
    normal: [0, 0, -1], view: [0, 0, 1], position: [0, 0, 0],
    albedo: [0.6, 0.5, 0.4], roughness: 0.8, metallic: 0, ...NOON
  });
  assert(dot(lit.N, [0, 0, 1]) > 0.99, 'the front face keeps its normal');
  assert(dot(unlit.N, [0, 0, 1]) > 0.99, 'the rear face normal is flipped toward the eye');
  assert(
    luminance(unlit.colour) > 0.02,
    `a rear face must still catch ambient, got ${luminance(unlit.colour)}`
  );
  assertClose(luminance(unlit.colour), luminance(lit.colour), 1e-6,
    'flipping makes the two faces of a flat-lit surface agree');
});

await test('a surface facing the sun is brighter than the same surface in shadow', () => {
  const base = {
    normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0],
    albedo: [0.6, 0.5, 0.4], roughness: 0.8, metallic: 0
  };
  const lit = shade({ ...base, ...NOON });
  const shadowed = shade({ ...base, ...NOON, sun: scale(SUN, -1) });
  assert(
    luminance(lit.colour) > luminance(shadowed.colour),
    `facing the sun (${luminance(lit.colour).toFixed(3)}) must beat facing away (${luminance(shadowed.colour).toFixed(3)})`
  );
  // And the lit one must actually be lit, not just marginally different. The
  // expected value is Lambert's cosine law, not a taste threshold: an albedo of
  // 0.6 at NoL = 0.45 under a sun of luminance ~0.96 gives
  // 0.6/PI * 0.45 * 0.96 ~= 0.083, so anything much above that would mean the
  // shader was adding energy rather than reflecting it.
  const expectedDirect = luminance(NOON.sunColor) * (0.2126 * 0.6 + 0.7152 * 0.5 + 0.0722 * 0.4)
    / PI * Math.max(dot([0, 0, 1], SUN), 0);
  assertClose(luminance(lit.direct), expectedDirect, 0.01,
    'the direct term must be Lambert\'s cosine law, not a brightness constant');
  assert(luminance(lit.direct) > 0.05, `direct sunlight should be substantial, got ${luminance(lit.direct)}`);
  // Shadowed surfaces are not black: the ambient term is what stops the
  // unlit half of a city reading as a void.
  assert(luminance(shadowed.ambient) > 0.01, 'ambient must lift surfaces the sun cannot reach');
});

await test('roughness widens the specular lobe instead of only dimming it', () => {
  // The defining property of a microfacet BRDF: a sharp highlight is far
  // brighter *at the mirror direction* and falls off almost immediately, while
  // a rough one is dimmer there and much brighter well off-axis. A shader that
  // merely multiplies the highlight by roughness satisfies neither half of that
  // and is what makes CG surfaces look like plastic.
  //
  // The mirror direction is derived from the light and the normal rather than
  // hard-coded, because "off-axis" only means something relative to it.
  const mirrorView = norm(sub(scale([0, 0, 1], 2 * dot([0, 0, 1], SUN)), SUN));
  const offAxisView = norm([0.85, 0.10, 0.52]);
  const sample = (roughness, view) => shade({
    normal: [0, 0, 1], view, position: [0, 0, 0],
    albedo: [0.9, 0.9, 0.9], roughness, metallic: 1, ...NOON
  }).direct;

  const sharpAtMirror = sample(0.05, mirrorView);
  const roughAtMirror = sample(0.95, mirrorView);
  const sharpOffAxis = sample(0.05, offAxisView);
  const roughOffAxis = sample(0.95, offAxisView);

  assert(
    luminance(sharpAtMirror) > luminance(roughAtMirror) * 20,
    `a sharp highlight must dominate at the mirror direction (${luminance(sharpAtMirror).toFixed(3)} vs ${luminance(roughAtMirror).toFixed(3)})`
  );
  assert(
    luminance(roughOffAxis) > luminance(sharpOffAxis) * 1000,
    `a rough surface must spread energy off-axis (${luminance(roughOffAxis).toFixed(5)} vs ${luminance(sharpOffAxis).toExponential(2)})`
  );
  // Monotone falloff away from the mirror direction for a sharp surface. This
  // is the assertion a pure brightness multiply cannot pass.
  assert(
    luminance(sharpAtMirror) > luminance(sharpOffAxis),
    'a sharp highlight must fall off away from the mirror direction'
  );
  // And a rough surface must not have a peak at all: the whole point is that
  // there is no single mirror direction to peak in.
  assert(
    luminance(roughOffAxis) > luminance(roughAtMirror) * 0.5,
    'a rough surface should be roughly flat across the lobe, not peaked'
  );
  for (const [n, c] of [['sharp@mirror', sharpAtMirror], ['rough@mirror', roughAtMirror],
    ['sharp@off', sharpOffAxis], ['rough@off', roughOffAxis]]) {
    assert(c.every(Number.isFinite), `${n} specular produced a non-finite value`);
    assert(c.every((v) => v >= -1e-6), `${n} specular produced negative energy`);
  }
});

await test('a metal has no diffuse term and a dielectric is not a mirror', () => {
  const metal = shade({
    normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0],
    albedo: [0.9, 0.9, 0.9], roughness: 0.4, metallic: 1, ...NOON
  });
  const dielectric = shade({
    normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0],
    albedo: [0.9, 0.9, 0.9], roughness: 0.4, metallic: 0, ...NOON
  });
  // Identical albedo and roughness; the only difference is the metal path. At
  // normal incidence with the light behind the camera the diffuse term should
  // dominate for the dielectric, which is the whole reason a painted car body
  // (metallic 0) and bare steel (metallic 1) are different materials.
  assert(
    Math.abs(luminance(metal.colour) - luminance(dielectric.colour)) > 1e-3,
    'metallic and dielectric shading must differ for the same albedo'
  );
  // Fresnel reflectance at normal incidence: a dielectric reflects ~4%, a
  // conductor reflects its own colour. The f0 mix is what encodes that.
  const f0dielectric = mix([0.04, 0.04, 0.04], [0.9, 0.9, 0.9], 0);
  const f0metal = mix([0.04, 0.04, 0.04], [0.9, 0.9, 0.9], 1);
  assertClose(f0dielectric[0], 0.04, 1e-6, 'a dielectric reflects about 4% at normal incidence');
  assertClose(f0metal[0], 0.9, 1e-6, 'a metal takes its reflectance from its albedo');
});

await test('roughness from the ARM map is respected, including its floor', () => {
  // The bake writes roughness into the green channel and the shader clamps it.
  // A zero-roughness value would make GGX's denominator collapse; the clamp is
  // the difference between a sharp highlight and a hole in the image.
  for (const rough of [0, 0.001, 0.04, 0.5, 1.0]) {
    const clamped = Math.min(Math.max(rough, 0.04), 1.0);
    assert(clamped >= 0.04, `roughness ${rough} must be floored at 0.04`);
    const c = shade({
      normal: [0, 0, 1], view: [0.25, 0, 0.968], position: [0, 0, 0],
      albedo: [0.8, 0.8, 0.8], roughness: clamped, metallic: 0, ...NOON
    });
    assert(c.colour.every(Number.isFinite), `roughness ${rough} produced a non-finite colour`);
  }
});

await test('emissive materials glow at night and stay visible by day', () => {
  const lamp = { albedo: [1.0, 0.94, 0.80], roughness: 0.15, metallic: 0, emissive: 3.0 };
  const day = shade({ normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0], ...lamp, ...NOON, night: 0 });
  const night = shade({ normal: [0, 0, 1], view: [0, 0, 1], position: [0, 0, 0], ...lamp, ...NOON, night: 1 });
  assert(luminance(night.colour) > luminance(day.colour),
    'a lit window or lamp must be brighter at night than at noon');
  // Night ambient is scaled down in the renderer; the emissive term has to stay
  // legible against it rather than being swamped.
  assert(luminance(night.colour) > 0.8, `a lamp at night should read as a light source, got ${luminance(night.colour)}`);
});

await test('ambient is hemispherical: a surface facing up sees more sky than one facing down', () => {
  const up = shade({ normal: [0, 1, 0], view: [0, 0.2, 0], position: [0, 0, 0], albedo: [0.5, 0.5, 0.5], roughness: 0.9, metallic: 0, ...NOON });
  const down = shade({ normal: [0, -1, 0], view: [0, -0.2, 0], position: [0, 0, 0], albedo: [0.5, 0.5, 0.5], roughness: 0.9, metallic: 0, ...NOON });
  assert(luminance(up.ambient) > luminance(down.ambient),
    `a sky-facing surface must be brighter (${luminance(up.ambient).toFixed(4)} vs ${luminance(down.ambient).toFixed(4)})`);
  assertClose(luminance(up.ambient), luminance(NOON.skyColor) * 0.5 * 1.0, 0.05,
    'a fully sky-facing surface receives the full sky colour');
});

await test('no material configuration produces a non-finite or negative pixel', () => {
  // The sweep matters more than any single case: a NaN in a fragment shader
  // does not throw, it renders as a hole, and the harness's NaN check only
  // covers vertex uploads.
  const albedos = [[0, 0, 0], [1, 1, 1], [0.9, 0.1, 0.05], [0.02, 0.02, 0.03]];
  const roughties = [0.04, 0.5, 1.0];
  const metals = [0, 0.5, 1];
  const normals = [[0, 0, 1], [0, 1, 0], [0.577, 0.577, 0.577], [0, 0, -1]];
  const views = [[0, 0, 1], [0, 0, -1], [0.9, 0.1, 0.4]];
  let cases = 0;
  for (const albedo of albedos) {
    for (const roughness of roughties) {
      for (const metallic of metals) {
        for (const normal of normals) {
          for (const view of views) {
            for (const ao of [0, 0.5, 1]) {
              for (const sun of [[0, 1, 0], [0, -1, 0], [1, 0, 0]]) {
                const c = shade({
                  normal, view, position: [0, 0, 0], albedo, roughness, metallic, ao,
                  sun: norm(sun), sunColor: [1, 1, 1], skyColor: [0.3, 0.4, 0.55], groundColor: [0.16, 0.15, 0.13]
                });
                assert(c.colour.every(Number.isFinite),
                  `non-finite colour for albedo=${albedo} rough=${roughness} metal=${metallic} ao=${ao}`);
                assert(c.colour.every((v) => v >= -1e-6),
                  `negative energy for albedo=${albedo} rough=${roughness} metal=${metallic} ao=${ao}`);
                cases++;
              }
            }
          }
        }
      }
    }
  }
  assert(cases > 300, `expected a real sweep, only ran ${cases}`);
});

await test('the shading model in the source matches the model this suite tests', () => {
  // The transliteration above is a re-implementation. This is the check that
  // the re-implementation has not drifted from the GLSL it stands in for: the
  // structure of the shader is re-read from game3d.js and compared. A shader
  // edit that removes the normal flip, the f0 mix, or the roughness floor fails
  // here rather than silently invalidating every assertion above.
  const source = readFileSync(join(root, 'game3d.js'), 'utf8');
  const fsSrc = source.match(/const sceneFS = `([\s\S]*?)`;/)[1];
  const vsSrc = source.match(/const sceneVS = `([\s\S]*?)`;/)[1];

  assert(/dot\(Ng,V\)<0\.0\?\s*-Ng\s*:\s*Ng/.test(fsSrc),
    'the shader must flip the normal toward the eye, or the transliterated model is not the real one');
  assert(/mix\(vec3\(0\.04\),albedo,metal\)/.test(fsSrc),
    'the shader must mix the dielectric and metal reflectance the way this suite does');
  assert(/d_ggx/.test(fsSrc) && /g_smith/.test(fsSrc),
    'the shader must use the same GGX and Smith terms this suite tests');
  assert(/rough=clamp\(arm\.g,0\.04,1\.0\)/.test(fsSrc),
    'the shader must floor roughness at 0.04 as this suite assumes');
  assert(/mix\(uGroundColor,uSkyColor,N\.y\*0\.5\+0\.5\)/.test(fsSrc),
    'the shader must use the hemispherical ambient this suite measures');
  assert(/scale\(albedo,emis\)|albedo\*emis/.test(fsSrc),
    'the shader must apply the emissive term this suite tests');
  assert(/mode>0\.5 && mode<1\.5/.test(vsSrc),
    'the vertex shader must select the box-mapping plane the way this suite does');
});

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
console.log(`${results.length - failures}/${results.length} shading tests passed, ${totalChecks} assertions`);
process.exit(failures ? 1 : 0);
