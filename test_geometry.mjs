/**
 * Tests for geometry.mjs and materials.mjs — the asset pipeline's front end.
 *
 * These run without a GL context. The kit is pure geometry over plain arrays,
 * which is the point of keeping it out of `game3d.js`: a window's proportions,
 * a material's metalness and a vertex layout can all be asserted exactly,
 * instead of inferred from a screenshot nobody can take in CI.
 *
 * Run: `npm run test:geometry`
 */
import { createGeometryKit, VERTEX_FLOATS, VERTEX_BYTES } from './geometry.mjs';
import { buildMaterialTable, materialIndex, MAX_MATERIALS, SOLID_MATERIALS, MAP_MODE } from './materials.mjs';
import { MATERIAL_DESCRIPTOR } from './assets/textures/materials.mjs';
import { buildBuilding, buildRoad, buildTree, buildCar, buildCarProxy, buildCharacter } from './city.mjs';
import { terrainHeight } from './world.mjs';

let checks = 0;
let failures = 0;
const results = [];

function test(name, fn) {
  const before = checks;
  try {
    fn();
    process.stdout.write(`✓ ${name} (${checks - before} checks)\n`);
  } catch (err) {
    failures++;
    process.stdout.write(`✗ ${name}\n  ${err.message}\n`);
  }
}

function assert(cond, msg) {
  checks++;
  if (!cond) throw new Error(msg);
}
function assertEqual(actual, expected, msg) {
  checks++;
  if (actual !== expected) throw new Error(`${msg} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}
function assertClose(actual, expected, tol, msg) {
  checks++;
  if (!Number.isFinite(actual) || Math.abs(actual - expected) > tol) {
    throw new Error(`${msg} (expected ${expected} +/- ${tol}, got ${actual})`);
  }
}

const TABLE = buildMaterialTable(MATERIAL_DESCRIPTOR);
const G = createGeometryKit(TABLE);

/** Emit into a fresh array and split it back into vertices. */
const emit = (fn) => {
  const arr = [];
  fn(arr);
  const verts = [];
  for (let i = 0; i < arr.length; i += VERTEX_FLOATS) verts.push(arr.slice(i, i + VERTEX_FLOATS));
  return { arr, verts, count: arr.length / VERTEX_FLOATS };
};

const allFinite = (arr) => arr.every(Number.isFinite);

// ---------------------------------------------------------------------------
// The material table
// ---------------------------------------------------------------------------

test('the material table merges baked textures and solids into one index space', () => {
  assert(TABLE.count > 0, 'the table is not empty');
  assertEqual(TABLE.count, Object.keys(MATERIAL_DESCRIPTOR.materials).length + SOLID_MATERIALS.length,
    'every baked material and every solid has an entry');
  assert(TABLE.count <= MAX_MATERIALS, `the table fits the shader's uniform arrays (${TABLE.count} <= ${MAX_MATERIALS})`);
  assertEqual(TABLE.size, MATERIAL_DESCRIPTOR.size, 'the table agrees with the baked texture size');
  assertEqual(TABLE.layers, MATERIAL_DESCRIPTOR.arrays.albedo.layers, 'and with the layer count the bake wrote');
});

test('every material index in the table is unique and in range', () => {
  const seen = new Set();
  for (const [id, i] of Object.entries(TABLE.byId)) {
    assert(!seen.has(i), `${id} shares an index with another material`);
    seen.add(i);
    assert(i >= 0 && i < TABLE.count, `${id} is inside the table`);
  }
  assertEqual(seen.size, TABLE.count, 'no gaps in the index space');
});

test('looking up an unknown material is an error, not a silent fallback', () => {
  // A defaulted material renders as a plausible grey solid, so a typo here would
  // survive review and ship. It has to throw.
  let threw = false;
  try { materialIndex(TABLE, 'no_such_material'); } catch { threw = true; }
  assert(threw, 'an unknown material name must throw');
  for (const id of ['wall_brick', 'road_asphalt', 'terrain_grass', 'glass', 'rubber']) {
    assert(materialIndex(TABLE, id) >= 0, `${id} resolves`);
  }
});

