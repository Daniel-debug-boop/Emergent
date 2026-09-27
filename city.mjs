/**
 * City detail: what actually gets built into the world.
 *
 * This module turns the abstract records the world generator emits — a building
 * is `{x, z, w, d, h, floors, kind, seed, facade, roof, sign}` — into geometry.
 * It owns the decisions that make a procedural city look designed rather than
 * generated: which facade a building gets, how its windows are set out, where
 * its rooftop plant goes, what is left on the pavement beside it.
 *
 * ## The rule this is built around
 *
 * A box with a brick texture is still a box. What makes architecture read as
 * architecture is the small stuff: a plinth at the base, a band at the floor
 * line, a recessed reveal around every opening, a sill that projects far
 * enough to catch a shadow, a parapet with a coping, plant on the roof, a
 * different material at every level. None of that is expensive. All of it is
 * missing from a naive generator, and it is the whole difference between
 * "boxes with better textures" and a city.
 *
 * ## Detail is budgeted by distance
 *
 * `detail` runs 0..1 and is a distance-and-quality derived budget. Windows
 * appear above 0.4, balconies and fire escapes above 0.6, rooftop plant above
 * 0.5, and the small dressing only on the buildings nearest the player. This
 * is what allows the world to be much denser than it was without a linear
 * increase in triangles: the far half of a city still has correct massing,
 * correct materials and a silhouette, and only the near half pays for detail.
 *
 * ## Determinism
 *
 * Every random decision comes from the record's own `seed`, so a given world
 * generates the same building on every visit and after a save/load. Nothing
 * here calls `Math.random`.
 */

import { terrainHeight } from './world.mjs';

