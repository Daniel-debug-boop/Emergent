/**
 * The EMERGENT asset database.
 *
 * This is the single source of truth for every third-party asset the game
 * ships. Nothing enters `assets/` or `dist/` that is not named here, and
 * `tools/assets/audit.mjs` fails the build if a manifest entry is missing a
 * licence, a source URL, or a runtime path — or if a file on disk is not in
 * the manifest.
 *
 * ## Why Poly Haven and nothing else
 *
 * Poly Haven publishes everything under CC0, which is the only licence that
 * makes "the cooked assets ship inside a commercial game" a question with an
 * unambiguous answer. Every entry below carries `CC0` and a licence URL that
 * says so, and the fetch tool refuses anything else.
 *
 * ambientCG was the intended second source and is unreachable from the build
 * environment (TLS handshake fails, curl 35), so nothing depends on it. Quaternius
 * and Kenney were not needed: Poly Haven's model library covers the categories
 * EMERGENT actually uses, at a higher fidelity, under the same licence.
 *
 * ## The three maps per material
 *
 * Poly Haven publishes each material as a set. EMERGENT needs exactly three, and
 * each is a *packed* map rather than three separate greyscales:
 *
 *   diff    sRGB base colour
 *   nor_gl  tangent-space normal, OpenGL (+Y up) convention
 *   arm     R = ambient occlusion, G = roughness, B = metallic
 *
 * `arm` is why this pipeline is three downloads per material instead of five.
 *
 * ## Quality tiers
 *
 * `hero` materials are the ones a player stands next to — the road under their
 * feet, the wall in front of them. They are downloaded at a higher source
 * resolution than `standard` props, and `minor` materials are only ever seen at
 * a distance or inside a building. That is what stops a 30-material set from
 * costing 30 hero downloads.
 */

export const LICENSE = {
  id: 'CC0',
  name: 'CC0 1.0 Universal (Public Domain Dedication)',
  url: 'https://polyhaven.com/license',
  redistributable: true,
  attribution_required: false
};

export const PROVIDER = {
  id: 'polyhaven',
  name: 'Poly Haven',
  url: 'https://polyhaven.com',
  api: 'https://api.polyhaven.com',
  files: 'https://dl.polyhaven.org/file/ph-assets'
};

/**
 * The curated material set.
 *
 * Every entry was chosen against the criteria in the asset directive: it has to
 * look like it belongs in a modern city, it has to tile, it has to tile
 * *again* (seamless matters more than resolution for a road), and it has to
 * survive being seen at 4 metres and at 400 metres. Materials that are
 * beautiful but tile badly, or that duplicate another entry's job, are not
 * here.
 *
 * `scale` is the world-space size of one texture tile in metres. It is the
 * single most important number for believability: asphalt at 4 m reads as a
 * road, asphalt at 40 m reads as a grey plane, and the UV scale multiplies it
 * per surface so a building wall and a park path use the same texture at
 * different densities.
 */
