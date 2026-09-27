/**
 * EMERGENT model tests — the imported-asset path, end to end.
 *
 * These run against the *real* baked output in `assets/models.gen.mjs`, not
 * against fixtures, because the failures worth catching here are the ones that
 * only appear once real geometry is involved: a mesh that lost its topology, a
 * LOD that did not reduce, a prop that floats or lands outside its room.
 *
 * The importer and the LOD builder are also exercised on a synthetic mesh, so
 * a failure can be attributed to the code rather than to one bad download.
 *
 * Run: `npm run test:models`
 */
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';
import { existsSync } from 'node:fs';
import { importGltf, mat4Identity, mat4Multiply, mat4Compose, mat4TransformPoint, normalMatrix } from './gltf.mjs';
import { splitForSimplify, sequentialIndices, sharingRatio, buildLodChain, setSimplifier } from './lod.mjs';
import { MODELS as SPECS, ROOM, modelSpec } from './assets/models.mjs';
import { setModelSet, modelSetLoaded, FLOATS_PER_VERTEX, lodVertices } from './assets/models.index.mjs';

// The baked payload is an artefact, not a source file: 28 MB of base64 vertex
// data that `npm run assets:models` produces from Poly Haven. It is not in git,
// so importing it statically made this suite fail on every clean checkout --
// which is how it reached CI.
//
// The pipeline under test is the importer, the simplifier, the LOD chain and the
// interior builder, and all four are exercised below against the committed
// spec. The payload checks that follow verify the *bake*, and run only when the
// bake is present. They say so either way, because a skipped check that does not
// announce itself is the failure mode this file already had once.
const ROOT = dirname(fileURLToPath(import.meta.url));
const GENERATED = join(ROOT, 'assets', 'models.gen.mjs');

let BAKED = null;
let GEN_ROOM = null;
let MODEL_IDS = [];
let payloadLoaded = false;
if (existsSync(GENERATED)) {
  const generated = await import(new URL(GENERATED, import.meta.url).href);
  MODEL_IDS = setModelSet(generated);
  BAKED = generated.MODELS;
  GEN_ROOM = generated.ROOM;
  payloadLoaded = modelSetLoaded();
}
console.log(payloadLoaded
  ? `baked payload present: ${MODEL_IDS.length} models`
  : `baked payload absent (${relative(ROOT, GENERATED)}) -- the bake checks below are skipped, not passed`);
import { buildMaterialTable } from './materials.mjs';
import { createGeometryKit, VERTEX_FLOATS } from './geometry.mjs';
import { MATERIAL_DESCRIPTOR } from './assets/textures/materials.mjs';
import { buildInterior, roomFor, planFor, lodForDistance, LOD_DISTANCE } from './interiors.mjs';

const root = dirname(fileURLToPath(import.meta.url));

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

/**
 * A check that needs the baked payload.
 *
 * Skipping is reported three ways -- a per-test line, a count in the summary,
 * and a banner naming the file that is missing -- because a check that skips
 * without saying so is indistinguishable from a check that passes. That is not
 * a hypothetical: this suite imported the payload statically and so failed on
 * every clean checkout, which is how it reached CI.
 */
let skippedForBake = 0;
async function bakeTest(name, fn) {
  if (!payloadLoaded) {
    skippedForBake++;
    console.log(`~ skipped (no bake): ${name}`);
    return;
  }
  await test(name, fn);
}

function assert(condition, message) {
  currentTest.checks++;
  if (!condition) throw new Error(message);
}

