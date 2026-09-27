/**
 * The EMERGENT material system.
 *
 * One table, two kinds of entry, and one uniform layout. Everything the
 * renderer draws carries a material index, and the shader resolves that index
 * into a texture layer, a tiling scale, a roughness and a metalness. There is
 * no second code path and no per-surface shader.
 *
 * ## Textured entries
 *
 * These come from the bake. `assets/database.mjs` selects them, `fetch.mjs`
 * downloads them, `bake.mjs` resamples and validates them, and the bake emits
 * `assets/materials.mjs` with the layer indices. They are CC0, from Poly Haven,
 * and the provenance for each one is in `assets/provenance.json`.
 *
 * ## Solid entries
 *
 * Everything the texture set has no business covering: window glass, painted
 * steel, rubber, foliage, road markings, lamp glass. A PBR shader can shade
 * these correctly with a constant — a car body is a painted surface, and a
 * painted surface is a dielectric with a roughness, not a photograph. Giving
 * them a texture would add bytes and buy nothing.
 *
 * ## How texture coordinates are derived, and why there is no UV attribute
 *
 * Static world geometry is axis-aligned, so a surface's texture coordinate is a
 * projection of its world position onto the plane its normal points out of. The
 * vertex shader picks the plane from the normal and scales by the material's
 * tiling density. This is box mapping, and it is the right choice here for three
 * reasons: it has no seams, it needs no unwrapping, and it is *world locked*,
 * so a brick wall's bricks stay where they are while the player walks past
 * instead of shimmering.
 *
 * It is also wrong for anything that moves. A car driving at 20 m/s under a
 * world-locked projection slides its paint across its own body, which is far
 * worse than having no texture at all. So moving geometry — cars, characters,
 * anything the simulation advances — uses solid materials and vertex colour.
 * That is a deliberate split, not an oversight, and it is why no vertex here
 * carries a UV.
 *
 * Vertex layout is therefore 10 floats: position(3), normal(3), colour(3),
 * material(1).
 */

/** How the fragment shader derives a texture coordinate. */
export const MAP_MODE = {
  /** Project onto the world XZ plane. Ground, roads, roofs, horizontal tops. */
  GROUND: 0,
  /** Project onto world ZY. Walls whose normal points along X. */
  WALL_X: 1,
  /** Project onto world XY. Walls whose normal points along Z. */
  WALL_Z: 2,
  /** No texture. Vertex colour only. */
  SOLID: 3
};

/**
 * Untextured surfaces.
 *
 * `roughness` and `metallic` here are the physically right values, not taste:
 * a painted metal car body is metallic 0 because the paint is an oxide, glass is
 * metallic 0 and roughness 0.05, and bare galvanised steel is metallic 1. Getting
 * this wrong is the most common way a PBR scene ends up looking like plastic.
 */
export const SOLID_MATERIALS = [
  { id: 'glass', roughness: 0.06, metallic: 0.0, tint: [0.55, 0.68, 0.75], note: 'Window and windscreen glass. Dark by default because a window is a hole, not a light.' },
  { id: 'glass_lit', roughness: 0.10, metallic: 0.0, tint: [0.95, 0.82, 0.55], emissive: 1.6, note: 'Interior light behind glass, driven up at night.' },
  { id: 'paint_metal', roughness: 0.28, metallic: 0.0, tint: [0.5, 0.5, 0.52], note: 'Painted steel: poles, shutters, railings, containers. Paint is a dielectric.' },
  { id: 'metal_bare', roughness: 0.34, metallic: 1.0, tint: [0.62, 0.64, 0.67], note: 'Galvanised or unpainted steel. The one place the metal path is driven by a constant rather than a texture.' },
  { id: 'metal_dark', roughness: 0.45, metallic: 0.9, tint: [0.22, 0.23, 0.25], note: 'Dark structural steel: frames, brackets, exhausts.' },
  { id: 'rubber', roughness: 0.85, metallic: 0.0, tint: [0.09, 0.09, 0.10], note: 'Tyres, seals, mats.' },
  { id: 'road_paint', roughness: 0.55, metallic: 0.0, tint: [0.88, 0.86, 0.78], note: 'Lane and crossing markings. Worn, not white — a fresh white line is the oldest tell of a generated road.' },
  { id: 'road_paint_yellow', roughness: 0.55, metallic: 0.0, tint: [0.82, 0.68, 0.18], note: 'Bus stop boxes, loading bays, temporary works.' },
  { id: 'foliage', roughness: 0.62, metallic: 0.0, tint: [0.42, 0.58, 0.26], note: 'Leaves and canopy. Two-sided, and never specular, which is why it is its own material.' },
  { id: 'foliage_dry', roughness: 0.70, metallic: 0.0, tint: [0.56, 0.52, 0.30], note: 'Dead and autumn vegetation.' },
  { id: 'bark', roughness: 0.88, metallic: 0.0, tint: [0.30, 0.24, 0.18], note: 'Trunks and branches.' },
  { id: 'skin', roughness: 0.55, metallic: 0.0, tint: [0.72, 0.55, 0.44], note: 'Character skin. Slightly waxy, never shiny.' },
  { id: 'fabric', roughness: 0.82, metallic: 0.0, tint: [0.5, 0.5, 0.5], note: 'Clothing. Fully rough, which is the whole point.' },
  { id: 'lamp', roughness: 0.15, metallic: 0.0, tint: [1.0, 0.94, 0.80], emissive: 3.0, note: 'Lit lamp glass and bulb.' },
  { id: 'sign', roughness: 0.35, metallic: 0.0, tint: [0.85, 0.30, 0.22], emissive: 0.9, note: 'Illuminated signage.' },
  { id: 'plastic', roughness: 0.40, metallic: 0.0, tint: [0.6, 0.6, 0.6], note: 'Crates, bins, casings.' },
  { id: 'tarpaulin', roughness: 0.75, metallic: 0.0, tint: [0.35, 0.40, 0.35], note: 'Sheeting, covers, awnings.' }
];