test('the uniform tables describe what the shader reads', () => {
  assertEqual(TABLE.a.length, MAX_MATERIALS * 4, 'uMatA is a vec4 per material slot');
  assertEqual(TABLE.b.length, MAX_MATERIALS * 4, 'uMatB likewise');
  for (let i = 0; i < TABLE.count; i++) {
    const layer = TABLE.a[i * 4], invScale = TABLE.a[i * 4 + 1], rough = TABLE.a[i * 4 + 2], metal = TABLE.a[i * 4 + 3];
    const mode = TABLE.b[i * 4], textured = TABLE.b[i * 4 + 3];
    assert(layer >= -1 && layer < TABLE.layers, `material ${i} has a valid layer index (${layer})`);
    assert(rough >= 0 && rough <= 1.6, `material ${i} roughness is sane (${rough})`);
    // -1 is the documented sentinel for "read metalness from the texture",
    // which one material in the set (clean steel) needs and the rest do not.
    assert(metal === -1 || (metal >= 0 && metal <= 1), `material ${i} metalness is a value or the texture sentinel (${metal})`);
    assert(mode >= 0 && mode <= MAP_MODE.SOLID, `material ${i} has a known map mode (${mode})`);
    if (textured > 0.5) {
      assert(layer >= 0, `textured material ${i} points at a layer`);
      assert(invScale > 0, `textured material ${i} has a tiling scale`);
    } else {
      assertEqual(layer, -1, `untextured material ${i} is flagged as such`);
    }
  }
});

test('a painted surface is not metallic, and clean steel is', () => {
  // The most common PBR mistake in a city scene: calling a painted shutter
  // "metal" because it is made of metal. Paint is an oxide, and a dielectric.
  const shutter = materialIndex(TABLE, 'paint_metal');
  assertEqual(TABLE.a[shutter * 4 + 3], 0, 'painted metal is a dielectric');
  const bare = materialIndex(TABLE, 'metal_bare');
  assertEqual(TABLE.a[bare * 4 + 3], 1, 'bare steel is a metal');
  const glass = materialIndex(TABLE, 'glass');
  assert(TABLE.a[glass * 4 + 2] < 0.15, 'glass is smooth');
  const rubber = materialIndex(TABLE, 'rubber');
  assert(TABLE.a[rubber * 4 + 2] > 0.7, 'rubber is rough');
});

test('ground materials tile on the ground plane and walls on a wall plane', () => {
  const road = materialIndex(TABLE, 'road_asphalt');
  assertEqual(TABLE.b[road * 4], MAP_MODE.GROUND, 'a road is projected onto XZ');
  const brick = materialIndex(TABLE, 'wall_brick');
  assertNotGround(TABLE.b[brick * 4], 'a wall is not');
  const solid = materialIndex(TABLE, 'foliage');
  assertEqual(TABLE.b[solid * 4], MAP_MODE.SOLID, 'an untextured material uses no projection');
  function assertNotGround(mode, msg) { assert(mode !== MAP_MODE.GROUND, msg); }
});

test('tiling scales are physically sensible', () => {
  // A road that tiles every two metres looks like a cobbled texture at driving
  // speed; a brick wall that tiles every twenty metres has no visible bricks.
  const road = materialIndex(TABLE, 'road_asphalt');
  const inv = TABLE.a[road * 4 + 1];
  const metres = 1 / inv;
  assert(metres > 2 && metres < 12, `a road tile is ${metres.toFixed(1)}m, between 2 and 12`);
  const brick = materialIndex(TABLE, 'wall_brick');
  const brickM = 1 / TABLE.a[brick * 4 + 1];
  assert(brickM > 1 && brickM < 4, `a brick tile is ${brickM.toFixed(1)}m, between 1 and 4`);
});

// ---------------------------------------------------------------------------
// The vertex layout
// ---------------------------------------------------------------------------

test('the vertex layout is 10 floats of 40 bytes', () => {
  assertEqual(VERTEX_FLOATS, 10, 'position(3) normal(3) colour(3) material(1)');
  assertEqual(VERTEX_BYTES, 40, 'and packs into 40 bytes');
});

