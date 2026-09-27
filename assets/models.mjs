/**
 * Imported models — the curated list.
 *
 * This is the same discipline as `assets/database.mjs` for materials: every
 * entry is chosen on purpose and carries a written reason it belongs in
 * EMERGENT. Nothing here is a category sweep.
 *
 * ## Why not the whole catalogue
 *
 * Poly Haven ships 521 models. Tagging its interior and prop categories gives
 * 174 of 313 candidates leaning gothic/vintage/antique/ornate, 46 modern or
 * industrial, and 93 unlabelled. Dropping a category sweep into a contemporary
 * city produces exactly the asset-store look this project is trying to avoid:
 * a mahogany dresser and a laminate desk in the same room. So the list is
 * short, and every entry states the room it goes in.
 *
 * ## Scale
 *
 * `realHeight` is the asset's height in metres at world scale, measured from
 * the imported mesh's own bounding box and asserted at bake time. Poly Haven's
 * API `dimensions` field is not usable for this — it reports a coffee table as
 * 12 m tall — so the import measures and the bake refuses a model that lands
 * more than 15% away from the declared value. A wrong-scale chair is more
 * damaging than a missing one: it makes the generated architecture around it
 * look wrong too.
 *
 * ## Fit
 *
 * `room` is the generated interior archetype this belongs in. The world
 * generator places by archetype, not by scattering, so a model only appears
 * where it is plausible.
 *
 * `tint` multiplies the vertex colour. Used to pull a whole family toward one
 * palette — a cafe's chairs should not be five unrelated colours.
 *
 * ## Licensing
 *
 * Every entry is CC0 from Poly Haven. `tools/assets/models.mjs` verifies the
 * download against the provider's published MD5 and writes the author, source
 * URL, licence and date into `assets/provenance.json`, and
 * `tools/assets/audit.mjs` fails the build if any of that goes missing.
 */

/** Interior archetypes the world generator furnishes. */
export const ROOM = {
  RESIDENTIAL: 'residential',
  OFFICE: 'office',
  RETAIL: 'retail',
  WORKSHOP: 'workshop'
};

/**
 * @typedef {object} ModelSpec
 * @property {string} id           Stable key, and the runtime mesh name.
 * @property {string} source       Poly Haven asset id.
 * @property {string} room         One of ROOM.
 * @property {string} reason       Why this asset is in this game.
 * @property {number} realHeight   Height in metres, checked at bake time.
 * @property {boolean} [flat]      A genuinely thin object — a picture frame, a
 *   notepad. Exempts it from the aspect-ratio check, which exists to catch a
 *   mis-declared scale rather than an extreme but real shape.
 * @property {[number,number,number]} [tint] Multiplied into vertex colour.
 * @property {string} [materialHint] Which baked material family it uses.
 */

