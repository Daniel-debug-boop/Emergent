# The EMERGENT asset pipeline

Every third-party asset in EMERGENT passes through this pipeline, and nothing
that has not been through it ships. This document is the reference: what is
acquired, from where, under what licence, how it is processed, what it costs,
and which decisions were deliberate rather than accidental.

## The short version

| | |
|---|---|
| Source | Poly Haven, CC0 1.0 |
| Materials | 26, hand-curated in `assets/database.mjs` |
| Maps per material | 3 — base colour, tangent-space normal, packed AO/roughness/metalness |
| Baked size | 512 × 512 per layer |
| Arrays | 3 × `TEXTURE_2D_ARRAY`, 26 layers each, 10 mip levels |
| Shipped | 17.4 MB of PNG across 78 tiles |
| VRAM at runtime | 104 MB with complete mip chains |
| Re-acquisition | 80 MB of 2k/1k JPEG sources, recreated by `npm run assets:fetch` |

Everything below is measured on the build machine, not estimated.

## The stages

```
assets/database.mjs     the curated selection, with a reason for each entry
        |
        v
tools/assets/fetch.mjs  validate against the provider API, download, md5-verify,
        |                record source URL / creator / licence / date
        v
assets/source/          the raw downloads, gitignored, never modified
        |
        v
tools/assets/bake.mjs   validate channels and tileability, resample, pack,
        |                emit assets/textures/*.png and materials.mjs
        v
assets/textures/        the committed runtime set
        |
        v
tools/build.mjs         copied into dist/ with the code
        |
        v
the game                three texture arrays, one material index per vertex
```

`tools/assets/audit.mjs` closes the loop: it fails if any material loses its
licence, its provenance, or stops matching what actually ships.

## Why the baked output is committed

The game imports `assets/textures/materials.mjs` as a static ES module, which
means it has to exist before anything imports `game3d.js` — including the test
suite, on a machine with no network. Committing the cooked assets is what makes
the build reproducible and the tests hermetic. The 80 MB of 2k sources they were
derived from stay out of git and are recreated with one command.

## Why texture arrays and not an atlas

A 2D atlas needs a border-bleed margin around every tile to stop mip filtering
bleeding one material into its neighbour, and the margin has to grow as the mip
level shrinks. A 512-px tile in a 4096-px atlas therefore effectively costs
576-px of texture — about 30% wasted memory at mip 0 and worse below it.

A `TEXTURE_2D_ARRAY` has no such problem: array layers are sampled
independently, so mip filtering never crosses a tile boundary and no padding is
needed at any level. WebGL2 guarantees `sampler2DArray`, so this costs nothing
in compatibility.

## Why 512 px, and why all layers are the same size

A texture array requires all its layers to be the same size, so a per-material
runtime resolution is not available without a separate array and a second set
of samplers per map. 512 is the point where the road under the player's feet
holds up at a 5 m tile while 26 materials still fit a browser-sized budget.

Detail that genuinely needs more than 512 texels is bought with **tiling
density** — the `scale` field in the database, which is the number of world
metres one tile covers — rather than with more bytes per texel. Asphalt at 4 m
reads as a road; asphalt at 40 m reads as a grey plane.

What the *tier* does control is the **source** resolution the bake downloads
from, which is where the real cost is: a hero material sampled at 2k and
box-filtered down to 512 keeps far more micro-detail than one upsampled from 1k,
and only four materials in the set are seen from a metre away.

## Why RGBA8 and not KTX2 / Basis Universal

This is the one place the pipeline does not do what a modern asset pipeline
usually does, so here is the reasoning rather than a silent gap.

A browser cannot upload BCn or ASTC without either a `compressedTexImage2D`
extension for that exact format or a WASM transcoder, and WebGL2 guarantees
neither. Encoding BCn offline needs KTX-Software's `toktx`, which is not present
in this build environment and cannot be installed from it. Shipping a ~500 KB
Basis transcoder into the game's first frame to save download bytes that a
512-px PNG does not cost is the wrong trade for a web target.