export const MATERIALS = [
  // ---------------------------------------------------------------- roads --
  {
    id: 'road_asphalt', polyhaven: 'asphalt_02', tier: 'hero', scale: 5.0,
    role: 'road surface', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The road the player stands on. Tightly grained and seamless, which is what a road needs to be; anything with visible large-scale blotching tiles into patches at speed.'
  },
  {
    id: 'road_asphalt_worn', polyhaven: 'worn_asphalt', tier: 'standard', scale: 5.0,
    role: 'worn road surface, patched roads', roughness: 1.05, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Secondary streets and repair patches. Kept deliberately so the world can have more than one road: a city where every road is identical is the clearest tell of a generated prototype.'
  },
  {
    id: 'pavement_concrete', polyhaven: 'concrete_pavement_02', tier: 'standard', scale: 2.5,
    role: 'sidewalk, plaza paving', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Sidewalks. Half the ground a player sees in a city is pavement, and it must not be the road texture at a different size.'
  },
  {
    id: 'pavement_pavers', polyhaven: 'concrete_pavers_02', tier: 'standard', scale: 2.0,
    role: 'plaza, courtyard, old street paving', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Plazas and courtyards. A visible joint pattern gives the ground a readable scale, which a flat texture cannot.'
  },
  {
    id: 'street_cobble', polyhaven: 'cobblestone_03', tier: 'standard', scale: 2.5,
    role: 'old district street', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The historic district. Strong normal detail, so it reads as cobble under raking light even at a distance.'
  },
  {
    id: 'ground_gravel', polyhaven: 'bicolour_gravel', tier: 'standard', scale: 1.6,
    role: 'road shoulder, yards, hardstanding', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Shoulders and yards. Breaks the hard line where asphalt meets terrain, which is otherwise a visible seam.'
  },

  // -------------------------------------------------------------- facades --
  {
    id: 'wall_brick', polyhaven: 'brick_wall_006', tier: 'hero', scale: 2.2,
    role: 'brick facade', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The archetypal city facade. A strong, regular bond pattern is what makes a box read as a building rather than a block.'
  },
  {
    id: 'wall_brick_dark', polyhaven: 'brick_wall_10', tier: 'standard', scale: 2.2,
    role: 'dark brick, soot-stained brick', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Variety for the brick family. One brick texture across a whole district is exactly the repetition the visual acceptance test is looking for.'
  },
  {
    id: 'wall_plaster', polyhaven: 'white_plaster_02', tier: 'standard', scale: 2.6,
    role: 'rendered plaster facade', roughness: 0.95, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Stucco/render construction. Common on older residential blocks and very different in silhouette from brick, which is why it earns a slot.'
  },
  {
    id: 'wall_plaster_dirty', polyhaven: 'plastered_wall_03', tier: 'standard', scale: 2.6,
    role: 'weathered plaster, back alley', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The service side of buildings. Grime is most of what makes a city look lived in rather than modelled.'
  },
  {
    id: 'wall_concrete', polyhaven: 'concrete_wall_005', tier: 'standard', scale: 3.0,
    role: 'concrete panel facade, retaining walls', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Modern construction and civil engineering. Poured-concrete surfaces are the other half of a city after brick.'
  },
  {
    id: 'wall_painted_concrete', polyhaven: 'painted_concrete', tier: 'standard', scale: 3.0,
    role: 'painted render, coloured facade', roughness: 0.9, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Painted render. Gives the facade palette somewhere to go other than brick red, and its roughness reads as paint.'
  },
  {
    id: 'wall_stone', polyhaven: 'sandstone_blocks_05', tier: 'standard', scale: 2.4,
    role: 'stone cladding, institutional buildings', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Civic and institutional architecture. Chosen by measurement over stone_tile_wall, which looked better but has a wrap seam 10.2x its interior difference — a visible grid across every stone wall in the game.'
  },
  {
    id: 'wall_siding', polyhaven: 'wood_planks_grey', tier: 'standard', scale: 2.0,
    role: 'weathered timber siding', roughness: 0.95, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Low-rise residential and warehouse cladding. Weathered grey, because the only large wood surface on an exterior should not be new. Replaces wood_plank_wall, which measured a seam ratio of 7.8.'
  },
  {
    id: 'wall_metal_shutter', polyhaven: 'painted_metal_shutter', tier: 'standard', scale: 2.4,
    role: 'roller shutter, industrial cladding', roughness: 0.75, metallic: 0.0, expectedMetal: [0, 0.1],
    why: 'Shutters and industrial facades. The ribbed normal is what sells it as metal. Metallic is forced to 0 on purpose: paint is a dielectric oxide over the steel, and treating a painted shutter as bare metal is the most common PBR mistake in a city scene.'
  },
  {
    id: 'wall_corrugated', polyhaven: 'rusty_corrugated_iron', tier: 'standard', scale: 2.4,
    role: 'corrugated iron, warehouse, shed', roughness: 0.8, metallic: 0.2, expectedMetal: [0, 0.1],
    why: 'Sheds, warehouses and containers. The strongest normal map in the set, and the corrugation silhouette is what makes a shed look like a shed. Metalness is 0.2 rather than 0.7 because the surface is rust.'
  },
  {
    id: 'metal_rust', polyhaven: 'rusty_metal_02', tier: 'standard', scale: 1.4,
    role: 'rusted steel, exposed structure', roughness: 0.85, metallic: 0.25, expectedMetal: [0, 0.1],
    why: 'Rails, gantries, plant and anything derelict. Rust is the cheapest realism per triangle available in a city. Its measured metalness is 0.003, so a 0.25 override stands in for the bare steel showing through the oxide.'
  },
  {
    id: 'metal_plate', polyhaven: 'metal_plate_02', tier: 'minor', scale: 1.6,
    role: 'steel plate, containers, machinery', roughness: 0.6, metallic: null, expectedMetal: [0.5, 1.0],
    why: 'Clean steel for containers and machinery. The one material whose metalness is read straight from the texture (0.91 measured), which makes it the shader\'s real metal path rather than a constant.'
  },

  // ------------------------------------------------------------- terrain --
  {
    id: 'terrain_grass', polyhaven: 'grass_ground', tier: 'hero', scale: 9.0,
    role: 'grass, parkland', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The single largest surface in the world. Low tiling scale is deliberate: grass needs macro variation more than it needs detail, and at 2 m it would look like a lawn.'
  },
  {
    id: 'terrain_dirt', polyhaven: 'dirt', tier: 'standard', scale: 6.0,
    role: 'bare earth, worn ground, verges', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Where grass gives way to nothing. The transition between grass and dirt is most of what makes terrain look like ground.'
  },
  {
    id: 'terrain_mud', polyhaven: 'mud_forest', tier: 'standard', scale: 5.0,
    role: 'mud, wet ground, construction ground', roughness: 0.75, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Wet ground. The lower roughness is the point: mud is the only large surface in the set that is genuinely reflective.'
  },
  {
    id: 'terrain_rock', polyhaven: 'rock_ground_02', tier: 'standard', scale: 7.0,
    role: 'rocky ground, scree, cliff foot', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Exposed rock where the ground steepens. Also the slope-blend partner for cliffs.'
  },
  {
    id: 'terrain_sand', polyhaven: 'sand_02', tier: 'minor', scale: 6.0,
    role: 'beach, dry verge, construction sand', roughness: 1.0, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Beaches and the one dry material. Minor tier: it is a small fraction of any given world.'
  },

  // ------------------------------------------------------------ interior --
  {
    id: 'floor_concrete', polyhaven: 'concrete_floor_01', tier: 'minor', scale: 2.4,
    role: 'warehouse and shop floor', roughness: 0.9, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Interior floor. Minor tier: only ever seen through a doorway, and always at close range under interior light.'
  },
  {
    id: 'floor_wood', polyhaven: 'plank_flooring', tier: 'minor', scale: 1.6,
    role: 'residential and cafe floor', roughness: 0.7, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'Warmth indoors. Its measured roughness of 0.38 is what separates a varnished floor from a brown box. Replaces wood_planks, which measured a seam ratio of 8.3.'
  },
  {
    id: 'floor_tile', polyhaven: 'large_grey_tiles', tier: 'minor', scale: 1.2,
    role: 'commercial floor, bathroom, kitchen', roughness: 0.45, metallic: 0.0, expectedMetal: [0, 0.15],
    why: 'The glossiest large surface in the set. Ceramic is the one interior material that should visibly reflect a light source.'
  }
];