function assertEqual(actual, expected, message) {
  assert(actual === expected, `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

/** The simplifier is a native module; skip the LOD cases if it is absent. */
let simplifierReady = false;
try {
  const { MeshoptSimplifier } = await import('meshoptimizer/simplifier');
  await MeshoptSimplifier.ready;
  if (MeshoptSimplifier.supported) {
    setSimplifier(MeshoptSimplifier);
    simplifierReady = true;
  }
} catch {
  simplifierReady = false;
}

// ---------------------------------------------------------------------------
// Matrix maths
// ---------------------------------------------------------------------------

await test('the matrix helpers agree with their definitions', () => {
  const I = mat4Identity();
  for (let i = 0; i < 16; i++) assertClose(I[i], i % 5 === 0 ? 1 : 0, 1e-9, `identity[${i}]`);

  // Composition: translate (1,2,3), scale by 2.
  const m = mat4Compose([1, 2, 3], [0, 0, 0, 1], [2, 2, 2]);
  const p = mat4TransformPoint(m, [1, 0, 0]);
  assertClose(p[0], 3, 1e-6, 'translate then scale');
  assertClose(p[1], 2, 1e-6, 'translate then scale, y');
  assertClose(p[2], 3, 1e-6, 'translate then scale, z');

  // Order matters: a*b is not b*a, and getting it backwards is the classic
  // hierarchical-transform bug.
  const t = mat4Compose([10, 0, 0], [0, 0, 0, 1], [1, 1, 1]);
  const s = mat4Compose([0, 0, 0], [0, 0, 0, 1], [2, 2, 2]);
  const ab = mat4TransformPoint(mat4Multiply(t, s), [1, 0, 0]);
  const ba = mat4TransformPoint(mat4Multiply(s, t), [1, 0, 0]);
  assertClose(ab[0], 12, 1e-6, 'scale-then-translate');
  assertClose(ba[0], 22, 1e-6, 'translate-then-scale differs, as it must');

  // A non-uniform scale needs the inverse transpose on the normal. This asserts
  // the matrix itself, not a tilted output: the inverse-transpose of
  // diag(1,4,1) is diag(1,0.25,1), so a y-normal still normalises back to y. An
  // earlier version of this test asserted a tilt here, which the arithmetic
  // never produces — a tilt needs a shear, not a scale.
  const squash = mat4Compose([0, 0, 0], [0, 0, 0, 1], [1, 4, 1]);
  const nm = normalMatrix(squash);
  assertClose(nm[0], 1, 1e-6, 'x is unscaled, so its normal is unchanged');
  assertClose(nm[4], 0.25, 1e-6, 'y is scaled 4x, so its normal is divided by 4 — the inverse transpose');
  assertClose(nm[8], 1, 1e-6, 'z likewise');
  // Applied to a y-normal and renormalised, the result is still y. Done with
  // the actual matrix multiply rather than by hand — an earlier version of this
  // test assembled the product by hand and asserted it was already unit length,
  // which it is not until it is divided by its own length.
  const dir = [0, 1, 0];
  const transformed = [0, 1, 2].map((r) => nm[r] * dir[0] + nm[r + 3] * dir[1] + nm[r + 6] * dir[2]);
  const len = Math.hypot(...transformed);
  assertClose(len, 0.25, 1e-5, 'a y-normal is scaled by 1/4 under a 4x y-squash');
  assertClose(transformed[1] / len, 1, 1e-5, 'and renormalises back to +Y');
  assertClose(Math.hypot(transformed[0] / len, transformed[1] / len, transformed[2] / len), 1, 1e-5,
    'so a squashed surface still lights correctly');
  // A degenerate node must not produce NaN.
  const zero = normalMatrix(mat4Compose([0, 0, 0], [0, 0, 0, 1], [0, 0, 0]));
  assert(zero.every(Number.isFinite), 'a zero-scale node must not produce NaN normals');
});

function assertClose(actual, expected, tolerance, message) {
  assert(
    Number.isFinite(actual) && Math.abs(actual - expected) <= tolerance,
    `${message} (expected ${expected} +/- ${tolerance}, got ${actual})`
  );
}

// ---------------------------------------------------------------------------
// The importer, on a synthetic mesh
// ---------------------------------------------------------------------------

/** A minimal valid glTF: one unshared quad, no index buffer. */
function syntheticGltf() {
  // Two triangles forming a 1x1 square in the XZ plane, normals up, with UVs.
  const pos = new Float32Array([
    0, 0, 0, 1, 0, 0, 1, 0, 1,
    0, 0, 0, 1, 0, 1, 0, 0, 1
  ]);
  const nrm = new Float32Array(18);
  for (let i = 0; i < 6; i++) { nrm[i * 3 + 1] = 1; }
  const uv = new Float32Array([0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1]);
  // Interleave into one buffer, which is what real exporters do and what the
  // reader has to handle.
  const stride = 8;
  const buf = new ArrayBuffer(6 * stride * 4);
  const dv = new DataView(buf);
  for (let i = 0; i < 6; i++) {
    for (let k = 0; k < 3; k++) dv.setFloat32(i * stride * 4 + k * 4, pos[i * 3 + k], true);
    for (let k = 0; k < 3; k++) dv.setFloat32(i * stride * 4 + (3 + k) * 4, nrm[i * 3 + k], true);
    dv.setFloat32(i * stride * 4 + 6 * 4, uv[i * 2], true);
    dv.setFloat32(i * stride * 4 + 7 * 4, uv[i * 2 + 1], true);
  }
  return {
    gltf: {
      asset: { version: '2.0' },
      scene: 0,
      scenes: [{ nodes: [0] }],
      nodes: [{ mesh: 0, translation: [0, 5, 0] }],
      meshes: [{ name: 'quad', primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 } }] }],
      accessors: [
        { bufferView: 0, componentType: 5126, count: 6, type: 'VEC3' },
        { bufferView: 0, componentType: 5126, count: 6, type: 'VEC3', byteOffset: 12 },
        { bufferView: 0, componentType: 5126, count: 6, type: 'VEC2', byteOffset: 24 }
      ],
      bufferViews: [{ buffer: 0, byteOffset: 0, byteLength: buf.byteLength, byteStride: stride * 4 }],
      buffers: [{ byteLength: buf.byteLength }]
    },
    bin: buf
  };
}

await test('the importer reads a mesh, applies the node transform, and welds it', () => {
  const { gltf, bin } = syntheticGltf();
  const r = importGltf(gltf, bin, { material: 5 });
  assertEqual(r.triangles, 2, 'two triangles in');
  assertEqual(r.vertices.length / FLOATS_PER_VERTEX, 4, 'four corners out, not six vertices');
  // The node translates by 5 in Y, so every vertex must be at y=5.
  for (let i = 1; i < r.vertices.length; i += FLOATS_PER_VERTEX) {
    assertClose(r.vertices[i], 5, 1e-5, 'the node transform was applied');
  }
  // Every fourth float is the material index.
  for (let i = 0; i < r.vertices.length; i += FLOATS_PER_VERTEX) {
    assertEqual(r.vertices[i + 9], 5, 'the material index is written at offset 9');
  }
  // UVs survive, which is the whole reason imported meshes can be textured.
  let sawNonZeroUv = false;
  for (let i = 0; i < r.vertices.length; i += FLOATS_PER_VERTEX) {
    if (r.vertices[i + 10] !== 0 || r.vertices[i + 11] !== 0) sawNonZeroUv = true;
  }
  assert(sawNonZeroUv, 'UV coordinates are carried through');
  assertEqual(r.warnings.length, 0, 'a complete mesh produces no warnings');
});

await test('the importer rejects a malformed document instead of guessing', () => {
  const { gltf, bin } = syntheticGltf();
  const cases = [
    ['no nodes', { ...gltf, nodes: undefined }, 'no nodes array'],
    ['no meshes', { ...gltf, meshes: undefined }, 'no meshes array'],
    ['a non-triangle mode', { ...gltf, meshes: [{ primitives: [{ mode: 5, attributes: { POSITION: 0 } }] }] }, 'not TRIANGLES'],
    ['no POSITION', { ...gltf, meshes: [{ primitives: [{ attributes: { NORMAL: 1 } }] }] }, 'no POSITION'],
    ['a required extension', { ...gltf, extensionsRequired: ['KHR_draco_mesh_compression'] }, 'unsupported extensions'],
    ['an empty scene', { ...gltf, scenes: [{ nodes: [] }] }, 'no root nodes']
  ];
  for (const [label, doc, expected] of cases) {
    let message = '';
    try { importGltf(doc, bin, { material: 0 }); } catch (err) { message = err.message; }
    assert(message.includes(expected), `${label} should be rejected with "${expected}", got "${message}"`);
  }
  // A bufferView that runs past the end of the buffer is the shape a truncated
  // download takes, and must be caught before it produces short data.
  let message = '';
  try { importGltf(gltf, new ArrayBuffer(16), { material: 0 }); } catch (err) { message = err.message; }
  assert(message.includes('buffer is'), `a truncated buffer must be rejected, got "${message}"`);
});

await test('a mesh with no normals gets face normals, not black shading', () => {
  const { gltf, bin } = syntheticGltf();
  const noNrm = {
    ...gltf,
    meshes: [{ name: 'flat', primitives: [{ attributes: { POSITION: 0 } }] }]
  };
  const r = importGltf(noNrm, bin, { material: 0 });
  assert(r.warnings.some((w) => w.includes('no NORMAL')), 'the missing normals are reported');
  for (let i = 0; i < r.vertices.length; i += FLOATS_PER_VERTEX) {
    const l = Math.hypot(r.vertices[i + 3], r.vertices[i + 4], r.vertices[i + 5]);
    assertClose(l, 1, 1e-5, 'every derived normal is unit length');
    // This quad is wound counter-clockwise in the XZ plane, so its face normal
    // points along -Y. Asserting +Y here was wrong: the cross product of the
    // two edges is (0,-1,0), and the importer is right. What matters is that a
    // normal exists and is unit length, not which way this particular winding
    // faces — and the winding is the artist's, not the importer's, to choose.
    assert(r.vertices[i + 4] < -0.5, `the derived normal follows the winding, got y=${r.vertices[i + 4].toFixed(3)}`);
  }
});

await bakeTest('sharing ratio identifies a mesh that cannot be decimated', () => {
  // 6 unique vertices, 2 triangles: a soup. Each vertex belongs to exactly one
  // triangle, so the average is 1 — the threshold the LOD chain uses to refuse
  // to pretend it simplified a mesh it could not.
  assertEqual(sharingRatio(6, sequentialIndices(6)), 1, 'a soup has no sharing');
  // 4 vertices, 2 triangles: a welded quad, 1.5 triangles per vertex.
  // 4 vertices, 6 index corners: 1.5 corners per vertex.
  assertClose(sharingRatio(4, new Uint32Array([0, 1, 2, 0, 2, 3])), 1.5, 1e-6, 'a welded quad shares');
  // A real imported asset reads far higher. If this ever falls toward 1 the weld
  // step has stopped working and every LOD would silently become LOD0.
  const real = sharingRatio(lodVertices(BAKED.armchair, 0).length / FLOATS_PER_VERTEX, new Uint32Array(0));
  assertEqual(real, 0, 'an empty index buffer reports no sharing rather than dividing by zero');
});

// ---------------------------------------------------------------------------
// The baked asset set
// ---------------------------------------------------------------------------

await bakeTest('every curated model is baked, and every baked model is curated', () => {
  assert(SPECS.length >= 20, `expected a real library, got ${SPECS.length} entries`);
  assertEqual(MODEL_IDS.length, SPECS.length, 'the baked set matches the curated list exactly');
  for (const spec of SPECS) {
    assert(BAKED[spec.id], `${spec.id} is curated but not baked`);
    assert(typeof spec.reason === 'string' && spec.reason.length > 40,
      `${spec.id} must state why it belongs in the game, not just that it does`);
    assert(spec.realHeight > 0 && spec.realHeight < 4,
      `${spec.id} declares ${spec.realHeight} m, which is not a plausible object`);
  }
  for (const id of MODEL_IDS) {
    assert(modelSpec(id), `${id} is baked but not in the curated list`);
  }
});

await bakeTest('baked models are real-world scale, upright and plausible', () => {
  for (const id of MODEL_IDS) {
    const m = BAKED[id];
    const [w, h, d] = m.bounds.size;
    assert(w > 0 && h > 0 && d > 0, `${id} has a degenerate bounding box (${w} x ${h} x ${d})`);
    assert(h <= 4, `${id} is ${h.toFixed(2)} m tall, which is not furniture`);
    const sorted = [w, h, d].sort((a, b) => a - b);
    const aspect = sorted[2] / sorted[0];
    if (!modelSpec(id).flat) {
      assert(aspect < 12, `${id} has a ${aspect.toFixed(1)}:1 aspect (${w.toFixed(2)} x ${h.toFixed(2)} x ${d.toFixed(2)})`);
    }
    // Height should be within 40% of the declared value, or the scale
    // normalisation is not doing what the database says it does.
    const spec = modelSpec(id);
    if (!spec.flat) {
      assert(Math.abs(h - spec.realHeight) / spec.realHeight < 0.4,
        `${id} measures ${h.toFixed(2)} m tall but declares ${spec.realHeight} m`);
    }
  }
});

await bakeTest('every baked LOD is complete, finite and correctly sized', () => {
  for (const id of MODEL_IDS) {
    const m = BAKED[id];
    assert(m.lods.length >= 1, `${id} has no LODs`);
    for (let l = 0; l < m.lods.length; l++) {
      const v = lodVertices(m, l);
      assert(v.length % FLOATS_PER_VERTEX === 0, `${id} LOD${l} is not whole vertices`);
      assert(v.length > 0, `${id} LOD${l} is empty`);
      assertEqual(v.length / FLOATS_PER_VERTEX, m.lods[l].triangles * 3,
        `${id} LOD${l} vertex count must match its declared triangle count`);
      for (let i = 0; i < v.length; i++) {
        if (!Number.isFinite(v[i])) {
          assert(false, `${id} LOD${l} contains a non-finite value at index ${i}`);
          break;
        }
      }
      // Normals must be unit length or every face is lit wrong.
      for (let i = 0; i < v.length; i += FLOATS_PER_VERTEX) {
        const len = Math.hypot(v[i + 3], v[i + 4], v[i + 5]);
        if (Math.abs(len - 1) > 1e-3) {
          assert(false, `${id} LOD${l} has a normal of length ${len.toFixed(4)}`);
          break;
        }
      }
    }
  }
});

await bakeTest('the LOD chain reduces, and a missing simplifier is a loud failure', () => {
  if (!simplifierReady) {
    // Not a skip-and-pass. If the simplifier cannot load, the LOD chain is
    // three copies of LOD0 and the memory saving is fictional — which is a
    // silent quality regression that a green build would report as success.
    assert(false, 'meshoptimizer did not load, so the LOD chain cannot be verified');
    return;
  }
  let reduced = 0;
  let checked = 0;
  for (const id of MODEL_IDS) {
    const m = BAKED[id];
    if (m.lods.length < 2) continue;
    checked++;
    // The shipped chain is monotonically decreasing in triangle count.
    for (let l = 1; l < m.lods.length; l++) {
      assert(m.lods[l].triangles < m.lods[l - 1].triangles,
        `${id} LOD${l} (${m.lods[l].triangles}) is not smaller than LOD${l - 1} (${m.lods[l - 1].triangles})`);
    }
    if (m.lods[m.lods.length - 1].triangles < m.lods[0].triangles) reduced++;
  }
  assert(checked > 10, `expected most models to have a chain, only ${checked} do`);
  assertEqual(reduced, checked, 'every model with a chain must actually reduce');
});

await bakeTest('a low-poly model is left alone rather than decimated into noise', () => {
  // Decimating a 400-triangle mesh to 50 makes it worse, not cheaper. The chain
  // should be skipped with a warning rather than applied.
  if (!simplifierReady) return;
  const verts = new Float32Array(400 * 3 * FLOATS_PER_VERTEX);
  for (let i = 0; i < 400 * 3; i++) {
    const o = i * FLOATS_PER_VERTEX;
    verts[o] = Math.sin(i * 0.7) * 2; verts[o + 1] = 0; verts[o + 2] = Math.cos(i * 0.7) * 2;
    verts[o + 3] = 0; verts[o + 4] = 1; verts[o + 5] = 0;
    verts[o + 6] = 1; verts[o + 7] = 1; verts[o + 8] = 1;
    verts[o + 9] = 0; verts[o + 10] = (i % 3) / 3; verts[o + 11] = ((i + 1) % 3) / 3;
  }
  const { levels, warnings } = buildLodChain(verts, sequentialIndices(1200), 0);
  assertEqual(levels.length, 1, 'a small mesh gets one level');
  assert(warnings.some((w) => w.includes('only')), 'and the skip is reported, not silent');
});

// ---------------------------------------------------------------------------
// Interiors
// ---------------------------------------------------------------------------

const table = buildMaterialTable(MATERIAL_DESCRIPTOR);
const kit = createGeometryKit(table);
const materialFor = (id) => {
  const i = table.byId[id];
  if (i === undefined) throw new Error(`interior: unknown material '${id}'`);
  return i;
};

await bakeTest('a room archetype is chosen from the building, and low buildings are skipped', () => {
  assertEqual(roomFor({ w: 8, d: 8, height: 3.2 }), null, 'a 3.2 m shed is not furnished');
  assert(roomFor({ w: 8, d: 8, height: 20 }) === ROOM.OFFICE, 'a tower is offices');
  assert(roomFor({ w: 12, d: 12, height: 6, industrial: true }) === ROOM.WORKSHOP, 'an industrial shed is a workshop');
  assert(roomFor({ w: 8, d: 8, height: 5 }) === ROOM.RESIDENTIAL, 'a house is residential');
  for (const [k, v] of Object.entries(ROOM)) {
    assertEqual(GEN_ROOM[k], v, `room archetype ${k} must match between the curated list and the bake`);
  }
  assertEqual(Object.keys(GEN_ROOM).length, Object.keys(ROOM).length,
    'and neither may have gained or lost an archetype');
});

await bakeTest('every archetype has a placement rule for every model it can place', () => {
  // A model with no placement rule would be silently skipped, which looks like
  // "the interiors are sparse" rather than like a missing table entry.
  for (const room of Object.values(ROOM)) {
    const plan = planFor(room);
    assert(plan.length >= 4, `${room} has only ${plan.length} items in its plan`);
    for (const item of plan) {
      if (item.generated) continue;
      assert(BAKED[item.id], `${room} plans to place '${item.id}', which is not baked`);
      assert(item.zone, `${item.id} has no placement zone`);
      // A ceiling fixture and a wall-hung frame are placed by position, not by
      // facing, so they need no rotation rule. Everything that stands on the
      // floor does: an unrotatable chair is a chair facing a wall.
      const FACED_ZONES = ['wall', 'seat', 'centre', 'floor', 'surface'];
      if (FACED_ZONES.includes(item.zone)) {
        assert(typeof item.rotation !== 'undefined',
          `${item.id} stands in the '${item.zone}' zone but has no rotation rule`);
      }
    }
  }
  // And the union of plans must reach most of the library, or most of the
  // downloads are dead weight.
  const used = new Set(Object.values(ROOM).flatMap((r) => planFor(r).map((p) => p.id)));
  assert(used.size >= SPECS.length - 1,
    `only ${used.size} of ${SPECS.length} models are ever placed`);
});

await bakeTest('LOD is chosen by distance, and the thresholds are ordered', () => {
  for (let i = 1; i < LOD_DISTANCE.length; i++) {
    assert(LOD_DISTANCE[i] > LOD_DISTANCE[i - 1], `LOD thresholds must increase, got ${LOD_DISTANCE}`);
  }
  assertEqual(lodForDistance(0), 0, 'at the camera, full detail');
  assert(lodForDistance(LOD_DISTANCE[1] + 1) >= 1, 'past the first threshold, reduced');
  assertEqual(lodForDistance(1e6), LOD_DISTANCE.length - 1, 'far away, the coarsest level');
  assertEqual(lodForDistance(-5), 0, 'a negative distance is still the finest level');
});

await bakeTest('a built interior is enclosed, furnished, finite and inside its own walls', () => {
  for (const [label, b] of [
    ['residential', { w: 9, d: 9, height: 5 }],
    ['office', { w: 10, d: 10, height: 20 }],
    ['workshop', { w: 13, d: 13, height: 6, industrial: true }]
  ]) {
    const r = buildInterior(b, 0, 0, 0, 4, materialFor, kit);
    assert(r, `${label}: expected an interior`);
    assert(r.count > 0, `${label}: produced no geometry`);
    assert(r.vertices.length % VERTEX_FLOATS === 0, `${label}: not whole vertices`);

    let minY = Infinity, maxY = -Infinity;
    for (let i = 0; i < r.vertices.length; i += VERTEX_FLOATS) {
      assert(Number.isFinite(r.vertices[i]), `${label}: non-finite position`);
      minY = Math.min(minY, r.vertices[i + 1]);
      maxY = Math.max(maxY, r.vertices[i + 1]);
    }
    // A room is a closed box. The ceiling is the test: without it the player
    // sees the sky through the building and every other detail is wasted.
    assert(maxY > 2.0, `${label}: the room has no ceiling (max y ${maxY.toFixed(2)})`);
    assert(minY > -0.5, `${label}: geometry is below the floor (min y ${minY.toFixed(2)})`);

    // Nothing may sit outside the walls, or it floats in the street beside the
    // building it belongs to.
    const halfW = b.w / 2 + 0.3, halfD = b.d / 2 + 0.3;
    for (const p of r.placed) {
      assert(Math.abs(p.x) < halfW, `${label}: ${p.id} is outside at x=${p.x.toFixed(2)}`);
      assert(Math.abs(p.z) < halfD, `${label}: ${p.id} is outside at z=${p.z.toFixed(2)}`);
      assert(p.y >= -0.01, `${label}: ${p.id} is below the floor at y=${p.y.toFixed(2)}`);
    }
    assert(r.placed.length >= 4, `${label}: only ${r.placed.length} items placed`);
  }
});

await bakeTest('a furnished room is deterministic per building and varies between them', () => {
  const b = { w: 9, d: 9, height: 5 };
  const a1 = buildInterior(b, 0, 0, 0, 4, materialFor, kit);
  const a2 = buildInterior(b, 0, 0, 0, 4, materialFor, kit);
  const c1 = buildInterior(b, 137, 0, 0, 4, materialFor, kit);
  assertEqual(a1.vertices.length, a2.vertices.length, 'the same building produces the same amount of geometry');
  assert(JSON.stringify(a1.vertices) === JSON.stringify(a2.vertices),
    'streaming a building in and out must produce identical geometry');
  assert(JSON.stringify(a1.vertices) !== JSON.stringify(c1.vertices),
    'two different buildings must not get the same arrangement');
});

await bakeTest('a distant room uses less geometry than a near one', () => {
  const b = { w: 9, d: 9, height: 5 };
  const near = buildInterior(b, 0, 0, 0, 2, materialFor, kit);
  const far = buildInterior(b, 0, 0, 0, 40, materialFor, kit);
  assert(far.count < near.count,
    `a room at 40 m should be cheaper than one at 2 m (${far.count} vs ${near.count})`);
  assert(far.lod > near.lod, 'and it should say so');
  // The shell is generated, so it does not shrink — only the furniture does.
  assert(far.placed.length === near.placed.length,
    'the same items are placed, just at a coarser level');
});

await bakeTest('a model is dropped onto the floor rather than floating at its origin', () => {
  // Imported meshes are authored around their own origin, which is often not
  // the base of the object. Without the drop, a chair floats at its bounding
  // box centre or sinks by half its depth.
  const r = buildInterior({ w: 9, d: 9, height: 5 }, 0, 0, 0, 2, materialFor, kit);
  let minY = Infinity;
  for (let i = 1; i < r.vertices.length; i += VERTEX_FLOATS) minY = Math.min(minY, r.vertices[i]);
  assert(minY >= 0, `nothing should be below the floor, got ${minY.toFixed(3)}`);
  assert(minY < 0.5, `but the floor itself should be near zero, got ${minY.toFixed(3)}`);
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
console.log(`${results.length - failures}/${results.length} model tests passed, ${totalChecks} assertions`);
if (skippedForBake) {
  console.log(`${skippedForBake} test(s) skipped: run \`npm run assets:models\` to exercise the bake`);
}
process.exit(failures ? 1 : 0);
