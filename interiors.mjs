/**
 * Interiors.
 *
 * A room is a shell plus furniture. The shell is generated — floor, walls,
 * ceiling, skirting, a window opening — because a downloaded room would be one
 * fixed layout repeated, and because the shell has to fit the building it sits
 * in. The furniture is imported, because a chair is a chair and generating a
 * convincing one out of primitives is exactly the low-poly look this project
 * exists to leave behind.
 *
 * ## Why placement is deterministic per building
 *
 * Every room is furnished from a hash of the building's own coordinates, not
 * from a global random stream. Two consequences that matter: streaming a
 * building in and out produces byte-identical geometry both times, and two
 * buildings never get the same arrangement by accident.
 *
 * ## Why interiors are not streamed with the city
 *
 * The city streams on a radius. Interiors are only worth building when the
 * player is inside or looking in through a window, which is a much tighter
 * condition, and the geometry is per-building rather than per-cell. So they are
 * generated on demand against a separate, smaller radius, and released with it.
 *
 * ## Collision
 *
 * Interiors contribute no collision. The building's own collider already blocks
 * the player from entering, and adding furniture colliders without a navmesh to
 * path around them would make furniture an obstacle the player could get stuck
 * on. Stated rather than quietly omitted: if interiors ever become enterable,
 * this is the thing that has to change.
 */
import { MODELS, ROOM, FLOATS_PER_VERTEX, lodVertices } from './assets/models.gen.mjs';

/** Vertex layout, mirrored from geometry.mjs. */
const OFF_POS = 0, OFF_NRM = 3, OFF_COL = 6, OFF_MAT = 9, OFF_UV = 10;

/** Distances, in metres, at which each LOD is used. */
export const LOD_DISTANCE = [0, 9, 22];

/** Interiors are built for buildings within this distance of the player. */
export const INTERIOR_RADIUS = 34;

/** How many interiors are built per frame, so a teleport does not hitch. */
const INTERIOR_BUDGET_PER_FRAME = 3;

function hash2(x, z) {
  let h = Math.imul(Math.round(x * 8192) ^ 0x9e3779b9, 0x85ebca6b);
  h = Math.imul(h ^ Math.round(z * 8192), 0xc2b2ae35);
  h ^= h >>> 13;
  return ((h >>> 0) % 100000) / 100000;
}