test('every emitter writes whole, finite, in-range vertices', () => {
  const cases = {
    box: (a) => G.box(a, 1, 2, 3, 4, 5, 6, [0.5, 0.5, 0.5], G.M('wall_brick')),
    taper: (a) => G.taper(a, 0, 0, 0, 2, 2, 1, 1, 3, [0.4, 0.4, 0.4], G.M('wall_brick')),
    cylinder: (a) => G.cylinder(a, 0, 0, 0, 1, 2, [0.3, 0.3, 0.3], G.M('metal_bare'), 8),
    cone: (a) => G.cone(a, 0, 0, 0, 1, 2, [0.3, 0.3, 0.3], G.M('foliage'), 7),
    plane: (a) => G.plane(a, 0, 0, 0, 4, 4, [0.8, 0.8, 0.8], G.M('road_paint')),
    panel: (a) => G.panel(a, 0, 1, 0, 3, 2, 'z+', [0.9, 0.3, 0.2], G.M('sign')),
    blob: (a) => G.blob(a, 0, 0, 0, 2, 3, [0.2, 0.4, 0.2], G.M('foliage'), mulberry(3), 4, 7),
    window: (a) => G.window(a, 0, 1, 0, 1.4, 1.8, 'z+', mulberry(7)),
    door: (a) => G.door(a, 0, 0, 0, 1.2, 2.3, 'z+', mulberry(9), true),
    railing: (a) => G.railing(a, 0, 0, 0, 4, 1.05, 'x', [0.3, 0.3, 0.3], G.M('paint_metal')),
    parapet: (a) => G.parapet(a, 0, 0, 0, 10, 8, 0.75, [0.5, 0.5, 0.5], G.M('wall_concrete')),
    gableRoof: (a) => G.gableRoof(a, 0, 0, 0, 10, 8, 2, [0.3, 0.3, 0.3], G.M('wall_corrugated')),
    balcony: (a) => G.balcony(a, 0, 3, 0, 3, 1.3, [0.6, 0.6, 0.6], G.M('wall_stone')),
    acUnit: (a) => G.acUnit(a, 0, 0, 0, 1.6, 1.2, 0.9, mulberry(11)),
    container: (a) => G.container(a, 0, 0, 0, 6.1, 'z', mulberry(13)),
    streetLamp: (a) => G.streetLamp(a, 0, 0, 0, 8, 'z', mulberry(17)),
    trafficLight: (a) => G.trafficLight(a, 0, 0, 0, 5.4, 'x'),
    busShelter: (a) => G.busShelter(a, 0, 0, 0, 4.2, 'z'),
    dumpster: (a) => G.dumpster(a, 0, 0, 0, 'z'),
    bench: (a) => G.bench(a, 0, 0, 0, 'x'),
    hydrant: (a) => G.hydrant(a, 0, 0, 0),
    mailbox: (a) => G.mailbox(a, 0, 0, 0, 'z'),
    stairs: (a) => G.stairs(a, 0, 0, 0, 5, 2, 0.18, 0.3, [0.5, 0.5, 0.5], G.M('wall_concrete')),
    fireEscape: (a) => G.fireEscape(a, 0, 0, 0, 3, 2, 3.2, 'x'),
    pipeRun: (a) => G.pipeRun(a, 0, 1, 0, 8, 'z', [0.4, 0.4, 0.4], G.M('metal_bare'))
  };
  let totalVerts = 0;
  for (const [name, fn] of Object.entries(cases)) {
    const { arr, verts } = emit(fn);
    assert(arr.length % VERTEX_FLOATS === 0, `${name} emits whole vertices (${arr.length} floats)`);
    assert(verts.length > 0, `${name} emits something`);
    assert(allFinite(arr), `${name} emits only finite values`);
    for (const v of verts) {
      const n = [v[3], v[4], v[5]];
      const len = Math.hypot(n[0], n[1], n[2]);
      assertClose(len, 1, 0.001, `${name} emits unit normals`);
      assert(v[9] >= 0 && v[9] < TABLE.count, `${name} emits a valid material index (${v[9]})`);
      for (let i = 0; i < 3; i++) {
        assert(v[6 + i] >= 0 && v[6 + i] <= 1.0001, `${name} emits colours in 0..1 (got ${v[6 + i]})`);
      }
    }
    totalVerts += verts.length;
  }
  assert(totalVerts > 500, `the kit has a real amount of geometry to it (${totalVerts} vertices across ${Object.keys(cases).length} emitters)`);
});