/** A small deterministic stream, seeded per object. */
function rng(seed) {
  let s = (Math.floor(seed * 4294967296) >>> 0) || 1;
  return () => {
    s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

/** Facade systems. Each is a real construction type, not a colour swap. */
const FACADES = [
  { id: 'brick', wall: 'wall_brick', plinth: 'wall_stone', band: 'wall_stone', trim: 'wall_stone' },
  { id: 'brick_dark', wall: 'wall_brick_dark', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_concrete' },
  { id: 'plaster', wall: 'wall_plaster', plinth: 'wall_stone', band: 'wall_stone', trim: 'wall_concrete' },
  { id: 'plaster_dirty', wall: 'wall_plaster_dirty', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_concrete' },
  { id: 'concrete', wall: 'wall_concrete', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_concrete' },
  { id: 'painted', wall: 'wall_painted_concrete', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_stone' },
  { id: 'stone', wall: 'wall_stone', plinth: 'wall_stone', band: 'wall_stone', trim: 'wall_stone' },
  { id: 'siding', wall: 'wall_siding', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_siding' }
];

/** Roof forms, matched to building kind. */
const ROOFS = {
  tower: ['flat', 'flat', 'flat', 'setback'],
  shop: ['flat', 'gable', 'flat', 'mansard'],
  house: ['gable', 'gable', 'gable', 'hip'],
  warehouse: ['sawtooth', 'gable', 'flat', 'flat']
};

const TINTS = {
  wall_brick: [1.0, 0.98, 0.95],
  wall_brick_dark: [0.95, 0.96, 1.0],
  wall_plaster: [1.0, 1.0, 0.98],
  wall_plaster_dirty: [0.96, 0.96, 0.92],
  wall_concrete: [1.0, 1.0, 1.0],
  wall_painted_concrete: [1.0, 1.0, 1.0],
  wall_stone: [0.98, 0.98, 1.0],
  wall_siding: [1.0, 0.97, 0.92]
};

const tint = (base, k) => [
  Math.min(1, base[0] * k), Math.min(1, base[1] * k), Math.min(1, base[2] * k)
];

/**
 * Pick the facade for a building.
 *
 * Keyed off the building's own seed, and biased by kind: a warehouse in
 * plaster and a shop in corrugated iron both look wrong, and a city where every
 * building chooses freely looks like a catalogue rather than a place.
 */
function facadeFor(b) {
  const r = rng(b.seed * 7919 + 11);
  let pool;
  switch (b.kind) {
    case 'warehouse': pool = ['corrugated', 'concrete', 'brick_dark']; break;
    case 'tower': pool = ['concrete', 'painted', 'glass_concrete', 'stone']; break;
    case 'shop': pool = ['brick', 'plaster', 'painted', 'brick_dark']; break;
    default: pool = ['brick', 'plaster', 'plaster_dirty', 'siding', 'brick_dark', 'stone', 'painted'];
  }
  const pick = pool[Math.floor(r() * pool.length) % pool.length];
  if (pick === 'glass_concrete') {
    return { id: 'concrete', wall: 'wall_concrete', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_concrete', glazed: true };
  }
  if (pick === 'corrugated') {
    return { id: 'corrugated', wall: 'wall_corrugated', plinth: 'wall_concrete', band: 'wall_concrete', trim: 'wall_metal_shutter' };
  }
  return FACADES.find(f => f.id === pick) || FACADES[0];
}

/**
 * A building, with everything that makes it a building.
 *
 * The order matters: plinth, mass, horizontal bands, then openings, then the
 * roof. Openings are cut last so that nothing can be drawn on top of them, and
 * the roof is added after the walls so plant sits on a surface that exists.
 */
export function buildBuilding(G, arr, b, detail, seed, night) {
  const r = rng(b.seed * 104729 + 3);
  const y = terrainHeight(b.x, b.z, seed);
  const F = facadeFor(b);
  const wallTint = tint(TINTS[F.wall] || [1, 1, 1], 0.86 + r() * 0.28);
  const trimTint = tint([1, 1, 1], 0.9 + r() * 0.2);
  const M = G.M;

  // -- plinth: a taller, darker base course. Every real building has one, and
  // it is what stops the mass reading as a box that was dropped on the ground.
  const plinthH = b.kind === 'warehouse' ? 1.2 : 0.9;
  G.box(arr, b.x, y, b.z, b.w + 0.24, plinthH, b.d + 0.24, tint(wallTint, 0.72), M(F.plinth));

  // -- the mass.
  const bodyH = b.h - plinthH;
  G.box(arr, b.x, y + plinthH, b.z, b.w, bodyH, b.d, wallTint, M(F.wall));

  // -- horizontal banding. A string course at each floor line turns a tall
  // slab into a stack of storeys, which is what a real elevation is.
  if (detail > 0.28) {
    const floorH = bodyH / Math.max(1, b.floors);
    const bandEvery = b.floors > 6 ? 2 : 1;
    for (let f = 1; f < b.floors; f += bandEvery) {
      const by = y + plinthH + f * floorH;
      G.band(arr, b.x, by - 0.12, b.z, b.w, b.d, 0.10, 0.24, trimTint, M(F.band));
    }
    // A deeper cornice at the top.
    G.band(arr, b.x, y + b.h - 0.55, b.z, b.w, b.d, 0.34, 0.55, trimTint, M(F.band));
  }

  // -- openings.
  if (detail > 0.4) {
    const floorH = bodyH / Math.max(1, b.floors);
    const baseY = y + plinthH + 0.9;

    if (b.kind === 'warehouse') {
      // A big roller shutter and a personnel door. This is the single detail
      // that makes a warehouse read as a warehouse rather than a shed.
      G.shutter(arr, b.x, y + 0.02, b.z + b.d / 2 + 0.06, Math.min(9, b.w * 0.42), 4.4, 'z+', [0.34, 0.36, 0.34]);
      G.door(arr, b.x + b.w * 0.3, y, b.z + b.d / 2 + 0.05, 1.0, 2.1, 'z+', r, false);
      G.signBoard(arr, b.x, y + 5.0, b.z + b.d / 2 + 0.08, Math.min(7, b.w * 0.3), 1.1, 'z+', r() < 0.5);
    } else if (b.kind === 'shop') {
      // Ground-floor glazing with an awning over it, then flats above.
      const bays = Math.max(2, Math.round(b.w / 7));
      for (let i = 0; i < bays; i++) {
        const bx = b.x - b.w / 2 + (b.w / bays) * (i + 0.5);
        G.window(arr, bx, y + 0.9, b.z + b.d / 2 + 0.02, Math.min(4.2, (b.w / bays) * 0.7), 2.5, 'z+', r);
        if (r() < 0.55) G.bladeSign(arr, bx, y + 3.6, b.z + b.d / 2 + 0.4, 1.1, 'z+', [0.85, 0.3 + 0.3 * r(), 0.2]);
      }
      G.door(arr, b.x, y, b.z + b.d / 2 + 0.04, 1.4, 2.4, 'z+', r, true);
      for (let f = 1; f < b.floors; f++) {
        const cols = Math.max(1, Math.round(b.w / 5.5));
        for (let c = 0; c < cols; c++) {
          if (r() < 0.12) continue;
          const wx = b.x - b.w / 2 + 2.2 + ((b.w - 4.4) * c) / Math.max(1, cols - 1);
          G.window(arr, wx, baseY + (f - 1) * floorH, b.z + b.d / 2 + 0.02, 1.5, 1.9, 'z+', r);
        }
      }
    } else {
      // Residential and office: a regular grid of openings on the front, and
      // a plainer treatment on the other three sides, which is what a real
      // block does and what stops every facade being equally busy.
      const face = b.doorSide === 0 ? 'z+' : 'x+';
      const cols = Math.max(1, Math.round((face === 'z+' ? b.w : b.d) / 5.2));
      const span = (face === 'z+' ? b.w : b.d) - 3.6;
      for (let f = 0; f < b.floors; f++) {
        const wy = baseY + f * floorH;
        if (wy + 2.4 > y + b.h - 0.9) break;
        for (let c = 0; c < cols; c++) {
          if (r() < 0.10) continue;
          const t = cols === 1 ? 0.5 : c / (cols - 1);
          const o = -span / 2 + span * t;
          if (face === 'z+') G.window(arr, b.x + o, wy, b.z + b.d / 2 + 0.02, 1.35, 1.75, 'z+', r);
          else G.window(arr, b.x + b.w / 2 + 0.02, wy, b.z + o, 1.35, 1.75, 'x+', r);
        }
      }
      // A balcony every other floor on a residential block.
      if (b.kind === 'house' && detail > 0.55) {
        for (let f = 1; f < b.floors; f += 2) {
          const wy = baseY + f * floorH;
          if (wy > y + b.h - 2.6) break;
          if (face === 'z+') G.balcony(arr, b.x + (r() - 0.5) * 2, wy - 0.9, b.z + b.d / 2 + 0.7, Math.min(3.4, b.w * 0.4), 1.3, trimTint, M(F.band));
        }
      }
      if (face === 'x+' && detail > 0.6 && b.floors > 2) {
        G.fireEscape(arr, b.x, y + 3.2, b.z, 3.0, Math.min(3, b.floors - 1), bodyH / b.floors, 'x');
      }
      G.door(arr, face === 'z+' ? b.x : b.x + b.w / 2 + 0.04, y, face === 'z+' ? b.z + b.d / 2 + 0.04 : b.z, 1.2, 2.3, face, r, b.kind === 'shop');
    }

    // Side and rear elevations get a cheaper, sparser version of the same idea.
    if (detail > 0.5) {
      const rear = b.doorSide === 0 ? 'z-' : 'x-';
      const cols = Math.max(1, Math.round((rear === 'z-' ? b.w : b.d) / 8));
      for (let f = 0; f < b.floors; f++) {
        const wy = baseY + f * (bodyH / b.floors);
        if (wy + 1.9 > y + b.h - 0.9) break;
        for (let c = 0; c < cols; c++) {
          if (r() < 0.25) continue;
          const o = cols === 1 ? 0 : -((rear === 'z-' ? b.w : b.d) / 2 - 3) + (((rear === 'z-' ? b.w : b.d) - 6) * c) / Math.max(1, cols - 1);
          if (rear === 'z-') G.window(arr, b.x + o, wy, b.z - b.d / 2 - 0.02, 1.1, 1.4, 'z-', r);
          else G.window(arr, b.x - b.w / 2 - 0.02, wy, b.z + o, 1.1, 1.4, 'x-', r);
        }
      }
    }
  }

  // -- roof.
  const roofForms = ROOFS[b.kind] || ROOFS.house;
  const form = roofForms[Math.floor(b.roof * roofForms.length) % roofForms.length];
  if (form === 'gable' && detail > 0.3) {
    G.gableRoof(arr, b.x, y + b.h, b.z, b.w, b.d, Math.min(3.4, b.w * 0.22), [0.26, 0.24, 0.23], M('wall_corrugated'));
  } else if (form === 'hip') {
    G.gableRoof(arr, b.x, y + b.h, b.z, b.w, b.d, Math.min(2.6, b.w * 0.16), [0.30, 0.28, 0.26], M('wall_corrugated'));
  } else if (form === 'sawtooth') {
    // North-light industrial roofs: a run of small gables.
    const bays = Math.max(2, Math.round(b.w / 14));
    for (let i = 0; i < bays; i++) {
      const bx = b.x - b.w / 2 + (b.w / bays) * (i + 0.5);
      G.gableRoof(arr, bx, y + b.h, b.z, (b.w / bays) * 0.92, b.d * 0.9, 1.8, [0.28, 0.30, 0.30], M('wall_metal_shutter'));
    }
  } else if (form === 'setback' && detail > 0.3) {
    G.box(arr, b.x, y + b.h, b.z, b.w * 0.72, 2.4, b.d * 0.72, tint(wallTint, 0.95), M(F.wall));
    G.band(arr, b.x, y + b.h + 2.4, b.z, b.w * 0.72, b.d * 0.72, 0.3, 0.4, trimTint, M(F.band));
  } else {
    G.parapet(arr, b.x, y + b.h, b.z, b.w, b.d, 0.75, tint(wallTint, 0.9), M(F.band));
  }

  // -- rooftop plant. The single cheapest thing that makes a skyline look
  // inhabited: nothing is ever on a real roof except machinery and access.
  if (detail > 0.5) {
    const roofY = y + b.h + (form === 'setback' ? 2.8 : form === 'gable' || form === 'hip' || form === 'sawtooth' ? Math.min(3.4, b.w * 0.22) : 0.75);
    const flat = form === 'flat' || form === 'setback';
    if (flat) {
      const n = 1 + Math.floor(r() * 3);
      for (let i = 0; i < n; i++) {
        const px = b.x + (r() - 0.5) * b.w * 0.6;
        const pz = b.z + (r() - 0.5) * b.d * 0.6;
        const roll = r();
        if (roll < 0.4) G.acUnit(arr, px, roofY, pz, 1.5 + r() * 0.9, 1.1 + r() * 0.6, 0.8 + r() * 0.4, r);
        else if (roll < 0.6) G.vent(arr, px, roofY, pz, 0.8 + r() * 1.2, 0.16);
        else if (roll < 0.75) G.chimney(arr, px, roofY, pz, 0.8, 0.7, 1.2 + r(), tint(wallTint, 0.8), M(F.band));
        else G.roofTank(arr, px, roofY, pz, 0.75, 1.1);
      }
      if (r() < 0.5) {
        G.vent(arr, b.x + (r() - 0.5) * b.w * 0.7, roofY, b.z + (r() - 0.5) * b.d * 0.7, 1.6, 0.22);
      }
    }
    if (detail > 0.62) {
      // Wall-mounted services: downpipes and a cable run on the flank.
      const px = b.doorSide === 0 ? b.x + b.w / 2 + 0.12 : b.x - b.w / 2 - 0.12;
      G.pipeRun(arr, px, y, b.z - b.d * 0.45, b.d * 0.9, 'z', [0.42, 0.42, 0.40], M('metal_bare'), 3);
      G.utilityBox(arr, b.x + b.w * 0.28, y + 1.4, b.z + b.d / 2 + 0.16, 0.5, 0.7, 0.24);
    }
  }

  // -- signage.
  if (b.sign && detail > 0.42) {
    const face = b.doorSide === 0 ? 'z+' : 'x+';
    const lit = night ? r() < 0.65 : r() < 0.3;
    if (face === 'z+') G.signBoard(arr, b.x, y + 3.2 + (b.kind === 'shop' ? 0.9 : 0), b.z + b.d / 2 + 0.1, Math.min(5.5, b.w * 0.3), 0.9, 'z+', lit);
    else G.signBoard(arr, b.x + b.w / 2 + 0.1, y + 3.2, b.z, Math.min(5.5, b.d * 0.3), 0.9, 'x+', lit);
  }
}

/**
 * A road, with its kerbs, footway, markings and drainage.
 *
 * The markings are the part that matters. A grey strip with a darker strip
 * beside it is not a road; a road has a centre line, lane divisions, and the
 * wear where the tyres run. Markings are placed in road space and are laid
 * slightly proud of the surface, which is why they read at a grazing angle.
 */
export function buildRoad(G, arr, r, detail, seed) {
  const rand = rng(r.id * 2654435761 + 7);
  const y = terrainHeight(r.x + r.w / 2, r.z + r.d / 2, seed) + 0.07;
  const along = r.w >= r.d ? 'x' : 'z';
  const length = along === 'x' ? r.w : r.d;
  const width = along === 'x' ? r.d : r.w;
  const M = G.M;

  // Carriageway, laid as a strip of panels so the ground-planar projection has
  // the surface at a consistent height along a road that crosses uneven ground.
  const panels = Math.max(1, Math.round(length / 40));
  for (let i = 0; i < panels; i++) {
    const t = (i + 0.5) / panels;
    const cx = along === 'x' ? r.x + r.w / 2 : r.x + r.w / 2;
    const cz = along === 'x' ? r.z + r.d / 2 : r.z + r.d / 2;
    const px = along === 'x' ? r.x + (r.w / panels) * (i + 0.5) : cx;
    const pz = along === 'x' ? cz : r.z + (r.d / panels) * (i + 0.5);
    const py = terrainHeight(px, pz, seed) + 0.07;
    const segLen = (along === 'x' ? r.w : r.d) / panels;
    G.box(arr, px, py, pz, along === 'x' ? segLen : r.w, 0.18, along === 'x' ? r.d : segLen,
      [0.9, 0.9, 0.92], M(r.main >= 2 ? 'road_asphalt' : r.main === 1 ? 'road_asphalt_worn' : 'pavement_concrete'));
  }

  // Footway and kerb on both sides.
  const fw = 2.4;
  const kerbH = 0.14;
  for (const side of [-1, 1]) {
    const ox = along === 'x' ? 0 : side * (r.w / 2 + fw / 2);
    const oz = along === 'x' ? side * (r.d / 2 + fw / 2) : 0;
    const fy = terrainHeight(r.x + r.w / 2 + ox, r.z + r.d / 2 + oz, seed) + 0.07;
    G.box(arr, r.x + r.w / 2 + ox, fy + kerbH, r.z + r.d / 2 + oz,
      along === 'x' ? r.w : fw, kerbH, along === 'x' ? fw : r.d, [0.86, 0.86, 0.84], M('pavement_concrete'));
  }

  if (detail < 0.5) return;

  // Centre line: dashed on minor roads, double on a main road.
  const dash = 3.2, gap = 4.4;
  const yTop = y + 0.19;
  const centreOffset = 0;
  const lines = r.main >= 2 ? [-0.55, 0.55] : r.main === 1 ? [0] : [];
  for (const lo of lines) {
    for (let t = 0; t < length; t += dash + gap) {
      if (t + dash > length) break;
      const mid = t + dash / 2;
      const px = along === 'x' ? r.x + mid : r.x + r.w / 2 + lo;
      const pz = along === 'x' ? r.z + r.d / 2 + lo : r.z + mid;
      const py = terrainHeight(px, pz, seed) + 0.19;
      G.plane(arr, px, py, pz, along === 'x' ? dash : 0.16, along === 'x' ? 0.16 : dash,
        [0.86, 0.85, 0.78], M('road_paint'));
    }
  }
  // Edge lines, continuous, just inside the kerb.
  for (const side of [-1, 1]) {
    for (let t = 0; t < length; t += 24) {
      const segLen = Math.min(24, length - t);
      const px = along === 'x' ? r.x + t + segLen / 2 : r.x + r.w / 2 + side * (r.w / 2 - 0.5);
      const pz = along === 'x' ? r.z + r.d / 2 + side * (r.d / 2 - 0.5) : r.z + t + segLen / 2;
      const py = terrainHeight(px, pz, seed) + 0.19;
      G.plane(arr, px, py, pz, along === 'x' ? segLen : 0.13, along === 'x' ? 0.13 : segLen,
        [0.80, 0.79, 0.72], M('road_paint'));
    }
  }

  // Drainage and manholes at intervals — small, but they break the surface up.
  const gullyEvery = 55;
  for (let t = gullyEvery; t < length; t += gullyEvery) {
    const side = (Math.floor(t / gullyEvery) % 2) ? 1 : -1;
    const px = along === 'x' ? r.x + t : r.x + r.w / 2 + side * (r.w / 2 - 0.6);
    const pz = along === 'x' ? r.z + r.d / 2 + side * (r.d / 2 - 0.6) : r.z + t;
    G.manhole(arr, px, terrainHeight(px, pz, seed) + 0.2, pz);
  }

  // Street lighting and signal heads. Placed on a real spacing with the arm
  // reaching over the carriageway, alternating sides, which is how a road is
  // actually lit and is a large part of why a lit street reads as a street.
  if (detail > 0.6) {
    const spacing = r.main >= 2 ? 34 : 46;
    for (let t = spacing * 0.5; t < length; t += spacing) {
      const side = (Math.floor(t / spacing) % 2) ? 1 : -1;
      const px = along === 'x' ? r.x + t : r.x + r.w / 2 + side * (r.w / 2 + 1.1);
      const pz = along === 'x' ? r.z + r.d / 2 + side * (r.d / 2 + 1.1) : r.z + t;
      G.streetLamp(arr, px, terrainHeight(px, pz, seed) + 0.21, pz, 8.0, along === 'x' ? 'z' : 'x', rand);
    }
    if (r.main >= 2 && detail > 0.7) {
      for (let t = 70; t < length; t += 150) {
        const side = (Math.floor(t / 150) % 2) ? 1 : -1;
        const px = along === 'x' ? r.x + t : r.x + r.w / 2 + side * (r.w / 2 + 1.2);
        const pz = along === 'x' ? r.z + r.d / 2 + side * (r.d / 2 + 1.2) : r.z + t;
        G.trafficLight(arr, px, terrainHeight(px, pz, seed) + 0.21, pz, 5.4, along === 'x' ? 'x' : 'z');
      }
    }
  }
}

/**
 * A crossing: a ladder of bars plus the stop line.
 *
 * Placed on the road surface as decals rather than modelled kerbs, because a
 * zebra crossing is paint and painting it is what it is.
 */
export function buildCrossing(G, arr, x, z, along, width, depth, seed) {
  const y = terrainHeight(x, z, seed) + 0.2;
  const bars = Math.max(4, Math.round(depth / 1.1));
  for (let i = 0; i < bars; i++) {
    const o = -depth / 2 + (depth / bars) * (i + 0.5);
    if (along === 'x') G.plane(arr, x, y, z + o, width, (depth / bars) * 0.55, [0.84, 0.83, 0.76], G.M('road_paint'));
    else G.plane(arr, x + o, y, z, (depth / bars) * 0.55, width, [0.84, 0.83, 0.76], G.M('road_paint'));
  }
  // Stop line behind it.
  if (along === 'x') G.plane(arr, x - width * 0.5, y, z, 0.45, depth * 0.9, [0.84, 0.83, 0.76], G.M('road_paint'));
  else G.plane(arr, x, y, z - width * 0.5, depth * 0.9, 0.45, [0.84, 0.83, 0.76], G.M('road_paint'));
}

/**
 * Dress the frontage of a building: what is left on the pavement outside it.
 *
 * Placed along the side the entrance is on, at a spacing that leaves gaps, and
 * with a bias toward the wall. The three placement rules — against the wall,
 * mid-pavement, at the kerb — are what stop a pavement looking like a
 * randomly-seeded inventory rather than a place.
 */
export function dressBuilding(G, arr, b, detail, seed, night) {
  if (detail < 0.34) return;
  const r = rng(b.seed * 2718281 + 19);
  const face = b.doorSide === 0 ? 'z+' : 'x+';
  const span = face === 'z+' ? b.w : b.d;
  const count = Math.max(1, Math.round(span / 16));

  for (let i = 0; i < count; i++) {
    if (r() > detail) continue;
    const t = count === 1 ? 0.5 : i / (count - 1);
    const o = -span / 2 + 2 + (span - 4) * t + (r() - 0.5) * 2;
    // Against the wall.
    const wx = face === 'z+' ? b.x + o : b.x + (b.w / 2 + 0.7);
    const wz = face === 'z+' ? b.z + b.d / 2 + 0.7 : b.z + o;
    dressStreet(G, arr, wx, wz, face, detail, r, seed);
    // Mid-pavement, and nearer the kerb.
    if (r() < 0.5 * detail) {
      const mx = face === 'z+' ? b.x + o + (r() - 0.5) * 1.5 : b.x + b.w / 2 + 1.7;
      const mz = face === 'z+' ? b.z + b.d / 2 + 1.7 : b.z + o + (r() - 0.5) * 1.5;
      const roll = r();
      if (roll < 0.28) G.bench(arr, mx, terrainHeight(mx, mz, seed) + 0.2, mz, face === 'z+' ? 'x' : 'z');
      else if (roll < 0.46) G.hydrant(arr, mx, terrainHeight(mx, mz, seed) + 0.2, mz);
      else if (roll < 0.62) G.bollard(arr, mx, terrainHeight(mx, mz, seed) + 0.2, mz);
      else if (roll < 0.78) buildBush(G, arr, mx, mz, 0.8 + r() * 0.5, r, seed);
      else G.bin(arr, mx, terrainHeight(mx, mz, seed) + 0.2, mz, r);
    }
  }

  // A bus shelter or a container on the wider frontages, because a pavement
  // with nothing large on it is a pavement nobody uses.
  if (detail > 0.62 && span > 26 && r() < 0.34) {
    const o = (r() - 0.5) * (span - 12);
    const sx = face === 'z+' ? b.x + o : b.x + b.w / 2 + 2.4;
    const sz = face === 'z+' ? b.z + b.d / 2 + 2.4 : b.z + o;
    const y = terrainHeight(sx, sz, seed) + 0.2;
    if (r() < 0.5) G.busShelter(arr, sx, y, sz, 4.2, face === 'z+' ? 'z' : 'x');
    else G.container(arr, sx, y, sz, 6.1, face === 'z+' ? 'z' : 'x', r);
  }
}

/**
 * A tree.
 *
 * A trunk with a real taper, two or three branch stubs, and a canopy built
 * from seeded stacked rings. The canopy wobble is per-tree, so a street of
 * thirty trees reads as thirty trees rather than one tree thirty times, which
 * was the single most visible flaw of the previous generator.
 */
export function buildTree(G, arr, t, detail, seed) {
  const y = terrainHeight(t.x, t.z, seed);
  const r = rng(t.seed * 65537 + 11);
  const s = t.s;
  const trunkH = (t.type === 'pine' ? 2.6 : 2.2) * s;
  const trunkR = 0.16 * s + 0.05;
  const bark = [0.30 + 0.08 * r(), 0.24 + 0.06 * r(), 0.18 + 0.05 * r()];
  G.taper(arr, t.x, y, t.z, trunkR * 2.1, trunkR * 2.1, trunkR, trunkR, trunkH, bark, G.M('bark'));
  // A root flare, which is what stops a trunk looking like a pole pushed into
  // the ground.
  G.taper(arr, t.x, y, t.z, trunkR * 3.0, trunkR * 3.0, trunkR * 2.1, trunkR * 2.1, 0.28, bark, G.M('bark'));

  const leafBase = t.type === 'pine' ? [0.13, 0.30, 0.16] : t.type === 'broadleaf' ? [0.19, 0.38, 0.20] : [0.15, 0.33, 0.17];
  const leaf = tint(leafBase, 0.82 + r() * 0.4);
  const mLeaf = G.M(t.type === 'pine' ? 'foliage' : 'foliage');

  if (t.type === 'pine') {
    // Conifer: three stacked cones of decreasing radius.
    const layers = detail > 0.5 ? 4 : 2;
    for (let i = 0; i < layers; i++) {
      const t0 = i / layers;
      const rr = (1.5 - t0 * 0.95) * s;
      const ly = y + trunkH * 0.6 + t0 * 2.4 * s;
      G.cone(arr, t.x, ly, t.z, rr, 1.7 * s, tint(leaf, 0.86 + 0.3 * t0), mLeaf, detail > 0.5 ? 8 : 6);
    }
  } else {
    // Broadleaf: a trunk, a few branches, and a canopy in two or three lobes.
    if (detail > 0.45) {
      const branches = 2 + Math.floor(r() * 3);
      for (let i = 0; i < branches; i++) {
        const a = r() * Math.PI * 2;
        const len = (0.9 + r() * 0.9) * s;
        const bx = t.x + Math.cos(a) * len * 0.4;
        const bz = t.z + Math.sin(a) * len * 0.4;
        G.taper(arr, bx, y + trunkH * (0.55 + r() * 0.25), bz, 0.09 * s, 0.09 * s, 0.05 * s, 0.05 * s, len, bark, G.M('bark'));
      }
    }
    const lobes = detail > 0.55 ? 3 : 1;
    for (let i = 0; i < lobes; i++) {
      const a = (i / lobes) * Math.PI * 2 + r();
      const off = lobes === 1 ? 0 : 0.5 * s;
      const rad = (1.5 + r() * 0.9) * s * (lobes === 1 ? 1 : 0.72);
      G.blob(arr,
        t.x + Math.cos(a) * off, y + trunkH * (0.85 + 0.12 * r()), t.z + Math.sin(a) * off,
        rad, rad * 1.5, tint(leaf, 0.9 + 0.2 * r()), mLeaf, r,
        detail > 0.5 ? 4 : 2, detail > 0.5 ? 7 : 5);
    }
  }
}

/** A bush or shrub, for verges and building fronts. */
export function buildBush(G, arr, x, z, s, r, seed) {
  const y = terrainHeight(x, z, seed);
  const leaf = tint([0.16, 0.34, 0.17], 0.8 + r() * 0.45);
  G.blob(arr, x, y, z, 0.7 * s, 0.95 * s, leaf, G.M('foliage'), r, 2, 6);
}

/**
 * Pavement dressing beside a building.
 *
 * This is the layer that decides whether a street looks inhabited. A real
 * pavement has: things against the wall, things by the kerb, and things
 * abandoned in between. The three are placed by different rules, which is why
 * doing it procedurally is worth doing at all.
 */
export function dressStreet(G, arr, x, z, facing, detail, rand, seed) {
  const y = terrainHeight(x, z, seed) + 0.2;
  const M = G.M;
  const roll = rand();

  // Against the wall: bins, utility boxes, a stack of crates, a parked pallet.
  if (roll < 0.30) G.bin(arr, x, y, z, rand);
  else if (roll < 0.46) G.utilityBox(arr, x, y, z, 0.6 + rand() * 0.4, 1.0 + rand() * 0.5, 0.3);
  else if (roll < 0.60) {
    G.pallet(arr, x, y, z);
    if (rand() < 0.6) G.crate(arr, x + (rand() - 0.5) * 0.6, y + 0.12, z + (rand() - 0.5) * 0.6, 0.5 + rand() * 0.4, rand);
  } else if (roll < 0.72 && detail > 0.7) G.crate(arr, x, y, z, 0.55 + rand() * 0.5, rand);
  else if (roll < 0.80) G.trafficCone(arr, x, y, z);

  // By the kerb: a lamp, a sign, a hydrant, a meter.
  if (detail > 0.6) {
    const r2 = rand();
    if (r2 < 0.30) G.hydrant(arr, x, y, z);
    else if (r2 < 0.52) G.bollard(arr, x, y, z);
    else if (r2 < 0.68 && detail > 0.78) G.mailbox(arr, x, y, z, facing);
  }
}

/** A street lamp at its real 8 m. */
export function buildStreetLamp(G, arr, x, z, facing, seed) {
  G.streetLamp(arr, x, terrainHeight(x, z, seed) + 0.2, z, 8.0, facing, Math.random === null ? null : seededUnit(0.3));
}

let unitStream = 0;
function seededUnit(k) {
  unitStream = (unitStream * 1103515245 + 12345) & 0x7fffffff;
  return () => (unitStream / 0x7fffffff + k) % 1;
}

/**
 * A vehicle, with enough parts to read as a vehicle.
 *
 * Body, greenhouse, glazing, wheels with hubs, lights, bumpers and mirrors.
 * Solid materials only — see materials.mjs for why a moving object must not
 * use a world-locked texture.
 */
export function buildCar(G, arr, c, seed, lod = 0) {
  const r = seededUnit(c.id);
  const y = terrainHeight(c.x, c.z, seed) + 0.32;
  const horiz = c.horizontal;
  const bodyCol = c.color;
  const dark = [0.10, 0.11, 0.12];
  const M = G.M;
  const glassCol = [0.30, 0.38, 0.42];

  const L = horiz ? 4.4 : 2.0;
  const W = horiz ? 2.0 : 4.4;
  // Lower body.
  G.box(arr, c.x, y, c.z, horiz ? L : W, 0.62, horiz ? W : L, bodyCol, M('paint_metal'));
  // Sills, slightly inset, which is what gives a car a shadow line along its side.
  G.box(arr, c.x, y + 0.18, c.z, horiz ? L * 0.94 : W * 1.02, 0.22, horiz ? W * 1.02 : L * 0.94, [bodyCol[0] * 0.5, bodyCol[1] * 0.5, bodyCol[2] * 0.5], M('metal_dark'));
  // Bonnet and boot, stepped down from the greenhouse.
  G.box(arr, c.x, y + 0.62, c.z - (horiz ? 1.55 : 0), horiz ? 1.5 : W * 0.92, 0.14, horiz ? W * 0.92 : 1.5, tint(bodyCol, 1.03), M('paint_metal'));
  G.box(arr, c.x, y + 0.62, c.z + (horiz ? 1.55 : 0), horiz ? 1.4 : W * 0.92, 0.14, horiz ? W * 0.92 : 1.4, tint(bodyCol, 1.03), M('paint_metal'));
  // Greenhouse.
  const ghY = y + 0.76;
  G.box(arr, c.x, ghY, c.z, horiz ? 2.3 : W * 0.9, 0.52, horiz ? W * 0.9 : 2.3, glassCol, M('glass'));
  // Roof panel over the greenhouse.
  G.box(arr, c.x, ghY + 0.52, c.z, horiz ? 2.1 : W * 0.86, 0.09, horiz ? W * 0.86 : 2.1, bodyCol, M('paint_metal'));
  // Bumpers.
  for (const s of [-1, 1]) {
    const bx = horiz ? c.x + s * (L / 2 - 0.1) : c.x;
    const bz = horiz ? c.z : c.z + s * (L / 2 - 0.1);
    G.box(arr, bx, y + 0.20, bz, horiz ? 0.22 : W * 0.98, 0.26, horiz ? W * 0.98 : 0.22, dark, M('metal_dark'));
  }
  // Lights.
  for (const s of [-1, 1]) {
    const lx = horiz ? c.x + s * (L / 2 - 0.05) : c.x + (horiz ? 0 : s * (W / 2 - 0.3));
    const lz = horiz ? c.z + s * (W / 2 - 0.3) : c.z + s * (L / 2 - 0.05);
    G.box(arr, lx, y + 0.44, lz, horiz ? 0.14 : 0.5, 0.18, horiz ? 0.5 : 0.14, [0.95, 0.95, 0.9], M('lamp'));
    const rx = horiz ? c.x - s * (L / 2 - 0.05) : c.x + (horiz ? 0 : s * (W / 2 - 0.3));
    const rz = horiz ? c.z + s * (W / 2 - 0.3) : c.z - s * (L / 2 - 0.05);
    G.box(arr, rx, y + 0.44, rz, horiz ? 0.14 : 0.45, 0.16, horiz ? 0.45 : 0.14, [0.6, 0.10, 0.08], M('plastic'));
  }
  // Mirrors.
  for (const s of [-1, 1]) {
    const mx = horiz ? c.x + 0.9 : c.x + s * (W / 2 + 0.1);
    const mz = horiz ? c.z + s * (W / 2 + 0.1) : c.z + 0.9;
    G.box(arr, mx, y + 0.86, mz, horiz ? 0.18 : 0.16, 0.10, horiz ? 0.16 : 0.18, bodyCol, M('paint_metal'));
  }
  // Wheels. Tyre, then a hub, so they are not black discs — but only at full
  // detail. Past about a hundred metres a wheel is two pixels across, and past
  // the far cut-off the whole car is a single box (see buildCarProxy).
  if (lod > 1) return;
  const axles = horiz ? [[-1.35, -0.92], [-1.35, 0.92], [1.35, -0.92], [1.35, 0.92]] : [[-0.92, -1.35], [0.92, -1.35], [-0.92, 1.35], [0.92, 1.35]];
  const segs = lod === 0 ? 9 : 6;
  for (const [ax, az] of axles) {
    G.cylinder(arr, c.x + ax - 0.11, y - 0.02, c.z + az, 0.34, 0.22, [0.07, 0.07, 0.08], M('rubber'), segs);
    if (lod === 0) G.cylinder(arr, c.x + ax - 0.12, y + 0.02, c.z + az, 0.20, 0.02, [0.42, 0.43, 0.45], M('metal_bare'), 6);
  }
}

/**
 * The reduced car: a body and a greenhouse, no wheels, no lights, no trim.
 *
 * At a hundred and fifty metres this is indistinguishable from a fully
 * detailed car, and a hundred cars is the difference between a frame budget
 * and a stutter.
 */
export function buildCarProxy(G, arr, c, seed) {
  const y = terrainHeight(c.x, c.z, seed) + 0.32;
  const horiz = c.horizontal;
  G.box(arr, c.x, y, c.z, horiz ? 4.4 : 2.0, 1.05, horiz ? 2.0 : 4.4, c.color, G.M('paint_metal'));
  G.box(arr, c.x, y + 0.76, c.z, horiz ? 2.2 : 1.8, 0.5, horiz ? 1.8 : 2.2, [0.30, 0.38, 0.42], G.M('glass'));
}

/**
 * A character, built to a real 1.8 m.
 *
 * Proportions are the thing: a head about an eighth of the height, shoulders
 * roughly a quarter of the height across, legs a bit under half. Anything built
 * to "looks right at a glance" ends up with a nine-head giant or a Lego figure,
 * and a crowd of those destroys every scene it appears in.
 */
export function buildCharacter(G, arr, x, z, y0, id, job, activity, phase, seed, lod = 0) {
  const r = seededUnit(id * 31 + 7);
  const y = y0 !== undefined ? y0 : terrainHeight(x, z, seed);
  const M = G.M;
  const bob = Math.sin(phase * 2) * 0.03 * (activity === 'travel' ? 1 : 0.15);
  const swing = activity === 'travel' ? Math.sin(phase) * 0.5 : 0;

  const skinTint = tint([0.78, 0.60, 0.48], 0.82 + r() * 0.3);
  const hairCol = tint([0.18, 0.14, 0.11], 0.6 + r() * 1.2);
  const shirt = job === 'worker' ? [0.86, 0.58, 0.22] : job === 'merchant' ? [0.30, 0.55, 0.78] : job === 'service' ? [0.24, 0.58, 0.44] : [0.56, 0.40, 0.72];
  const trousers = tint([0.24, 0.26, 0.32], 0.7 + r() * 0.8);

  // Proportions, as fractions of a total height that they must sum to.
  //
  // They did not, at first: 0.47 + 0.30 + 0.125 plus a neck came to 1.60 m for
  // a "1.78 m" character. It is the kind of unit error that is invisible in
  // review and obvious on screen — a crowd of fifteen percenters standing next
  // to a 2.1 m door. test_geometry.mjs now measures the top of the head against
  // H, so the next edit to one segment cannot quietly break the others.
  const H = 1.78;
  const FRACTION = { legs: 0.475, torso: 0.300, neck: 0.025, head: 0.150, hair: 0.050 };
  // Per-person build: a few centimetres either way, and a little in the
  // shoulders. A crowd where everyone is the same width is the most obvious
  // tell that a "population" is a loop.
  const build = 0.94 + r() * 0.13;
  const H2 = H * build;
  const legH = H2 * FRACTION.legs;
  const torsoH = H2 * FRACTION.torso;
  const neckH = H2 * FRACTION.neck;
  const headH = H2 * FRACTION.head;
  const hairH = H2 * FRACTION.hair;
  const shoulderW = H2 * 0.245 * (0.94 + r() * 0.14);
  const legW = H2 * 0.075 * (0.95 + r() * 0.12);

  // Legs, with a swing so a walking character reads as walking from any angle.
  for (const s of [-1, 1]) {
    const off = swing * s;
    const lx = x + (s * legW * 0.7);
    const lz = z + off * legH * 0.45;
    G.box(arr, lx, y + bob, lz, legW, legH * 0.98, legW, trousers, M('fabric'));
    // Shoe.
    G.box(arr, lx, y + bob, lz + 0.04, legW * 1.15, 0.07, legW * 1.9, [0.12, 0.11, 0.11], M('rubber'));
  }
  // Hips and torso.
  G.box(arr, x, y + legH + bob, z, shoulderW * 0.82, torsoH * 0.24, H * 0.13, trousers, M('fabric'));
  G.taper(arr, x, y + legH + torsoH * 0.24 + bob, z, shoulderW * 0.86, H * 0.14, shoulderW, H * 0.13, torsoH * 0.76, shirt, M('fabric'));
  // Arms and hands. A hand is a centimetre of geometry that disappears past
  // about forty metres, so the mid LOD stops at the forearm.
  for (const s of [-1, 1]) {
    const off = -swing * s;
    G.box(arr, x + s * shoulderW * 0.56, y + legH + torsoH * 0.2 + bob, z + off * 0.14, H * 0.055, torsoH * 0.62, H * 0.055, shirt, M('fabric'));
    if (lod === 0) G.box(arr, x + s * shoulderW * 0.56, y + legH + torsoH * 0.2 - 0.05 + bob, z + off * 0.14, H * 0.05, 0.09, H * 0.05, skinTint, M('skin'));
  }
  // Neck, head, hair — stacked so the top of the head lands on H exactly.
  G.box(arr, x, y + legH + torsoH + bob, z, H2 * 0.05, neckH, H2 * 0.05, skinTint, M('skin'));
  G.box(arr, x, y + legH + torsoH + neckH + bob, z, H2 * 0.098, headH * 0.72, H2 * 0.098, skinTint, M('skin'));
  if (lod === 0) {
    G.box(arr, x, y + legH + torsoH + neckH + headH * 0.55 + bob, z, H2 * 0.104, headH * 0.45, H2 * 0.104, skinTint, M('skin'));
    G.box(arr, x, y + legH + torsoH + neckH + headH * 0.72 + bob, z, H2 * 0.104, hairH, H2 * 0.104, hairCol, M('fabric'));
  }
}

/**
 * The reduced character: legs, torso, head. Three boxes.
 *
 * A crowd is the most-repeated geometry in the game and the least-inspected
 * object in it, so it is where the LOD budget has to be spent. At sixty metres
 * a three-box figure and a thirteen-box one are the same eight pixels tall.
 */
export function buildCharacterProxy(G, arr, x, z, y0, id, job, activity, phase, seed) {
  const r = seededUnit(id * 31 + 7);
  const y = y0 !== undefined ? y0 : terrainHeight(x, z, seed);
  const bob = Math.sin(phase * 2) * 0.03 * (activity === 'travel' ? 1 : 0.15);
  const shirt = job === 'worker' ? [0.86, 0.58, 0.22] : job === 'merchant' ? [0.30, 0.55, 0.78] : job === 'service' ? [0.24, 0.58, 0.44] : [0.56, 0.40, 0.72];
  const trousers = tint([0.24, 0.26, 0.32], 0.7 + r() * 0.8);
  const H = 1.78, legH = H * 0.47, torsoH = H * 0.30;
  G.box(arr, x, y + bob, z, H * 0.16, legH, H * 0.09, trousers, G.M('fabric'));
  G.box(arr, x, y + legH + bob, z, H * 0.245, torsoH, H * 0.13, shirt, G.M('fabric'));
  G.box(arr, x, y + legH + torsoH + bob, z, H * 0.098, H * 0.125, H * 0.098, tint([0.78, 0.60, 0.48], 0.82 + r() * 0.3), G.M('skin'));
}