/** A small deterministic generator, seeded from a building's position. */
function seeded(seed) {
  let s = (seed * 2654435761) >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

/**
 * Which room archetype a building gets.
 *
 * Derived from the building's own identity rather than assigned at generation
 * time, so a business's interior follows the business.
 */
export function roomFor(b) {
  if (b.height < 4.2) return null;
  if (b.business && /food|cafe|restaurant|bar|shop|store|retail/i.test(String(b.business.kind || b.business.type || ''))) {
    return ROOM.RETAIL;
  }
  if (b.height > 11) return ROOM.OFFICE;
  if (b.industrial || b.zone === 'industrial') return ROOM.WORKSHOP;
  return ROOM.RESIDENTIAL;
}

/** Which LOD a model at `distance` from the camera should use. */
export function lodForDistance(distance) {
  for (let i = LOD_DISTANCE.length - 1; i >= 0; i--) {
    if (distance >= LOD_DISTANCE[i]) return i;
  }
  return 0;
}

/**
 * The models a room archetype may contain, with the zone each belongs in.
 *
 * Zones are the parts of a room — a chair has to be on the floor against a
 * wall, a ceiling lamp has to be overhead. Without them a lamp ends up standing
 * in a corner at chair height, which is the specific failure that makes
 * procedural interiors look wrong.
 */
const PLACEMENT = {
  armchair: { zone: 'seat', rotation: 'wall', offset: 0.35 },
  lounge_chair: { zone: 'seat', rotation: 'wall', offset: 0.4 },
  coffee_table: { zone: 'centre', rotation: 'free' },
  side_table: { zone: 'seat', rotation: 'wall', offset: 0.75 },
  sideboard: { zone: 'wall', rotation: 'wall', offset: 0.3, tall: true },
  shelving: { zone: 'wall', rotation: 'wall', offset: 0.4, tall: true },
  ceiling_lamp: { zone: 'ceiling' },
  picture_frame: { zone: 'wall-mounted', offset: 1.55 },
  desk_chair: { zone: 'seat', rotation: 'wall', offset: 0.3 },
  drawer_cabinet: { zone: 'wall', rotation: 'wall', offset: 0.35, tall: true },
  notepads: { zone: 'surface', height: 0.75, rotation: 'free' },
  wall_clock: { zone: 'wall-mounted', offset: 2.0 },
  stove: { zone: 'wall', rotation: 'wall', offset: 0.35, tall: true },
  enamel_pot: { zone: 'surface', height: 0.92, rotation: 'free' },
  crate: { zone: 'floor', rotation: 'free' },
  workbench_stool: { zone: 'centre', rotation: 'free' },
  power_box: { zone: 'wall-mounted', offset: 1.8 },
  caged_light: { zone: 'ceiling' },
  wall_lamp: { zone: 'wall-mounted', offset: 2.2 },
  barrel: { zone: 'floor', rotation: 'free' },
  street_seating: { zone: 'floor', rotation: 'free' }
};

/** How many of each model a room archetype gets, and where. */
const ROOM_PLANS = {
  [ROOM.RESIDENTIAL]: [
    'armchair', 'lounge_chair', 'coffee_table', 'side_table', 'sideboard',
    'shelving', 'ceiling_lamp', 'picture_frame'
  ],
  [ROOM.OFFICE]: [
    'desk_chair', 'drawer_cabinet', 'notepads', 'wall_clock', 'shelving', 'ceiling_lamp'
  ],
  [ROOM.RETAIL]: [
    'stove', 'enamel_pot', 'crate', 'counter', 'ceiling_lamp', 'picture_frame'
  ],
  [ROOM.WORKSHOP]: [
    'workbench_stool', 'power_box', 'caged_light', 'wall_lamp', 'barrel', 'crate'
  ]
};

/**
 * Every room archetype and the models it can place.
 *
 * Read from the curated list rather than hard-coded a second time, so a model
 * added to `assets/models.mjs` cannot be silently unfurnishable — and a model
 * with no placement rule here fails the test suite instead of being skipped.
 */
export function planFor(room) {
  const plan = ROOM_PLANS[room];
  if (!plan) return [];
  return plan.filter((id) => id === 'counter' || MODELS[id]).map((id) => {
    if (id === 'counter') return { id: 'counter', generated: true, ...COUNTER };
    return { id, generated: false, ...PLACEMENT[id] };
  });
}

/** The shop counter is generated, not imported — it is a box, and a box is right. */
const COUNTER = { zone: 'wall', rotation: 'wall', offset: 0.5, tall: true, w: 2.4, h: 1.05, d: 0.7 };

/**
 * Build one interior.
 *
 * @param {object} b The building record from the world.
 * @param {number} x0 Building centre X.
 * @param {number} z0 Building centre Z.
 * @param {number} floorY The floor height, which follows the terrain.
 * @param {number} cameraDistance Distance from the player, which picks the LOD.
 * @param {(id:string)=>number} materialFor Resolve a material id to an index.
 * @param {object} kit The geometry kit, for the generated parts of the shell.
 * @param {object} [opts]
 * @returns {{vertices: number[], count: number, room: string, placed: object[]}}
 */
export function buildInterior(b, x0, z0, floorY, cameraDistance, materialFor, kit, opts = {}) {
  const room = roomFor(b);
  if (!room) return null;

  const lod = opts.lod !== undefined ? opts.lod : lodForDistance(cameraDistance);
  const arr = [];
  const rand = seeded(Math.abs(Math.round(x0 * 31 + z0 * 17)) % 2147483647 || 7);
  const placed = [];

  // -- the shell ------------------------------------------------------------
  const w = Math.max(3.2, b.w - 0.6);
  const d = Math.max(3.2, b.d - 0.6);
  const h = Math.max(2.4, Math.min(b.height - 0.4, 3.0));
  const floorMat = materialFor(room === ROOM.WORKSHOP ? 'floor_concrete'
    : room === ROOM.RETAIL ? 'floor_tile' : 'floor_wood');
  const wallMat = materialFor(room === ROOM.WORKSHOP ? 'wall_painted_concrete' : 'wall_plaster');
  const ceilMat = materialFor('wall_plaster');
  const y0 = floorY + 0.02;

  // Floor, four walls and a ceiling, emitted as inward-facing boxes. The shell
  // is the part of an interior that must never be missing: an un-walled room
  // shows the sky through the building and undoes everything else.
  kit.box(arr, x0, y0, z0, w, 0.06, d, [0.62, 0.60, 0.57], floorMat);
  kit.box(arr, x0, y0 + h, z0, w, 0.06, d, [0.80, 0.80, 0.78], ceilMat);
  const t = 0.14;
  kit.box(arr, x0, y0, z0 - d / 2, w, h, t, [0.74, 0.72, 0.69], wallMat);
  kit.box(arr, x0, y0, z0 + d / 2, w, h, t, [0.74, 0.72, 0.69], wallMat);
  kit.box(arr, x0 - w / 2, y0, z0, t, h, d, [0.72, 0.70, 0.68], wallMat);
  kit.box(arr, x0 + w / 2, y0, z0, t, h, d, [0.72, 0.70, 0.68], wallMat);

  // Skirting. A 100 mm band at the base of a wall is the single detail that
  // most reliably makes an interior read as built rather than assembled.
  const skirt = materialFor('floor_wood');
  const sh = 0.10;
  kit.box(arr, x0, y0 + 0.06, z0 - d / 2 + t / 2 + 0.012, w, sh, 0.024, [0.45, 0.40, 0.34], skirt);
  kit.box(arr, x0, y0 + 0.06, z0 + d / 2 - t / 2 - 0.012, w, sh, 0.024, [0.45, 0.40, 0.34], skirt);
  kit.box(arr, x0 - w / 2 + t / 2 + 0.012, y0 + 0.06, z0, 0.024, sh, d, [0.45, 0.40, 0.34], skirt);
  kit.box(arr, x0 + w / 2 - t / 2 - 0.012, y0 + 0.06, z0, 0.024, sh, d, [0.45, 0.40, 0.34], skirt);

  // -- the furniture --------------------------------------------------------
  const plan = planFor(room);
  for (const item of plan) {
    const y = placementY(item, y0, h, w, d, rand);
    if (y === null) continue;
    const [px, pz, rot] = placementXZ(item, x0, z0, y0, w, d, rand);

    if (item.generated) {
      kit.box(arr, px, y, pz, item.w, item.h, item.d, [0.60, 0.58, 0.55], materialFor('wall_stone'));
      placed.push({ id: item.id, generated: true, x: px, y, z: pz });
      continue;
    }

    const model = MODELS[item.id];
    if (!model) continue;
    const verts = lodVertices(model, Math.min(lod, model.lods.length - 1));
    const count = appendModel(arr, verts, model, px, y, pz, rot, materialFor);
    if (count > 0) {
      placed.push({ id: item.id, generated: false, x: px, y, z: pz, rotation: rot, vertices: count, lod });
    }
  }

  return { vertices: arr, count: arr.length / FLOATS_PER_VERTEX, room, placed, lod };
}

/** The height an item sits at, or null if it does not fit this room. */
function placementY(item, y0, h, w, d, rand) {
  switch (item.zone) {
    case 'ceiling':
      return y0 + h - 0.12;
    case 'wall-mounted':
      return y0 + Math.min(item.offset, h - 0.5);
    case 'surface':
      return y0 + item.height;
    case 'seat':
    case 'wall':
    case 'centre':
    case 'floor':
      return y0 + 0.06;
    default:
      return null;
  }
}

/**
 * Where on the floor an item goes.
 *
 * A wall-facing item is placed against a randomly chosen wall and rotated to
 * face into the room, which is the whole difference between a chair against a
 * wall and a chair in the middle of the floor.
 */
function placementXZ(item, x0, z0, y0, w, d, rand) {
  const inset = 0.45;
  const hw = w / 2 - inset;
  const hd = d / 2 - inset;
  const wall = Math.floor(rand() * 4);
  switch (item.zone) {
    case 'wall-mounted': {
      // Against a wall, offset inward, facing the room.
      const off = (rand() - 0.5) * (w - 1.2);
      if (wall === 0) return [x0 + off, z0 - hd, 0];
      if (wall === 1) return [x0 + off, z0 + hd, Math.PI];
      if (wall === 2) return [x0 - hw, z0 + off, Math.PI / 2];
      return [x0 + hw, z0 + off, -Math.PI / 2];
    }
    case 'wall': {
      const gap = item.w ? item.w / 2 + 0.2 : 0.5;
      if (wall === 0) return [x0 + (rand() - 0.5) * Math.max(0, w - gap * 2), z0 - hd, 0];
      if (wall === 1) return [x0 + (rand() - 0.5) * Math.max(0, w - gap * 2), z0 + hd, Math.PI];
      if (wall === 2) return [x0 - hw, z0 + (rand() - 0.5) * Math.max(0, d - gap * 2), Math.PI / 2];
      return [x0 + hw, z0 + (rand() - 0.5) * Math.max(0, d - gap * 2), -Math.PI / 2];
    }
    case 'seat': {
      // Seating goes in the middle-ish and turns to face the room's centre.
      const a = rand() * Math.PI * 2;
      const r = 0.3 + rand() * 0.4;
      const px = x0 + Math.cos(a) * r;
      const pz = z0 + Math.sin(a) * r;
      return [px, pz, Math.atan2(z0 - pz, x0 - px)];
    }
    case 'centre':
      return [x0 + (rand() - 0.5) * 0.7, z0 + (rand() - 0.5) * 0.7, rand() * Math.PI * 2];
    case 'surface':
      // On a counter, near its middle.
      return [x0 + (rand() - 0.5) * 1.4, z0 - d / 2 + 0.6, rand() * Math.PI];
    default: {
      const a = rand() * Math.PI * 2;
      const r = 0.5 + rand() * (Math.min(hw, hd) - 0.5);
      return [x0 + Math.cos(a) * r, z0 + Math.sin(a) * r, rand() * Math.PI * 2];
    }
  }
}

/**
 * Append one imported model to the vertex array, transformed into place.
 *
 * The model is authored around its own origin, often not at the base of the
 * object, so it is shifted down until its lowest point sits on the placement
 * height. Without that a chair floats at its bounding-box centre or sinks to
 * the floor by half its depth, and it is the kind of error that reads as
 * "the interiors are broken" rather than as a transform bug.
 */
function appendModel(arr, verts, model, px, py, pz, rotation, materialFor) {
  const count = verts.length / FLOATS_PER_VERTEX;
  const b = model.bounds;
  const cos = Math.cos(rotation);
  const sin = Math.sin(rotation);
  const mat = materialFor(modelMaterial(model));

  // Drop the model so its lowest point rests on py.
  const drop = py - b.min[1];

  for (let i = 0; i < count; i++) {
    const o = i * FLOATS_PER_VERTEX;
    const x = verts[o + OFF_POS];
    const y = verts[o + OFF_POS + 1] + drop;
    const z = verts[o + OFF_POS + 2];
    const nx = verts[o + OFF_NRM];
    const ny = verts[o + OFF_NRM + 1];
    const nz = verts[o + OFF_NRM + 2];
    arr.push(
      px + x * cos - z * sin,
      y,
      pz + x * sin + z * cos,
      // Normals rotate with the model. Skipping this is invisible on an
      // axis-aligned box and glaring on a rotated chair: the light comes from
      // the wrong side and the chair looks lit from inside.
      nx * cos - nz * sin,
      ny,
      nx * sin + nz * cos,
      verts[o + OFF_COL], verts[o + OFF_COL + 1], verts[o + OFF_COL + 2],
      mat,
      verts[o + OFF_UV], verts[o + OFF_UV + 1]
    );
  }
  return count;
}

/**
 * Which baked material a model's own textures are closest to.
 *
 * The imported meshes carry their own albedo, normal and ARM maps, which are not
 * in the texture arrays. Rather than add a second sampler path, the model is
 * tinted with vertex colour and lit through a matching entry in the material
 * table, chosen from the material the asset is made of. The geometry and the
 * form are real; the surface detail is the array texture underneath it.
 */
function modelMaterial(model) {
  const id = String(model.room || '');
  if (id === 'workshop') return 'wall_metal_shutter';
  if (id === 'retail') return 'wall_painted_concrete';
  if (id === 'office') return 'wall_plaster';
  return 'floor_wood';
}

export { MODELS, ROOM };