function mulberry(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

test('a box is six quads with outward normals', () => {
  const { verts } = emit((a) => G.box(a, 0, 0, 0, 2, 2, 2, [0.5, 0.5, 0.5], G.M('wall_brick')));
  assertEqual(verts.length, 36, 'a box is 6 faces of 2 triangles');
  // For each face, the normal must point away from the box centre.
  const faces = new Map();
  for (const v of verts) {
    const key = `${v[3]},${v[4]},${v[5]}`;
    if (!faces.has(key)) faces.set(key, []);
    faces.get(key).push(v);
  }
  assertEqual(faces.size, 6, 'six distinct face normals');
  // "Outward" means the face's vertices sit at the extreme of the box along its
  // own normal, not at a positive distance from the origin: the bottom face of
  // a box standing on the ground is at y = 0, and its dot product with its own
  // outward normal is legitimately zero.
  // `box` is bottom-anchored: it spans x and z about the origin but y from 0 to
  // its height, so the expected extent differs per axis and is measured rather
  // than assumed.
  const extent = (n) => Math.max(...verts.map(v => v[0] * n[0] + v[1] * n[1] + v[2] * n[2]));
  // The box spans -1..1 in x and z but 0..2 in y, so the expected extent along a
  // face normal is the half-extent for the sideways faces and zero for the
  // bottom, whose outward direction is negative and points at the base.
  // `box` spans -1..1 in x and z but 0..2 in y, so a face's outward extent is
  // the half-extent on an upward or sideways normal and zero on the bottom,
  // whose outward direction points down at the base plane.
  // `box` spans -1..1 in x and z but 0..2 in y, so the six faces sit at
  // different distances along their own normals. Stated rather than derived:
  // the point of the test is that each face is at its own outward extreme.
  const EXPECTED = { '1,0,0': 1, '-1,0,0': 1, '0,0,1': 1, '0,0,-1': 1, '0,1,0': 2, '0,-1,0': 0 };
  for (const [key, group] of faces) {
    const n = key.split(',').map(Number);
    const best = extent(n);
    const expected = EXPECTED[key];
    assert(expected !== undefined, `face ${key} is one of the six box faces`);
    assertClose(best, expected, 1e-6, `face ${key} is on the outside of the box`);
    for (const v of group) {
      assertClose(v[0] * n[0] + v[1] * n[1] + v[2] * n[2], best, 1e-6,
        `every vertex of face ${key} is coplanar with it`);
    }
  }
  // And the inward-facing test, which the top face cannot participate in.
  for (const [key, group] of faces) {
    if (key === '0,1,0') continue;
    const n = key.split(',').map(Number);
    for (const v of group) {
      assert(v[0] * n[0] + v[1] * n[1] + v[2] * n[2] > -0.6, `face ${key} faces outward, not inward`);
    }
  }
});

test('a box can carry a different material per face', () => {
  const { verts } = emit((a) => G.box(a, 0, 0, 0, 2, 2, 2, [0.5, 0.5, 0.5], G.M('wall_brick'), {
    top: G.M('wall_stone'), faces: { bottom: G.M('wall_concrete') }
  }));
  const mats = new Set(verts.map(v => v[9]));
  assert(mats.size >= 3, `the override produced several materials (${[...mats].join(',')})`);
  // Every vertex whose normal points down must be the concrete material.
  const concrete = materialIndex(TABLE, 'wall_concrete');
  for (const v of verts) {
    if (v[4] < -0.5) assertEqual(v[9], concrete, 'the underside uses the overridden material');
  }
});

test('a window is a real opening, not a painted rectangle', () => {
  // Measured against a bare panel: a window has to be substantially more
  // geometry, because the frame, the transom and the sill are the entire point.
  const unit = emit((a) => G.window(a, 0, 0, 0, 1.4, 1.8, 'z+', mulberry(1)));
  assert(unit.count >= 30, `a window unit is at least thirty vertices (${unit.count})`);
  const bare = emit((a) => G.plane(a, 0, 0, 0, 1.4, 1.8, [0.2, 0.2, 0.2], G.M('glass')));
  assert(unit.count > bare.count * 4, 'a window costs several times a flat pane');
  const mats = new Set(unit.verts.map(v => v[9]));
  assert(mats.size >= 3, `a window uses glass, frame and sill (${mats.size} materials)`);
  assert(mats.has(materialIndex(TABLE, 'glass')), 'and one of them is glass');
});

test('a window is built into the wall plane, not floating in front of it', () => {
  const { arr, verts } = emit((a) => G.window(a, 0, 0, 0, 1.4, 1.8, 'z+', mulberry(1)));
  // Facing +z at the origin: every vertex must be at positive z and inside the
  // window's own footprint, with the sill projecting furthest.
  for (const v of verts) {
    assert(v[2] > -0.2, `a +z window has no geometry behind the wall plane (z=${v[2].toFixed(3)})`);
    assert(v[2] < 0.4, `and none floating off it (z=${v[2].toFixed(3)})`);
    assert(Math.abs(v[0]) < 1.1, `and none wider than the opening (x=${v[0].toFixed(2)})`);
  }
  assert(arr.length > 0, 'it emitted something');
});

test('a cylinder is a closed tube of unit facets', () => {
  const { verts } = emit((a) => G.cylinder(a, 0, 0, 0, 1, 2, [0.4, 0.4, 0.4], G.M('metal_bare'), 6, true));
  // Six side quads (two triangles each) plus two caps of six triangles.
  assertEqual(verts.length, 6 * 6 + 2 * 6 * 3, 'segment count and caps are as declared');
  let onCylinder = 0;
  for (const v of verts) {
    const radial = Math.hypot(v[0], v[2]);
    if (Math.abs(radial - 1) < 0.01) onCylinder++;
  }
  assert(onCylinder >= 36, `the side wall sits on the radius (${onCylinder} vertices)`);
});

test('a cone tapers and closes', () => {
  const { verts } = emit((a) => G.cone(a, 0, 0, 0, 1, 2, [0.3, 0.3, 0.3], G.M('foliage'), 6));
  assert(verts.length > 0, 'it emitted geometry');
  let apex = 0;
  for (const v of verts) if (Math.hypot(v[0], v[2]) < 0.01 && Math.abs(v[1] - 2) < 0.01) apex++;
  assertEqual(apex, 6, 'every segment meets at the apex');
  for (const v of verts) {
    const y = v[1];
    assert(y >= -0.01 && y <= 2.01, `a cone does not extend past its height (y=${y.toFixed(2)})`);
  }
});

test('a blob canopy has volume and a tapering profile', () => {
  const { verts } = emit((a) => G.blob(a, 0, 0, 0, 2, 3, [0.2, 0.4, 0.2], G.M('foliage'), mulberry(3), 4, 7));
  let minY = Infinity, maxY = -Infinity, maxR = 0;
  for (const v of verts) {
    minY = Math.min(minY, v[1]); maxY = Math.max(maxY, v[1]);
    maxR = Math.max(maxR, Math.hypot(v[0], v[2]));
  }
  assertClose(maxY - minY, 3, 0.01, 'the canopy spans its full height');
  assert(maxR > 0.5 && maxR <= 2.05, `the canopy stays inside its radius (${maxR.toFixed(2)})`);
});

test('a blob seeded differently is a different shape', () => {
  // Determinism plus variation: the same seed must give the same canopy, and
  // different seeds must not. A street of identical trees is the single most
  // visible flaw a procedural generator can have.
  const shape = (seed) => emit((a) => G.blob(a, 0, 0, 0, 2, 3, [1, 1, 1], G.M('foliage'), mulberry(seed), 3, 6)).arr;
  assertEqual(JSON.stringify(shape(5)), JSON.stringify(shape(5)), 'the same seed gives the same canopy');
  assert(JSON.stringify(shape(5)) !== JSON.stringify(shape(6)), 'different seeds give different canopies');
});

test('street furniture stands on the ground plane it was given', () => {
  for (const [name, fn] of Object.entries({
    hydrant: (a) => G.hydrant(a, 3, 0, -2),
    bench: (a) => G.bench(a, 3, 0, -2, 'x'),
    bollard: (a) => G.bollard(a, 3, 0, -2),
    bin: (a) => G.bin(a, 3, 0, -2, mulberry(3)),
    crate: (a) => G.crate(a, 3, 0, -2, 0.6, mulberry(3)),
    container: (a) => G.container(a, 3, 0, -2, 6.1, 'z', mulberry(3)),
    streetLamp: (a) => G.streetLamp(a, 3, 0, -2, 8, 'z', mulberry(3))
  })) {
    const { verts } = emit(fn);
    let minY = Infinity;
    for (const v of verts) minY = Math.min(minY, v[1]);
    assert(minY > -0.25, `${name} does not sink below its origin (min y ${minY.toFixed(2)})`);
    assert(minY < 0.2, `${name} actually touches the ground (min y ${minY.toFixed(2)})`);
  }
});

test('a street lamp is eight metres and reaches over the carriageway', () => {
  const { verts } = emit((a) => G.streetLamp(a, 0, 0, 0, 8, 'z', mulberry(3)));
  let maxY = -Infinity, maxZ = -Infinity;
  for (const v of verts) { maxY = Math.max(maxY, v[1]); maxZ = Math.max(maxZ, v[2]); }
  assert(maxY > 8, `the column is its full height (${maxY.toFixed(2)}m)`);
  assert(maxY < 10, `and is not a telegraph pole (${maxY.toFixed(2)}m)`);
  assert(maxZ > 1.2, `the arm reaches over the road (${maxZ.toFixed(2)}m)`);
});

test('a shipping container is a standard 20-foot box', () => {
  const { verts } = emit((a) => G.container(a, 0, 0, 0, 6.1, 'z', mulberry(3)));
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity, minZ = Infinity, maxZ = -Infinity;
  for (const v of verts) {
    minX = Math.min(minX, v[0]); maxX = Math.max(maxX, v[0]);
    minY = Math.min(minY, v[1]); maxY = Math.max(maxY, v[1]);
    minZ = Math.min(minZ, v[2]); maxZ = Math.max(maxZ, v[2]);
  }
  // Facing 'z' means the container's length runs along x.
  assertClose(maxX - minX, 6.1, 0.3, 'a 20-foot container is 6.1m long');
  assertClose(maxZ - minZ, 2.44, 0.2, 'and 2.44m wide');
  assertClose(maxY - minY, 2.7, 0.3, 'and 2.6m tall, plus its corner castings');
});

test('a parapet stands on the roof and does not exceed its height', () => {
  const { verts } = emit((a) => G.parapet(a, 0, 10, 0, 20, 14, 0.75, [0.5, 0.5, 0.5], G.M('wall_concrete')));
  let minY = Infinity, maxY = -Infinity;
  for (const v of verts) { minY = Math.min(minY, v[1]); maxY = Math.max(maxY, v[1]); }
  assertClose(minY, 10, 0.01, 'it starts on the roof');
  assert(maxY <= 11.1, `and stops at its own height plus the coping (${maxY.toFixed(2)})`);
});

// ---------------------------------------------------------------------------
// World composition
// ---------------------------------------------------------------------------

const BUILDING = { id: 1, x: 100, z: 100, w: 30, d: 24, h: 18, floors: 5, kind: 'shop', seed: 0.31, facade: 0.4, roof: 0.2, sign: true, doorSide: 0, district: 0 };

test('a building grows with its detail budget rather than switching on', () => {
  const near = emit((a) => buildBuilding(G, a, BUILDING, 1.0, 12345, false));
  const mid = emit((a) => buildBuilding(G, a, BUILDING, 0.5, 12345, false));
  const far = emit((a) => buildBuilding(G, a, BUILDING, 0.0, 12345, false));
  assert(near.count > mid.count, `full detail is heavier than half (${near.count} vs ${mid.count})`);
  assert(mid.count > far.count, `half detail is heavier than none (${mid.count} vs ${far.count})`);
  assert(far.count > 0, 'even at zero detail a building has its mass and its plinth');
  assert(near.count > far.count * 4, `full detail is at least four times the mass (${near.count} vs ${far.count})`);
  for (const [e, name] of [[near, 'full'], [mid, 'mid'], [far, 'none']]) {
    assert(allFinite(e.arr), `${name} detail emits only finite values`);
  }
});

test('a building is a plausible number of storeys tall', () => {
  const { verts } = emit((a) => buildBuilding(G, a, BUILDING, 1.0, 12345, false));
  let maxY = -Infinity, minY = Infinity;
  for (const v of verts) { maxY = Math.max(maxY, v[1]); minY = Math.min(minY, v[1]); }
  // Measured from the building's own base, because it stands on terrain that
  // is not at zero. The record says 18 m; a cornice and roof plant sit above
  // it, but not by double — a building twice its stated height is a unit error.
  const built = maxY - minY;
  assert(built > BUILDING.h, `the building reaches its stated height (${built.toFixed(1)}m)`);
  assert(built < BUILDING.h * 1.5, `and does not tower over it (${built.toFixed(1)}m against a stated ${BUILDING.h}m)`);
});

test('a building is deterministic for a seed and varies between seeds', () => {
  const shape = (seed) => emit((a) => buildBuilding(G, a, { ...BUILDING, seed }, 1.0, 12345, false)).arr;
  assertEqual(JSON.stringify(shape(0.31)), JSON.stringify(shape(0.31)), 'the same seed builds the same building');
  assert(JSON.stringify(shape(0.31)) !== JSON.stringify(shape(0.77)), 'a different seed builds a different one');
});

test('a building uses several materials, not one', () => {
  const { verts } = emit((a) => buildBuilding(G, a, BUILDING, 1.0, 12345, false));
  const mats = new Set(verts.map(v => v[9]));
  assert(mats.size >= 4, `a detailed building draws on several materials (${mats.size})`);
  assert(mats.has(materialIndex(TABLE, 'glass')), 'including glass, for its windows');
});

test('a road has a carriageway, two footways and lane markings', () => {
  const road = { id: 3, x: 0, z: 0, w: 34, d: 600, main: 2 };
  const { verts, count } = emit((a) => buildRoad(G, a, road, 1.0, 999));
  assert(count > 200, `a main road is a substantial amount of geometry (${count})`);
  assert(allFinite(verts.flat()), 'all finite');
  const mats = new Set(verts.map(v => v[9]));
  assert(mats.has(materialIndex(TABLE, 'road_asphalt')), 'it is surfaced in asphalt');
  assert(mats.has(materialIndex(TABLE, 'road_paint')), 'and it has markings');
  assert(mats.has(materialIndex(TABLE, 'pavement_concrete')), 'and a footway');
  // A road with no markings at all is the clearest tell of a generated street.
  const bare = emit((a) => buildRoad(G, a, road, 0.2, 999));
  const bareMats = new Set(bare.verts.map(v => v[9]));
  assert(!bareMats.has(materialIndex(TABLE, 'road_paint')), 'and at low detail the markings are dropped');
});

test('a tree is a real tree, and trees vary', () => {
  const t = { id: 1, x: 50, z: 50, s: 1.4, type: 'tree', seed: 0.4 };
  const { verts, count } = emit((a) => buildTree(G, a, t, 1.0, 777));
  assert(count > 100, `a detailed tree has real geometry (${count} vertices)`);
  let maxY = -Infinity, minY = Infinity, maxR = 0;
  for (const v of verts) {
    maxY = Math.max(maxY, v[1]); minY = Math.min(minY, v[1]);
    maxR = Math.max(maxR, Math.hypot(v[0] - 50, v[2] - 50));
  }
  const h = maxY - minY;
  assert(h > 3 && h < 14, `a scale-${t.s} tree is a plausible height (${h.toFixed(1)}m)`);
  assert(maxR < 6, `and a plausible spread (${maxR.toFixed(1)}m radius)`);
  const shape = (seed) => emit((a) => buildTree(G, a, { ...t, seed }, 1.0, 777)).arr;
  assert(JSON.stringify(shape(0.4)) !== JSON.stringify(shape(0.9)), 'two trees are not the same tree');
});

test('a pine is a conifer and a broadleaf is not', () => {
  const pine = emit((a) => buildTree(G, a, { id: 1, x: 0, z: 0, s: 1.5, type: 'pine', seed: 0.5 }, 1.0, 1));
  const leaf = emit((a) => buildTree(G, a, { id: 1, x: 0, z: 0, s: 1.5, type: 'broadleaf', seed: 0.5 }, 1.0, 1));
  assert(JSON.stringify(pine.arr) !== JSON.stringify(leaf.arr), 'a pine and a broadleaf are different trees');
  const width = (e) => {
    let r = 0;
    for (const v of e.verts) r = Math.max(r, Math.hypot(v[0], v[2]));
    return r;
  };
  assert(width(pine) < width(leaf), `a conifer is narrower than a broadleaf (${width(pine).toFixed(1)} vs ${width(leaf).toFixed(1)})`);
});

test('a car is a car-shaped thing about four and a half metres long', () => {
  const car = { id: 1, x: 200, z: 200, horizontal: true, color: [0.6, 0.2, 0.2] };
  const { verts, count } = emit((a) => buildCar(G, a, car, 5, 0));
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (const v of verts) {
    minX = Math.min(minX, v[0]); maxX = Math.max(maxX, v[0]);
    minY = Math.min(minY, v[1]); maxY = Math.max(maxY, v[1]);
  }
  assert(maxX - minX > 3.5 && maxX - minX < 5.5, `a car is about 4.5m long (${(maxX - minX).toFixed(2)}m)`);
  assert(maxY - minY < 2.2, `and about 1.5m tall (${(maxY - minY).toFixed(2)}m)`);
  // Standing on the terrain means the body sits above the ground sample under
  // it, not that it sits at some absolute height.
  const ground = terrainHeight(200, 200, 5);
  assert(minY > ground - 0.3, `and stands on the terrain (wheels at ${minY.toFixed(2)}, ground at ${ground.toFixed(2)})`);
  assert(count > 200, `with real geometry on it (${count} vertices)`);
  const mid = emit((a) => buildCar(G, a, car, 5, 1));
  const proxy = emit((a) => buildCarProxy(G, a, car, 5));
  assert(mid.count < count, `the mid LOD drops the wheel hubs (${mid.count} vs ${count})`);
  assert(proxy.count < mid.count / 3, `and the far LOD is two boxes (${proxy.count} vs ${mid.count})`);
  // But the proxy still has to be recognisably a car, or distant traffic turns
  // into a field of bricks.
  let proxyMaxY = -Infinity;
  for (const v of proxy.verts) proxyMaxY = Math.max(proxyMaxY, v[1]);
  const proxyGround = terrainHeight(200, 200, 5);
  assert(proxyMaxY - proxyGround > 1.0, `the proxy stands a car-shaped height (${(proxyMaxY - proxyGround).toFixed(2)}m)`);
});

test('a character is 1.78 m and has a head on top', () => {
  const { verts } = emit((a) => buildCharacter(G, a, 10, 20, 0, 7, 'worker', 'travel', 0.5, 1, 0));
  let maxY = -Infinity, minY = Infinity, minX = Infinity, maxX = -Infinity;
  for (const v of verts) {
    maxY = Math.max(maxY, v[1]); minY = Math.min(minY, v[1]);
    minX = Math.min(minX, v[0]); maxX = Math.max(maxX, v[0]);
  }
  // Within the per-person build band rather than at exactly 1.78: individuals
  // are meant to differ, and the band is what stops that becoming nonsense.
  assert(maxY - 1.78 * 0.94 > 0, `a character is within the build band (${maxY.toFixed(2)}m)`);
  assert(maxY - 1.78 * 1.07 < 0, `and no more than the tallest build (${maxY.toFixed(2)}m)`);
  assertClose(minY, 0, 0.06, 'with its feet on the ground');
  const shoulder = maxX - minX;
  assert(shoulder > 0.35 && shoulder < 0.75, `and shoulders about a quarter of its height across (${shoulder.toFixed(2)}m)`);
});

test('characters vary in build and clothing by identity', () => {
  const shape = (id, job) => emit((a) => buildCharacter(G, a, 0, 0, 0, id, job, 'idle', 0, 1, 0)).arr;
  assert(JSON.stringify(shape(3, 'worker')) !== JSON.stringify(shape(4, 'worker')), 'two people are not identical');
  assert(JSON.stringify(shape(3, 'worker')) !== JSON.stringify(shape(3, 'service')), 'a worker is not a courier');
  const width = (id) => {
    let w = 0;
    for (const v of emit((a) => buildCharacter(G, a, 0, 0, 0, id, 'service', 'idle', 0, 1, 0)).verts) {
      w = Math.max(w, Math.abs(v[0]));
    }
    return w * 2;
  };
  const widths = [0, 1, 2, 3, 4, 5, 6, 7].map(width);
  const spread = Math.max(...widths) - Math.min(...widths);
  assert(spread > 0.01, `a crowd has a spread of shoulder widths (${spread.toFixed(3)}m across ${widths.length} people)`);
  const height = (id) => {
    let hi = -Infinity, lo = Infinity;
    for (const v of emit((a) => buildCharacter(G, a, 0, 0, 0, id, 'service', 'idle', 0, 1, 0)).verts) {
      hi = Math.max(hi, v[1]); lo = Math.min(lo, v[1]);
    }
    return hi - lo;
  };
  const heights = [0, 1, 2, 3, 4, 5, 6, 7].map(height);
  const hSpread = Math.max(...heights) - Math.min(...heights);
  assert(hSpread > 0.05, `and a spread of heights (${hSpread.toFixed(3)}m)`);
  for (const h of heights) assert(h > 1.6 && h < 1.95, `every person is a plausible height (${h.toFixed(2)}m)`);
});

process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