So the runtime is RGBA8 with GPU-generated mip chains. The moment a build host
with `toktx` exists, this is a one-line change to the encoder in
`tools/assets/bake.mjs`, not a redesign: the validation, the atlas packing, the
layer assignment and the loader all work in terms of "one 512×512 RGBA tile per
layer" and do not care how those bytes are encoded.

The cost of the decision, measured: 17.4 MB of download and 104 MB of VRAM.
The cost of the alternative, estimated: a 500 KB transcoder plus whatever the
compressed textures save, which at this size is likely a few megabytes. A reader
who disagrees with the trade can make it in one place.

## Validation, and what each check is for

The bake refuses to emit a material that fails any of these. None of them is
decoration; each caught a real problem in this set.

| Check | Catches |
|---|---|
| Albedo luma and standard deviation in a band | A failed download or a flat placeholder. md5 cannot catch these, because the provider published them. |
| Seam ratio: wrap-edge difference vs interior difference | A texture that tiles visibly. Measured per axis, because a texture that tiles left-to-right often does not tile top-to-bottom. |
| Blue channel dominates the normal map | A DirectX-convention normal, which lights the entire world inside out. |
| Metalness within a declared per-material range | A provider-side ARM channel swap. Roughness landing in blue makes every dielectric surface shiny. |
| Ambient occlusion above a floor | A material that would render black. |
| Every layer of every array filled | A hole in a texture array, which samples as transparent black. |

### The seam check, and what it changed

Poly Haven publishes most textures as tileable, but not all of them, and the
ones that are not are the expensive mistake: a visible grid across a road or a
wall reads as broken, and it reads as broken across the whole surface rather
than in one corner.

Three materials in the first cut were rejected by it and replaced:
`stone_tile_wall` (seam 10.2×), `wood_plank_wall` (7.8×) and `wood_planks`
(8.3×), replaced by `sandstone_blocks_05`, `wood_planks_grey` and
`plank_flooring`, which measure 0.9×, 2.8× and 0.7×. The selection tool
`tools/assets/probe.mjs` measures candidates at the resolution the bake produces
rather than at a smaller one, because an earlier version sampled at 256 and
recommended textures that turned out to seam at 512.

### The metalness correction

The first cut declared `metallic: 0.7` for rusty corrugated iron and `0.6` for
rusted steel. The ARM maps measure **0.001** and **0.003** in blue for both, and
they are right: rust is an oxide, and an oxide is a dielectric. The database was
wrong, not the textures. Those materials now carry explicit overrides
(0.2 and 0.25) and every material declares the band its texture must measure,
so the check is a real assertion rather than a restatement of intent.

Painted metal is the same story in the other direction: `paint_metal` — every
shutter, pole and railing in the city — is forced to metalness 0, because paint
is a dielectric and treating a painted shutter as bare metal is the most common
PBR mistake in a city scene.

## The material system at runtime

`materials.mjs` merges the 26 baked materials with 17 untextured ones into a
single index space of 43, uploaded as two `vec4` uniform arrays.

The untextured entries are not a fallback. A car body is a painted surface, and
a painted surface is a dielectric with a roughness — giving it a photograph adds
bytes and buys nothing. Window glass, lamp glass, rubber, foliage, bark, skin,
fabric and road markings all live in the same table, and the shader has no
second code path for them.

### How texture coordinates are derived, and why there is no UV attribute

Static world geometry is axis-aligned, so a surface's texture coordinate is a
projection of its world position onto the plane its normal points out of,
scaled by the material's tiling density. The vertex shader picks the plane from
the normal. This is box mapping, and it is right here for three reasons: it has
no seams, it needs no unwrapping, and it is *world locked*, so a brick wall's
bricks stay where they are while the player walks past instead of shimmering.

It is also wrong for anything that moves. A car at 20 m/s under a world-locked
projection slides its paint across its own body, which is much worse than having
no texture. So moving geometry — cars, characters — uses solid materials and
vertex colour. That is why the vertex layout is ten floats and not twelve:

```
position(3)  normal(3)  colour(3)  material(1)   =  40 bytes
```

## The geometry kit

`geometry.mjs` is every triangle in the world: boxes, tapers, cylinders, cones,
canopy blobs, window units with reveals and sills, doors with awnings, cornices,
parapets with copings, gables, railings, balconies, fire escapes, street lamps,
traffic lights, bus shelters, containers, pallets, crates and more.