/** Maximum material count the shader's uniform arrays are sized for. */
export const MAX_MATERIALS = 48;

/**
 * Merge the baked texture materials with the solid ones into the single table
 * the renderer uploads.
 *
 * @param {object} baked The `MATERIAL_DESCRIPTOR` the bake emits.
 * @returns {{count:number, byId:Record<string,number>, a:Float32Array, b:Float32Array, layers:number, size:number}}
 */
export function buildMaterialTable(baked) {
  const byId = {};
  const entries = [];

  for (const [id, m] of Object.entries(baked.materials)) {
    byId[id] = entries.length;
    entries.push({
      id, textured: true, layer: m.layer,
      invTileScale: 1 / Math.max(0.05, m.tileScale),
      roughness: m.roughness, metallic: m.metallic,
      // A road is a ground surface; a wall is a wall. The bake knows the role,
      // and the role is exactly what the projection plane should be.
      mapMode: GROUND_FOR_ROLE(m.role),
      normalStrength: 1.0
    });
  }

  for (const m of SOLID_MATERIALS) {
    byId[m.id] = entries.length;
    entries.push({
      id: m.id, textured: false, layer: -1, invTileScale: 0,
      roughness: m.roughness, metallic: m.metallic,
      mapMode: MAP_MODE.SOLID, normalStrength: 0
    });
  }

  if (entries.length > MAX_MATERIALS) {
    throw new Error(`material table overflow: ${entries.length} > ${MAX_MATERIALS}`);
  }

  // Emissive strength, by material id, built once. Doing a linear search of
  // SOLID_MATERIALS per table entry is fine at build time and is still wrong the
  // moment the table is built per frame.
  const emissiveById = new Map(SOLID_MATERIALS.map(s => [s.id, s.emissive || 0]));

  // Two vec4s per material. A uniform array of vec4s rather than a texture,
  // because a material table is a few hundred bytes and a data texture would
  // cost a sampler and a cache line to read the same thing.
  const a = new Float32Array(MAX_MATERIALS * 4);
  const b = new Float32Array(MAX_MATERIALS * 4);
  entries.forEach((e, i) => {
    a[i * 4 + 0] = e.layer;
    a[i * 4 + 1] = e.invTileScale;
    a[i * 4 + 2] = e.roughness;
    a[i * 4 + 3] = e.metallic;
    b[i * 4 + 0] = e.mapMode;
    b[i * 4 + 1] = e.normalStrength;
    b[i * 4 + 2] = emissiveById.get(e.id) || 0;
    b[i * 4 + 3] = e.textured ? 1 : 0;
  });

  return {
    count: entries.length,
    byId,
    a, b,
    layers: baked.arrays.albedo.layers,
    size: baked.size
  };
}

/**
 * Which plane a textured material is projected onto.
 *
 * Derived from the role the database recorded rather than guessed per surface,
 * so a road always tiles as ground even if something later places it on a
 * slope-facing quad.
 */
function GROUND_FOR_ROLE(role) {
  if (/road|pavement|street|shoulder|ground|terrain|beach|hardstanding|plaza/.test(role)) return MAP_MODE.GROUND;
  return MAP_MODE.WALL_X;
}

/**
 * Look up a material index, failing loudly.
 *
 * An undefined material would render as the default solid, which looks almost
 * right — so a typo here has to be an error rather than a silent fallback.
 */
export function materialIndex(table, id) {
  const i = table.byId[id];
  if (i === undefined) {
    throw new Error(`unknown material '${id}'. Known: ${Object.keys(table.byId).join(', ')}`);
  }
  return i;
}