/** Material id to definition, for the pipeline and the game. */
export const MATERIAL_BY_ID = new Map(MATERIALS.map(m => [m.id, m]));

/**
 * Source download resolution, by tier.
 *
 * The *runtime* texture is a single array for every material — see
 * `TEXTURE_SIZE` — so the tier does not control the shipped resolution. It
 * controls the resolution EMERGENT downloads and bakes *from*, which is where
 * the real quality is: a material sampled at 8k and box-filtered down keeps
 * far more micro-detail than one upsampled from 1k, and it is only a handful of
 * materials that are ever seen from a metre away.
 *
 * 8k is what the provider publishes for nearly every material here, so these
 * are the top of the available range rather than an arbitrary choice. The cost
 * is download time and a few hundred megabytes of gitignored source, both of
 * which are paid once at `npm run assets:fetch` and never again.
 */
export const TIER_SOURCE_RESOLUTION = { hero: '8k', standard: '4k', minor: '2k' };

/**
 * Runtime texture size, for every layer of every array.
 *
 * A `TEXTURE_2D_ARRAY` requires all its layers to be the same size, so a
 * per-material runtime resolution is not available without a separate array and
 * a second set of samplers per map.
 *
 * 1024, chosen by measurement rather than by wanting a bigger number. Three
 * maps times 26 layers:
 *
 *     512 px ->  105 MB VRAM   1/8 the texels of a 1080p screen at 3 m
 *    1024 px ->  418 MB VRAM   about half a screen at 3 m
 *    2048 px -> 1672 MB VRAM   4x the texels of a 1080p screen at 3 m
 *
 * 2048 and above spend gigabytes on detail that no viewpoint in this game
 * resolves: at 3 m a 1024 tile is already about half the screen, and at 10 m it
 * is a twentieth of it. 1024 is the last size where adding a texel changes
 * something a player can see, and it quadruples the detail of the previous
 * 512 bake on the surfaces that are seen from a metre away.
 *
 * Detail beyond that is bought with tiling density -- `scale` in each material's
 * entry -- rather than with more bytes per texel, which is free where this is
 * not.
 */
export const TEXTURE_SIZE = 1024;

/** Internal format for each array, and whether it is decoded as sRGB. */
export const TEXTURE_ARRAYS = [
  { key: 'albedo', map: 'diff', internalFormat: 'SRGB8_ALPHA8', format: 'RGBA', type: 'UNSIGNED_BYTE', srgb: true },
  { key: 'normal', map: 'normal', internalFormat: 'RGBA8', format: 'RGBA', type: 'UNSIGNED_BYTE', srgb: false },
  { key: 'arm', map: 'arm', internalFormat: 'RGBA8', format: 'RGBA', type: 'UNSIGNED_BYTE', srgb: false }
];

/** The three maps every material needs, and what each one is for. */
export const MAPS = [
  { key: 'diff', polyhaven: 'Diffuse', srgb: true, channels: 'rgb', gl: 'RGBA', label: 'base colour' },
  { key: 'normal', polyhaven: 'nor_gl', srgb: false, channels: 'rgb', gl: 'RGBA', label: 'tangent-space normal (OpenGL +Y)' },
  { key: 'arm', polyhaven: 'arm', srgb: false, channels: 'rgb', gl: 'RGBA', label: 'R=ambient occlusion G=roughness B=metallic' }
];

/**
 * Where a material's cooked tile ends up.
 *
 * `assets/textures/` in the repository, not `dist/`. The game imports the
 * generated descriptor as a module, so the baked output has to exist before
 * anything imports `game3d.js` — including the test suite on a machine with no
 * network. Committing the cooked assets is what makes the build reproducible
 * and the tests hermetic; the 80 MB of 2k sources they came from stay out of
 * git and are recreated with `npm run assets:fetch`.
 */
export const RUNTIME_DIR = 'assets/textures';