`city.mjs` composes them into buildings, roads, trees and pavement dressing,
with a detail budget that falls off with distance.

The rule the whole thing is built around: **a box with a brick texture is still
a box.** What makes architecture read as architecture is not its wall texture,
it is the shadow line under a window sill, the projection of a cornice, the
reveal of a doorway, the parapet's coping, plant on the roof, a different
material at every level. Those are separate small solids, and they are cheap.

### LOD, measured

Detail falls off over a *detail radius* that is deliberately shorter than the
streaming radius — 260 m to 800 m by quality, against 680 m to 1160 m streamed.
Streaming exists so the world continues past the horizon; the detail radius
exists so a building the player can read has its windows, cornice and roof
plant, and one 800 m away is a correctly materialled mass with the right
silhouette.

Agents have their own, much tighter, visual LOD: a character drops hand and hair
past 70 m and becomes three boxes past 260 m; a car drops its wheel hubs past
160 m and becomes two boxes past 520 m.

The measured effect, same scene, same machine, before and after:

| | naive detail | with LOD |
|---|---|---|
| Static vertices | 1,420,680 | **218,226** |
| Static buffer | 54.2 MB | **8.3 MB** |
| Static build | 668 ms | **99 ms** |
| Dynamic vertices | 36,228 | **11,202** |
| Dynamic build | 10.11 ms | **2.79 ms** |

Both static figures are 6.5× reductions with no loss of detail where the player
is standing.

## What the headless harness can and cannot verify

The harness in `tools/headless_runtime.mjs` models textures, texture arrays,
mipmap generation and sampler state, and it validates them: array layers in
range, internal format against the `(format, type)` pair, a mipmap filter
without a mip chain reported as the incomplete texture it is, and empty array
layers reported by name.

What it cannot do is produce pixels. So the claims this document can support
are about the *contract* — sizes, layer counts, mip completeness, material
index validity, geometry counts, build times — and not about how the result
looks. `test_headless_game.mjs` asserts all of the former; the latter is marked
**FINAL HARDWARE VALIDATION REQUIRED** below.

## Reproducing everything

```bash
npm run assets:fetch     # re-download the 80 MB of sources, md5-verified
npm run assets:bake      # validate, resample and pack the runtime set
npm run assets:audit     # licence, provenance and shipped-file audit
npm run assets:probe -- sandstone_blocks_05   # measure a candidate
```

`assets:fetch` needs network access. `assets:bake` and `assets:audit` do not.

## FINAL HARDWARE VALIDATION REQUIRED

Not measurable in this environment, and therefore not claimed anywhere:

- Frame rate, frame time and 1% / 0.1% lows
- GPU frame time versus CPU render-submission time
- VRAM residency against the 104 MB estimate
- Texture upload time and its effect on the first seconds of play
- Anisotropic filtering, which the samplers leave at the driver default
- Shader compilation time and any driver-specific GLSL complaints
- Thermal behaviour under sustained load

## Known limitations, stated plainly

- **No interiors.** Buildings are shells. The directive asks for rooms, doors,
  furniture and interior lighting; what ships is exterior architecture with
  window openings, ground-floor shopfronts and a small set of interior floor
  materials in the palette, ready for a room system to use. There is no
  interior.
- **No downloaded models.** Every mesh is procedural. Poly Haven's 521-model
  library was surveyed and deliberately not used: a city needs tens of
  thousands of instances, and glTF import plus a per-model LOD and collision
  pipeline is a larger piece of work than making the procedural city
  convincing, which is the higher-value half of the objective. The 17
  untextured materials cover the surfaces those models would need.
- **No decal system.** Surface variation comes from materials and vertex tint
  rather than from projected decals, so a wall cannot have graffiti on it.
- **No normal-mapped detail on the ground.** Terrain uses planar projection,
  which tiles well but has no triplanar blend, so a steep slope stretches its
  texture.
- **No vegetation impostors.** Trees swap to a lower-poly version, not a
  billboard.
- **ambientCG is unreachable** from this environment (TLS handshake failure,
  curl 35), so the intended second material source is unused. Poly Haven's
  862-texture library covered every category without it.