/** @type {ModelSpec[]} */
export const MODELS = [
  // -- residential ---------------------------------------------------------
  {
    id: 'armchair',
    source: 'modern_arm_chair_01',
    room: ROOM.RESIDENTIAL,
    reason: 'A living-room chair. The single most-seen silhouette in a flat and the '
      + 'hardest thing to fake convincingly: curved upholstery over a timber frame. '
      + 'This is the asset that decides whether interiors look bought or built.',
    realHeight: 0.78,
    tint: [0.92, 0.90, 0.88]
  },
  {
    id: 'lounge_chair',
    source: 'mid_century_lounge_chair',
    room: ROOM.RESIDENTIAL,
    reason: 'A second, lower and wider chair. Two silhouettes rather than one repeated '
      + 'is the cheapest way to stop a room reading as a showroom.',
    realHeight: 0.76,
    tint: [0.88, 0.86, 0.84]
  },
  {
    id: 'coffee_table',
    source: 'modern_coffee_table_01',
    room: ROOM.RESIDENTIAL,
    reason: 'A stone-and-timber coffee table. Reads as a hard, cold surface against '
      + 'the chairs, which is what makes the pair look like a real living room.',
    realHeight: 0.38,
    tint: [1.0, 1.0, 1.0]
  },
  {
    id: 'side_table',
    source: 'side_table_01',
    room: ROOM.RESIDENTIAL,
    reason: 'The small table next to a chair, which is the object that makes seating '
      + 'look arranged rather than dropped on a floor.',
    realHeight: 0.52,
    tint: [0.95, 0.93, 0.90]
  },
  {
    id: 'sideboard',
    source: 'modern_wooden_cabinet',
    room: ROOM.RESIDENTIAL,
    reason: 'A low sideboard against a wall. Storage is the main thing stopping an '
      + 'interior wall from reading as empty, and a wide low mass balances the '
      + 'vertical shelving better than another tall piece would.',
    // Measured: 2.44 x 0.68 x 0.52 m as authored. Declared as 0.80 m tall, which
    // scales it to roughly 2.9 m wide — a real sideboard. An earlier 1.35 m
    // declaration doubled it to 4.8 m, which is what the plausibility check
    // exists to catch.
    realHeight: 0.80,
    tint: [0.93, 0.90, 0.86]
  },
  {
    id: 'shelving',
    source: 'steel_frame_shelves_03',
    room: ROOM.RESIDENTIAL,
    reason: 'Open steel shelving. Its grid of thin members is a different silhouette '
      + 'from the solid cabinet, so the two do not read as the same object twice.',
    realHeight: 1.80,
    tint: [0.90, 0.90, 0.92]
  },
  {
    id: 'ceiling_lamp',
    source: 'modern_ceiling_lamp_01',
    room: ROOM.RESIDENTIAL,
    reason: 'A hanging ceiling light. Interiors lit only from outside look like '
      + 'dioramas; a practical light source above the camera is what sells the room.',
    realHeight: 0.55,
    tint: [1.0, 1.0, 1.0]
  },
  {
    id: 'picture_frame',
    source: 'hanging_picture_frame_01',
    room: ROOM.RESIDENTIAL,
    reason: 'A wall-hung frame. The cheapest way to stop an interior wall reading as '
      + 'a bare plane, and it costs no floor space.',
    // A picture frame is 10 mm deep. Like the notepad, this is `flat` by nature.
    realHeight: 0.60,
    flat: true,
    tint: [0.85, 0.83, 0.80]
  },

  // -- office --------------------------------------------------------------
  {
    id: 'desk_chair',
    source: 'dining_chair_02',
    room: ROOM.OFFICE,
    reason: 'Office seating. Leather and a simple frame, which is what a desk chair '
      + 'actually is once the castors and headrest are abstracted away.',
    realHeight: 0.90,
    tint: [0.80, 0.80, 0.82]
  },
  {
    id: 'drawer_cabinet',
    source: 'drawer_cabinet',
    room: ROOM.OFFICE,
    reason: 'A filing pedestal. Offices are defined by their repetitive storage, and '
      + 'the drawer fronts give a flat facade some horizontal rhythm.',
    realHeight: 1.20,
    tint: [0.86, 0.86, 0.88]
  },
  {
    id: 'notepads',
    source: 'office_notepads',
    room: ROOM.OFFICE,
    reason: 'Paper on a desk. A tiny, flat, almost invisible asset that supplies the '
      + 'small-scale detail that empty desks conspicuously lack.',
    // A notepad really is 2 mm thick. `flat: true` exempts it from the aspect
    // check, which is about catching a *wrong scale*, and paper is the one
    // thing in this set that is legitimately extreme on that axis.
    realHeight: 0.30,
    flat: true,
    tint: [1.0, 1.0, 1.0]
  },
  {
    id: 'wall_clock',
    source: 'wall_clock',
    room: ROOM.OFFICE,
    reason: 'A wall clock. Offices have one, rooms do not, so it is a free way to '
      + 'date a space as a workplace.',
    realHeight: 0.30,
    tint: [0.90, 0.90, 0.90]
  },

  // -- retail --------------------------------------------------------------
  {
    id: 'stove',
    source: 'electric_stove',
    room: ROOM.RETAIL,
    reason: 'A cooking appliance for the food businesses. A cafe with no equipment '
      + 'reads as an empty unit with a sign on it.',
    realHeight: 0.92,
    tint: [0.95, 0.95, 0.96]
  },
  {
    id: 'enamel_pot',
    source: 'pot_enamel_01',
    room: ROOM.RETAIL,
    reason: 'Cookware on a counter. Small, rounded, and the kind of object that makes '
      + 'a worktop look used.',
    realHeight: 0.22,
    tint: [1.0, 1.0, 1.0]
  },
  {
    id: 'crate',
    source: 'plastic_crate_02',
    room: ROOM.RETAIL,
    reason: 'A stackable crate. The back-of-house object every shop has and no '
      + 'generated city has.',
    realHeight: 0.32,
    tint: [0.95, 0.92, 0.88]
  },

  // -- workshop / industrial ----------------------------------------------
  {
    id: 'workbench_stool',
    source: 'metal_stool_02',
    room: ROOM.WORKSHOP,
    reason: 'A workshop stool. Metal, round, and completely different in material '
      + 'from everything domestic in the set, which is what separates a garage from a flat.',
    realHeight: 0.62,
    tint: [0.88, 0.88, 0.90]
  },
  {
    id: 'power_box',
    source: 'power_box_01',
    room: ROOM.WORKSHOP,
    reason: 'A distribution board. The electrical clutter that industrial interiors '
      + 'are built out of, and it is a genuinely different mesh class from the furniture.',
    realHeight: 0.65,
    tint: [0.90, 0.91, 0.92]
  },
  {
    id: 'caged_light',
    source: 'caged_hanging_light',
    room: ROOM.WORKSHOP,
    reason: 'A caged industrial pendant. Workshop lighting is caged and domestic '
      + 'lighting is not, so the two fixtures separate the two room types visually.',
    realHeight: 0.45,
    tint: [0.88, 0.88, 0.86]
  },
  {
    id: 'wall_lamp',
    source: 'industrial_wall_lamp',
    room: ROOM.WORKSHOP,
    reason: 'A bulkhead wall lamp. Sconces at head height are what stop a tall blank '
      + 'industrial wall from reading as a plane.',
    realHeight: 0.30,
    tint: [0.90, 0.90, 0.90]
  },
  {
    id: 'barrel',
    source: 'barrel_03',
    room: ROOM.WORKSHOP,
    reason: 'A painted steel drum. Cylindrical, dented, and a shape the procedural '
      + 'kit cannot currently produce, so it earns its download.',
    realHeight: 0.90,
    tint: [0.92, 0.93, 0.95]
  },

  // -- street --------------------------------------------------------------
  {
    id: 'street_seating',
    source: 'modular_street_seating',
    room: ROOM.RESIDENTIAL,
    reason: 'Modular public seating. Street furniture built from a model rather than '
      + 'from boxes is the difference between a plaza and an empty tarmac square.',
    realHeight: 0.45,
    tint: [0.90, 0.90, 0.90]
  }
];

/** Look up a spec, failing loudly. A silent default is a prop at the wrong scale. */
export function modelSpec(id) {
  const spec = MODELS.find((m) => m.id === id);
  if (!spec) {
    throw new Error(`unknown model '${id}'. Known: ${MODELS.map((m) => m.id).join(', ')}`);
  }
  return spec;
}

/** Every spec for one room archetype, in declaration order. */
export function modelsForRoom(room) {
  return MODELS.filter((m) => m.room === room);
}
