# CHANGELOG

## 2026-09-27 — the native renderer stops being a debug box viewer

The Vulkan backend drew `SceneBox`es: unit cubes, one per body, with a colour.
It was honest about that — the shader said so in a comment — but it meant the
native engine was not a renderer for this game, it was a viewer for one, and
every screenshot of it was of test data rather than of EMERGENT. The world,
the materials and the geometry now exist in C++ and the renderer consumes the
same concepts the browser does.

**The world is generated in C++, and it is provably the same world.**
`world_gen.cpp` is a port of `world.mjs`, and the interesting part is not that
it exists but that it is *exact*. Three properties make that possible: every
hash is an integer operation (`Math.imul` maps to explicit 32-bit wrap rather
than signed overflow, which is undefined behaviour and the optimiser is
entitled to exploit it); the one float function involved, `hash2`, is a `sin`
and is therefore quarantined in scattering, with terrain using an integer
lattice hash exactly as the JavaScript already does; and the xorshift
generator is the same 13/17/5 triple with the same wrap.

`emergent_world_parity_test` checks that against a fixture generated from
`world.mjs` and committed. **4,699 checks, 0 failures** — the same 3,077
buildings, 672 roads, 50 districts, 1,961 trees and 707 businesses, with every
sampled position agreeing to a micrometre. CI regenerates the fixture and fails
if it has gone stale, so a change to the JavaScript world cannot silently leave
the native one describing a city that no longer exists.

**Four real bugs the parity test caught**, none of which a code review finds:

- `occupied` held `&w.buildings.back()`. `w.buildings` is a vector and
  reallocates, so every stored pointer was dangling from the next push. The
  JavaScript holds object references and can never have this bug. It was a
  use-after-free a few hundred buildings in, caught by AddressSanitizer, and
  the test now runs clean.
- The building loop's storey count is a ternary *followed by a separate `if`*
  in the JavaScript, so an industrial district draws twice and throws the first
  away. Collapsing it into `else if` consumed 1,174 fewer draws across the
  city, which shifted every subsequent random and changed the tree count.
- `rand() < 0.17 ? 'pine' : rand() < 0.12 ? ...` draws twice when the first
  comparison fails. Reading it as one draw looked equivalent and was not.
- The business record invented three colour draws that do not exist in the
  reference and padded the rest — 3,682 extra draws, and 722 businesses instead
  of 707.

**Materials, in C++.** `materials.hpp`/`materials.cpp` are the same table the
browser has: 26 baked materials plus 17 solid ones in one index space, two vec4
uniform arrays, the same `MapMode` branches. The load-bearing values are tested
directly — painted steel is metallic 0 because paint is an oxide, bare
galvanised steel is metallic 1, and getting that backwards is the most common
way a PBR scene ends up looking like plastic.

**Geometry, in C++.** `geometry.hpp`/`geometry.cpp` are the detail kit:
windows with reveals, frames and projecting sills; doors with steps; parapets;
railings; roof plant; 8 m street lamps with arms over the carriageway; traffic
lights; shipping containers at their real 6.06 x 2.44 x 2.59 m. Every emitter
writes through one `MeshBuilder::vertex`, which aborts on a colour outside
0..1 or a material index the table does not cover — a material index passed into
a colour slot produces *finite* vertices that pass every NaN scan and draw as
garbage, and that happened twice in the JavaScript kit before the choke point
existed.

`emergent_material_geometry_test` is **142 checks** with no GPU: every normal is
unit length (an unnormalised cone normal is not a crash, it is a surface lit at
up to 2.2x too bright), every material index resolves, and the proportions are
checked against the only scale a player can verify — a 1.8 m person.

**The shader is the same shader.** `scene_pbr.vert`/`scene_pbr.frag` are GLSL
450 versions of the browser's PBR pair: the same GGX + Smith microfacet BRDF,
the same dielectric/metal reflectance mix, the same hemispherical ambient, the
same box mapping with a `MapMode::Uv` escape for imported meshes, and the same
derivative-based tangent frame. Two shaders for one game is how the two halves
quietly stop being the same game.

**Textures come from 8k sources now.** `TIER_SOURCE_RESOLUTION` is 8k/4k/2k by
tier, up from 2k/1k/1k, and the runtime tile is 1024 rather than 512 — four
times the detail, 418 MB of VRAM with a complete mip chain, 70 MB on disk across
78 tiles. 1024 is the last size where another texel changes anything: at 3 m a
1024 tile is about half a 1080p screen, and 2048 would cost 1.6 GB for detail no
viewpoint in this game resolves. Detail past that is bought with tiling density,
which is free.

**A harness bug that was hiding a real one.** The headless runtime reported
every image as 512x512, so raising the bake size to 1024 was invisible to it and
surfaced only as the game's own decode check failing with "decoded 512x512, the
descriptor says 1024x1024" on all 78 tiles — an error naming the harness's lie
as the cause. The harness now reads the real dimensions from each PNG's IHDR
chunk. The same class of bug hid in `test_headless_game.mjs`, which had its own
copy of the 512 literal; it now reads the descriptor the game itself imports.

**And a build that did not link.** `emergent_frame_loop` named
`emergent_animation` unconditionally, but that target only exists when an ozz
tree is supplied — so any configuration without one failed with a bare
`-lemergent_animation: No such file`, which is exactly the configuration whose
whole purpose is to build the parts that need no third-party tree.

**Verification, all run here:**

| | result |
|---|---|
| `emergent_world_parity` | 4,699 checks, 0 failures |
| `emergent_material_geometry` | 142 checks, 0 failures |
| native `ctest` | 7/7 |
| `npm test` | world 55, math 71, missions 445, geometry 34,888, shaders 111, shading 7,831, physics 71, input 189, tooling 7, runtime 15/15 at 20,937 — 0 failures |
| `npm run assets:audit` | every asset CC0, sourced, referenced, present |

**What is still not true.** The Vulkan render path has *still* never executed:
there is no ICD and no `/dev/dri` here, so `vkCreateInstance` correctly returns
`VK_ERROR_INCOMPATIBLE_DRIVER` and the backend reports that rather than
fabricating GPU work. Everything in this entry is CPU-side work verified on this
machine — the world, the materials, the geometry, the shader's *arithmetic* and
its GLSL syntax. Not one pixel has been rasterised by this project. **FINAL
HARDWARE VALIDATION REQUIRED.**

## 2026-09-27 — the city stops being boxes with better textures

EMERGENT had no textures at all. The vertex format was position, normal and
colour; the fragment shader was a Lambert term against a sun direction. Every
surface in the world was a flat colour. That is not a look that can be fixed by
making the boxes nicer, so this is the rendering and asset foundation the rest
of the game stands on.

**26 CC0 materials, acquired and processed.** Every entry is hand-selected in
`assets/database.mjs` with a written reason, downloaded from Poly Haven,
md5-verified against the provider's published checksums, and resampled and
validated into three 512px texture arrays. Base colour, tangent-space normal
and packed AO/roughness/metalness, 78 tiles, 17.4 MB shipped, 104 MB of VRAM.
`assets/provenance.json` records the source URL, the named creator, the licence
and the fetch date for all 26; `tools/assets/audit.mjs` fails the build if any
of them stops being CC0 or stops matching what ships.

**Validation that rejected real assets.** Three of the first cut were measured
as not tiling — `stone_tile_wall` at 10.2x its interior edge difference,
`wood_plank_wall` at 7.8x, `wood_planks` at 8.3x — and were replaced. Three more
declared a metalness the provider's own maps contradicted: rusted iron measured
0.001 metallic, and it is right, because rust is an oxide and an oxide is a
dielectric. The database was wrong, not the textures. Painted metal is now
forced to zero across every shutter, pole and railing in the city.

**The material system was not actually connected to the shader.** This is the
one that matters, and it is worth being blunt about: everything above — the
fetch, the bake, the validation, the provenance, the audit, the texture arrays,
the material table, the geometry kit — was in place and green, and the game was
still drawing every surface in the world with `colour * (ambient + lambert)`.
The fragment shader read `layout(location=0..2)` and stopped. It never declared
`uAlbedoTex`, `uNormalTex` or `uArmTex`; it never declared `uMatA` or `uMatB`;
it never read the material index the vertex buffer was uploading every frame as
`location=3`. The 78 texture tiles were decoded, uploaded to the GPU and bound
to units 0, 1 and 2 every frame, and then never read.

Every test in the repository passed. The harness reported clean frames, zero
validation errors, zero NaN uploads, three complete texture arrays and
geometry carrying material indices that "the shader can resolve" — the test
asserted the *upload*, and the upload was fine. The upload was never the
problem.

`sceneFS` is now a real shader. It resolves the material index through
`uMatA`/`uMatB`, samples albedo, normal and packed ARM from the three
`sampler2DArray`s at that material's layer, and shades with a GGX + Smith
microfacet BRDF: dielectric and metal reflectance mixed per material
(`mix(vec3(0.04), albedo, metallic)`, so painted steel is a dielectric and
bare galvanised steel is not), a hemispherical sky/ground ambient so a shaded
wall is not black, an environment term through the roughness-aware Fresnel
that gives rough surfaces something to reflect, and emissive strength for
lit windows and lamps that rises at night. Textures are box-projected in the
vertex shader from the material's recorded role — no seams, no unwrapping, no
UV attribute, and world-locked so a brick wall's bricks stay where they are
while the player walks past. Moving geometry keeps solid materials on purpose:
a world-locked projection on a car at 20 m/s slides its paint across its own
body.

**Two new suites, because the harness cannot see any of this.**
`tools/headless_runtime.mjs` stores shader source and reports
`COMPILE_STATUS` true for everything, since it does not compile GLSL — so a
syntax error in a shader is invisible to every other test in the project, and a
shader that compiles but does nothing is worse.

- `test_shaders.mjs` (new, 8 tests) parses all four shaders with
  `@shaderfrog/glsl-parser`, and then checks the wiring in both directions:
  every uniform the renderer sets is declared in a shader, and every uniform a
  shader declares is set. It asserts the vertex shader reads all four
  attributes the buffers upload, that the three texture arrays are declared as
  `sampler2DArray` and actually sampled, and that the `uMatA` array size is
  interpolated from `MAX_MATERIALS` rather than hand-typed.
- `test_shading.mjs` (new, 11 tests, 7,831 assertions) transliterates the
  box mapping and the BRDF into JS and asserts behaviour, because parsing a
  shader says nothing about whether the maths is right. It checks the normal is
  flipped toward the eye; that a lit surface beats a shadowed one by exactly
  Lambert's cosine law rather than by a brightness constant; that roughness
  *widens the specular lobe* — a sharp highlight dominating at the mirror
  direction and falling off off it, which a shader that merely multiplies
  brightness by roughness cannot pass; that a dielectric reflects 4% at normal
  incidence and a metal takes its reflectance from its albedo; that a sky-facing
  surface sees more ambient than a ground-facing one; and a 7,776-case sweep
  for non-finite or negative output. Its last test re-reads the GLSL and
  asserts the transliteration still matches it, so the two cannot drift.

`test_project.mjs` now names the specific wiring too. Deleting
`sampler2DArray uAlbedoTex` from the shader was run as a negative control: the
project test fails with *"the material system is not reachable from the
shader"*, which is the failure that should have caught this the day the
material system landed.

**A real material system.** `materials.mjs` merges the baked set with 17
untextured PBR surfaces into one index space uploaded as two uniform arrays. The
shader has one code path. Texture coordinates are world-planar box mapping
derived in the vertex shader from the normal, so there are no seams, no
unwrapping, and no UV attribute — and moving geometry deliberately uses solid
materials instead, because a world-locked texture on a car at 20 m/s slides its
paint across its own body.

**Geometry that is actually architecture.** `geometry.mjs` is a detail kit of
real small solids: window units with reveals, frames, transoms and projecting
sills; doors with steps and awnings; cornices and parapets with copings; gable
and sawtooth roofs; fire escapes; street lamps with arms reaching over the
carriageway; traffic lights; containers, pallets, crates, bins, hydrants,
benches, bus shelters. `city.mjs` composes them: buildings get a plinth, a
string course per floor, a cornice, a window grid, balconies, roof plant,
downpipes, signage and street dressing; roads get markings, kerbs, crossings,
drains and lighting; trees get a root flare, branches and a seeded canopy.

**Four real bugs this found, all of which would have shipped.** `cone` and
`gableRoof` emitted unnormalised normals, so every cone and every roof in the
world was lit up to 2.2x too bright — a bad normal is not a crash, it is
silently wrong. Characters were 1.60 m tall and labelled 1.78 m, because the
proportion fractions summed to 0.9. And `bench` and `pallet` passed a material
index into the colour argument, producing vertices that were finite and
therefore passed every NaN check while drawing as garbage; the geometry kit now
throws at the single vertex-emission choke point instead.

**Measured, on this machine, before and after the LOD pass:**

| | naive detail | with LOD |
|---|---|---|
| Static vertices | 1,420,680 | 218,226 |
| Static buffer | 54.2 MB | 8.3 MB |
| Static build | 668 ms | 99 ms |
| Dynamic build | 10.11 ms | 2.79 ms |

Detail falls off over a radius deliberately shorter than the streaming radius,
so a building the player can read has its windows and roof plant and one 800 m
away is a correctly materialled mass. Characters and cars have their own, much
tighter, visual LOD.

**The harness grew a texture model.** `tools/headless_runtime.mjs` now validates
texture arrays: layers in range, internal format against its `(format, type)`
pair, a mipmap minification filter with no mip chain reported as the incomplete
texture it is, and empty layers named. It cannot produce pixels, and it cannot
compile GLSL, so what is claimed here is the contract, not the image — which is
exactly why `test_shaders.mjs` and `test_shading.mjs` exist, and why the
harness should not be trusted alone on anything visual.

Verification, all run on this machine:

- `tools/assets/bake.mjs` — 26 materials, every validation passed
- `tools/assets/audit.mjs` — every asset CC0, sourced, referenced, present
- `test_geometry.mjs` (new) — **34,888 assertions**: material table integrity,
  vertex layout, every emitter's normals and indices, and real-world proportions
  for windows, containers, lamps, cars and people
- `test_shaders.mjs` (new) — 8 tests, 97 assertions: all four shaders parse,
  and the uniform and attribute wiring holds in both directions
- `test_shading.mjs` (new) — 11 tests, 7,831 assertions: the BRDF's physical
  behaviour, including a 7,776-case sweep for non-finite or negative output
- `test_headless_game.mjs` — two new tests: three complete texture arrays with
  one layer per material, and a walk of the uploaded vertex buffer proving every
  material index resolves. **15/15 tests, 20,937 assertions**
- `npm test` — world, math, culling, missions, geometry, physics, input,
  project, tooling, runtime: all green
- `npm run build` — 23 MB, `dist/` boots and renders headlessly

`docs/ASSET_PIPELINE.md` has the full reference, including the reasoning for
RGBA8 over KTX2/Basis, what the harness cannot verify, and this honestly
stated list of what is **not** done: no interiors, no downloaded models, no
decal system, no triplanar terrain blending, no vegetation impostors, and no
measured frame rate — all of which is **FINAL HARDWARE VALIDATION REQUIRED**.

## 2026-09-27 — the controls are data, not key checks

Every control in EMERGENT was a hard-coded key comparison. `updatePlayer` read
`keys['d']`, `keys['shift']` and `keys[' ']` out of a mutable object that one
listener wrote, and a second listener compared `e.key.toLowerCase()` against the
strings `'e'`, `'t'`, `'q'`, `'v'`, `'f2'`, `'f3'`, `'d'` and `'n'` to run the
game's verbs. Mouse look was a literal in the `mousemove` listener, the gamepad
was read by index at one call site, and the touch stick fed `touchMove` straight
into the movement maths. Four devices, four separate code paths, and no way to
change a binding without editing the source.

`input.mjs` replaces all of it with one action layer. Fourteen actions —
`forward`, `back`, `left`, `right`, `sprint`, `jump`, `interact`,
`toggleRenderer`, `toggleQuality`, `toggleCamera`, `toggleTelemetry`, `save`,
`load`, `newWorld` and `releasePointer` — are bound to physical key codes,
standard-gamepad buttons and stick axes from a table. Keyboard, pad and touch
all write into the same state, and the game reads only `moveAxes`, `isDown`,
`wasPressed` and `addMouseDelta`. Rebinding is a data operation: `beginRebind`,
`completeRebind`, `resetBindings` and `serialiseBindings` all work on the live
state, and the binding table is reachable from the outside via
`EMERGENT.input`.

Three real defects fell out of doing it:

- **Strafe right walked backwards.** `moveAxes` reports +y for "away from the
  player", EMERGENT's heading convention puts walking forward along
  `-[sin(yaw), cos(yaw)]`, and the two signs disagreed. Every W-key press in the
  game would have moved the character the wrong way. The three existing movement
  tests caught it; the sign is now resolved once, in `updatePlayer`.
- **D opened the telemetry overlay while you strafed right.** EMERGENT's first
  release bound both to `KeyD`. Holding D to strafe therefore opened the debug
  HUD underneath the player. The overlay is `F1` now, and the E2E test asserts it
  stays closed.
- **The harness dispatched events a browser would not.** `tools/headless_runtime.mjs`
  kept handing an event to every listener after one called
  `stopImmediatePropagation`, and modelled `requestPointerLock()` as a no-op —
  so the mouse look, the one control with no fallback binding, could not be
  exercised at all. Both are now faithful.

A key press is now edge-triggered. Holding `T` switches the renderer once
instead of sixty times a second; the quality test had been relying on the old
`keydown`-fires-every-time behaviour and now sends the release a real player does.

Verification, all run on this machine:

- `test_input.mjs` — **29 tests, 189 assertions** over the action layer alone:
  defaults, edge detection, rebinding, collision-free bindings, serialisation
  round-trips and readable labels.
- `test_headless_game.mjs` — new end-to-end test,
  *"the action-based input layer drives movement, look and interaction"*,
  **41 checks**: W and S are opposite along the camera axis, A and D are opposite
  along the strafe axis, D does not toggle telemetry, Shift sprints and releasing
  it walks again, the mouse yaws and pitches only while the pointer is locked, the
  heading the mouse produces is the heading the character walks, E advances an
  interact mission stage, T toggles the renderer once per press and not while
  held, and rebinding `forward` to `I` genuinely rewires the game — W stops
  moving the character and `I` does. Every synthetic event carries `code`, as a
  real browser sends, so a binding written against `key` could not pass by
  accident. Suite total: **13/13 runtime tests, 167 assertions**.
- `test_missions.mjs` — five new tests over the world-facing seam this change
  also moved into the rules module (`jobAvailability`, `resolveStage`,
  `createJob`): availability follows world state rather than the archetype
  table, a stage resolves to a real named place, an unsupportable stage is
  dropped rather than faked, a survey waypoint goes somewhere new, and a created
  job is playable and deterministic. **445 assertions, 0 failing.**
- `npm test` — world, math, culling, missions, physics, input, project, tooling
  and runtime, all green. `test:input` is a new script and is wired into
  `npm test` and CI; `input.mjs` is in the build manifest, and CI now asserts
  `dist/input.mjs` exists.

What this does not do: there is no controls screen in the UI yet. The layer,
the rebind path and the serialisation are all shipped and reachable, so the
screen is a rendering of state that already exists rather than a new system.

## 2026-09-26 — jobs you can actually take, and an economy with shortages in it

There was one kind of job. It was a delivery, it was hard-coded across three
functions (`chooseMission`, `updateMission` and three branches inside
`interact`), and it branched on a `stage: 'pickup' | 'delivery'` string that
every call site had to switch on. Adding a second kind of job would have meant
editing all three.

Jobs are now data. `missions.mjs` holds the archetypes as data — four of them,
`delivery`, `restock`, `survey` and `respond` — each a list of stage templates
and a reward curve. A mission is a list of concrete stages; the game supplies
positions and business ids through one resolver, and the module decides when a
stage is done, what happens next, and whether the job succeeded or ran out of
time. Four stage types cover it: `goto`, `interact`, `hold` and `evade`. Adding
a fifth kind of job is now a data entry.

The rules live in a module with no GL, no DOM and no world generation, so they
are tested directly rather than inferred from a delivery completing: **21 tests,
408 assertions**, covering proximity and keypress separately, expiry ordering
(an expired job cannot be banked by a late arrival), stage-time reset between
legs, reward monotonicity, and the pre-pipeline save shape that would otherwise
throw on the first tick after a load.

### The economy underneath it was a floor, not a balance

Two of the four archetypes were unreachable, and the reason was a real defect
that every existing test walked straight past. Passive resupply was a flat
trickle applied only below 18 — a hard floor. Measured across 714 businesses,
the minimum stock sat at *exactly* 18, at every point in time, in every seed.
Nothing was ever short, so "low stock" as a mission premise could not occur and
the player's deliveries moved goods between businesses that were all fine.

Resupply is now logistic — strongest on an empty shelf, zero on a full one —
and every business has its own `supply` and `popularity`, so consumption and
resupply balance at a different level in each one. Measured after 160 seconds
of simulation: minimum **18 → 15** and still falling, p10 **26**, median **59**,
p90 **92**, with **~70 businesses short** and **~470 with surplus**. Businesses
close when they empty and the player's deliveries are what refill them.

A delivery now moves 22 units of stock rather than 3. Against a forty-unit
shortfall, three crates is not a delivery, it is a rounding error.

### Also

- The objective readout is driven from the simulation rather than the once-a-second
  HUD refresh, so the distance counts down live instead of jumping once a second.
- The screen-space marker and the 3D beacon follow the current *stage*, which is
  what lets a multi-stage job change where it points without the renderer knowing
  what a mission type is.

## 2026-09-26 — the crowd and the traffic are solid

The player walked through everyone. NPCs had no colliders at all, and cars drove
through the player and through each other. `setAgentBox` existed in the physics
layer, was covered by a test, and was called from nowhere in the game.

Agents within 260 units of the player now get kinematic bodies, keyed by entity
and synced every frame as a set, so one that leaves the radius has its body
removed and one that enters gets one created. Kinematic rather than dynamic on
purpose: NPCs follow scripted goals and cars follow lanes, so simulating them
with forces would be slower, less controllable, and would produce a crowd that
shoves the player around rather than one the player has to walk around.

The radius is a reachability bound, not a quality dial — nothing beyond 260
units can be touched, so a collider out there costs solver time and buys
nothing. A budget of 64 bodies, filled nearest-first, bounds the cost in a dense
district; the agents that fall off the end are the ones already out of reach.

**Measured.** Walking straight into an NPC, the closest approach is **2.07
units** against a geometric minimum of 1.60 (player radius 1.15 + NPC half-width
0.45) plus the collider offset — the player is stopped by the body rather than
passing through it. Frame cost across three 300-frame benchmark runs: 4223ms
with the crowd solid against 4174ms without, a difference of about 0.16ms per
frame and inside the run-to-run spread of ±130ms. This harness advances a
virtual clock, so its reported FPS is a function of the frame interval rather
than of throughput and is not a performance figure.

A new runtime test walks the real game loop into an NPC and asserts the closest
approach never falls below 1.2 units, which a world without colliders fails at
zero.

## 2026-09-26 — a real character controller, and terrain you can stand on

The player collided with the world through `blockedPlayer(x, z)`: an
axis-aligned point test with a 1.2-unit pad, evaluated only at the destination.
That is not collision, it is a suggestion. There was no swept test, so anything
thin could be crossed; no step-up, so a kerb was a wall; no slope limit; and no
jump. Height was not simulated at all — `player.y` was assigned
`terrainHeight(x, z) + 0.55` every frame, so the player was glued to the ground
by fiat and could never leave it.

Movement is now Rapier's `KinematicCharacterController` (`@dimforge/rapier3d-compat`
0.21.0, Apache-2.0), wrapped by a new `physics.mjs` that owns no collision
algorithm of its own. Swept collision, autostep, snap-to-ground, slope limits,
character mass and dynamic-body pushing are the library's. What the wrapper owns
is keeping a Rapier world in step with EMERGENT's procedural city: a terrain
heightfield rebuilt around the player, keyed static-box colliders synced from
the streaming residency set, and a fixed-timestep accumulator so the simulation
does not change with the frame rate.

**The terrain was the blocker, and it was worse than a tuning problem.**
`fbm` sampled the lattice hash directly, with no interpolation between lattice
points, so `terrainHeight` was white noise: measured, it changed by up to
**13 units over a 3-unit step**. A coarse render mesh hid this completely, and
so did the old collision, because the player's height was simply assigned from
the same function. The moment anything needed a *surface* — a collider to stand
on, a slope to walk up — there was nothing coherent to use. `fbm` now
interpolates the four corner hashes with a quintic fade, and the measured
terrain has a **maximum slope of 9.9°** across three seeds, with under a unit
of change over any 4-unit step.

Everything downstream of that had to be real:

- The collider is a Rapier heightfield on a 12-unit grid, chosen by measurement:
  it holds the collider surface within **0.28 units** of the surface the
  renderer draws, where a 26-unit grid was off by 0.78. Spawning at the raw
  height function instead of the collider surface drops the character inside the
  terrain, where the controller cannot move at all.
- A terrain patch is **283 rows generated a few rows per frame**, with the
  previous collider staying live throughout. Built synchronously it costs 43ms
  in one frame, four times every four seconds of walking.
- Movement is sub-stepped so no single sweep is larger than the character
  radius, and the body is advanced between sweeps. Re-casting from an unmoved
  body is not sub-stepping, it is the same sweep counted N times — which is
  exactly how a "sub-stepped" character walks through walls.
- A jump is a press, not a state. Gating on the level of the key re-fires the
  instant the character lands, which is how platformers acquire bunny hopping.

**Tests.** `test_physics.mjs` is 26 tests and 71 assertions running the real
engine — real WASM, no mocks, on every CI run, on a machine with no GPU and no
browser. It covers swept collision at 40 units per step, wall sliding, kerb
step-up, ramp ascent, slope limits, jump gating, determinism across two
identical worlds, body-leak checks, incremental terrain rebuild, and a 1200-step
walk over real terrain asserting the character never sinks through the collider.
`test_headless_game.mjs` gained an end-to-end test that walks the real game loop
into a building and asserts the character is stopped at its wall.

**Also removed:** the hand-rolled point test, and the `player.y = terrainHeight(…) + 0.55`
line that made the player untouchable by physics.

## 2026-09-26 — frustum culling: a third of the city stops reaching the GPU

The renderer uploaded the entire streaming radius — up to a 1160-unit disc of
city, terrain, buildings, roads, trees and water — as one vertex buffer, and
submitted all of it every frame regardless of where the camera pointed. Roughly
two thirds of that was behind the player. There was no frustum culling at all;
the only spatial filtering was the streaming radius, which is a residency
decision, not a visibility one.

Static geometry is now laid out as one contiguous run per 480-unit cell inside
the same single buffer, each with its own bounds. Every frame the six frustum
planes are extracted from the view-projection matrix and each cell is tested; a
rejected cell costs six plane tests and nothing else, because a run is a
`drawArrays(first, count)` sub-range rather than a separate buffer. One upload,
many draws, no VAO churn.

Measured in the headless harness at the default view: **33% of static vertices
are now rejected before reaching the GPU** (7,272 of 22,038), and the rejected
set changes when the camera turns.

`culling.mjs` is deliberately pure — no GL, no DOM, no world state. The
arithmetic that decides whether geometry is drawn is the arithmetic most likely
to be subtly wrong, and a wrong plane sign does not crash, does not NaN, and
does not fail any check that looks at whether the game runs. It deletes a third
of the city at the screen edge instead. So the plane math is unit tested against
ground truth: a camera is built, a 4913-point grid is projected to NDC by an
independent path, and the predicate is required never to cull a point that is
actually on screen. The reverse direction — conservatively keeping a box that
straddles a frustum plane — is deliberate and documented, not a failure.

**Two real bugs found while building it**, both in the new code and both caught
by tests rather than by inspection:

- `growBoundsBox` discarded the result of its first `growBounds` call. With a
  null accumulator that call allocates, so the second call started a *different*
  accumulator from the maximum corner — every box bound was half its real size,
  which would have culled geometry that was on screen.
- The first culling test asserted that a volume predicate and a point test
  produce identical answers. They cannot, and should not: a box straddling a
  frustum plane is correctly kept while its centre projects off screen.
  Comparing them for equality is a test that can only pass if the culler is
  wrong. It now asserts the one-directional safety property instead.

`tools/build.mjs` gained `culling.mjs` in its manifest — caught by the existing
`the built dist/ artifact boots and renders` check, which is exactly what that
check is for.

Still per-cell, not per-object: a cell is drawn if any part of it is on screen.
That is the right trade at this cell size and it is a floor, not a maximum.

## 2026-09-26 — the native frame loop, and a renderer that reports honestly

The native engine had a self-test and no frame loop. There was no code
anywhere that sequenced physics and animation against a clock, and no renderer
beyond a one-shot bootstrap. This adds both.

**The loop** (`native/src/frame_loop.cpp`, 125 behaviour checks). Physics runs at
a fixed 1/60s step, always; real time accumulates and drains in whole steps, so
the simulation advances at a rate that depends on nothing but the time that has
elapsed. Presentation runs at the display rate, with the leftover fraction
`alpha` used to interpolate between the last two simulation states. Animation is
advanced once per frame by the real delta and never per substep, which is what
keeps motion smooth at 144Hz while the solver stays deterministic.

The synchronization rules are the substance here, and each one is a test:

- 60 frames at 1/60s and 30 frames at 1/30s take the same 60 steps and end in
  the same place. Confirmed through the shipped binary too: 4 seconds of
  `--walk` at 30Hz, 90Hz and 144Hz all end at `pos z=16.11`.
- The same delta and input sequence over 500 frames is bit-identical, in both
  position and pose.
- A frame that takes no physics step collapses its interpolation window, so at
  144Hz — where half of all frames take no step — nothing is ever drawn behind
  the simulation.
- A four-substep frame advances animation by exactly one real delta.
- A stalled frame is clamped and its step count capped, with the backlog
  discarded rather than carried, because carrying it is the spiral of death.
  An infinite delta is a failed clock and is discarded instead, because
  clamping it would simulate time that never happened.

**The renderer** (`native/src/vulkan_render.cpp`). `RenderBackend` is the
interface; `VulkanRenderBackend` implements a real render pass, depth target,
graphics pipeline, instanced draw of the scene boxes and the ozz skeleton
straight from the pose matrices, and acquire/submit/present. It uses a swapchain
when a window is attached and an offscreen target when one is not.

`NullRenderBackend` is not a stub. It validates every frame state and refuses to
count a rejected frame as submitted, which is what catches a loop that produces
a NaN on a machine with no GPU rather than on someone else's.

**Not verified: none of the Vulkan render path has ever executed.** There is no
ICD and no `/dev/dri` here, so `vkCreateInstance` fails and `open()` reports
why. That code is compile- and link-checked against Vulkan 1.3 headers and
nothing more. The GLSL is not compiled either — no `glslc` here — so `open()`
reports `shader module unavailable` by name. Details in
`docs/ENGINE_VERIFICATION.md`.

**No windowing dependency was added.** `SurfaceProvider` is the one extension
point; implementing it over GLFW, SDL or XCB is a single class, and nothing in
the loop, the physics, the animation or the render path changes to accommodate
one. The reasoning is in `docs/OPEN_SOURCE_DECISIONS.md`.

Also: `JoltPhysicsWorld` gained stable body handles and
`bodyPosition`/`bodyVelocity`/`setBodyVelocity`/`setBodyPosition`, because a
frame loop cannot drive a body through `firstDynamicY()`. `emergent_native`
gained `--frames`, `--hz`, `--realtime`, `--render` and `--walk`, and ctest now
runs the shipped binary's loop at three rates as well as the test binary.

## 2026-09-26 — the last three unwired dependencies are now real

The previous pass left a list of things the README implied and the tree did not
have. Three of them are now genuinely integrated, and the two that are not are
documented as deliberate exclusions rather than silent gaps.

**ozz-animation: 0 symbols → 188.** It was wired into the build and nothing
called it, so the linker dropped both archives and it cost build time for no
code. `native/src/animation.cpp` now owns the authoring half ozz does not
ship: a ten-joint biped rig and idle/walk/run clips, described by joint motion
curves in code and baked once through `SkeletonBuilder` and `AnimationBuilder`.
Playback runs the real runtime jobs — `SamplingJob`, `BlendingJob`,
`LocalToModelJob` — behind `AnimationLibrary`/`Animator`/`Pose`, none of which
leaks an ozz type into a header. `NativeEngine` bakes the rig during
`initialize()` and plays a walk cycle in the self-test, which is what puts the
symbols in the binary.

Two real bugs were found and fixed while building it:
- **The rig's joint table was in the wrong order.** ozz stores joints in
  depth-first order and `LocalToModelJob` resolves each from its parent's
  earlier index; a table that looks right but is not depth-first animates the
  wrong bones with no error. The bake now asserts the baked order against the
  table and refuses to produce a rig that disagrees. This assertion is what
  caught the bug.
- **`SamplingJob::Context` takes a track count, not a SoA slot count.** Passing
  `num_soa_tracks()` (3) instead of `num_tracks()` (10) produces a context of
  one SoA slot, and every `SamplingJob::Run()` then fails validation.

**Zstandard: nothing → 450 symbols, and a pack format.** `native/src/asset_pack.cpp`
defines `.ezpk`: a 48-byte header, one zstd frame per entry, and a trailing
frame holding the index. The writer and reader are both in the engine, and the
self-test writes a pack and reads it back on every run.

One finding is worth more than the feature. `ZSTD_c_checksumFlag` is **off by
default**, so `ZSTD_compress()` produces frames that decode cleanly after
corruption — the obvious implementation looks like it has integrity checking and
does not. The writer now sets the flag explicitly and the reader *refuses* any
frame that does not carry a checksum, after inspecting the frame header via
`ZSTD_getFrameHeader` before allocating anything. The corruption test corrupts a
byte inside a payload and requires the read to fail; it passed against the
first implementation by accident and fails against this one on purpose.

Header offsets are range-checked by subtraction rather than addition, so a
hostile index cannot overflow the check and steer a read outside the buffer.

**Profiling: none → always-on zone accounting, plus optional Tracy.** A named
`profile::Scope` accumulates calls, total, minimum and maximum wall time into a
fixed 64-entry table that allocates nothing after start-up, and a run that
overflows it reports `droppedScopes` rather than quietly measuring a subset.
`emergent_native` instruments physics, ECS, mesh optimisation, animation and the
scene cull, and writes `emergent_profile.json`; CI publishes it as an artifact.
Tracy (`v0.13.0`) is wired behind `-DEMERGENT_ENABLE_TRACY=ON` with the same zone
names, and CI *builds and runs* that variant so the option cannot rot. It stays
off by default and no capture is claimed anywhere: Tracy's client streams to a
running Tracy server, and a build container has none. There is deliberately no
"connected" predicate in the API, because Tracy exposes no connection state and
inventing one would be the exact failure this module exists to prevent.

Running that Tracy-enabled variant is what caught a real defect in it. The
CMake block had `set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)`, reasoning that
"don't build the profiler" meant "don't turn profiling on". It means the
opposite: with `TRACY_ENABLE` off, `TracyClient.cpp` compiles to a stub that
defines none of the profiler API, so `emergent_profiling` built cleanly and then
*any* consumer failed to link on `tracy::GetProfiler()`. The block also set
`TRACY_UPLOAD`, an option that does not exist in Tracy v0.13.0. Both are fixed;
the Tracy-enabled executable now links 477 `tracy::` symbols and its 4/4 tests
pass, and the profiling test correctly reports `compiled in; a capture needs a
running Tracy server` rather than claiming a capture.

**Verification.** `ctest` is now 6/6, up from 3/3. The three new suites add 215
behaviour checks: 84 animation, 83 asset pack, 48 profiling. The profiling test
drives the real physics and animation subsystems rather than a synthetic loop, so
"the engine reports through the profiler" is checked rather than assumed.

Still not integrated, and now stated as exclusions: **KTX/Basis** (no texture
assets to decode and no GPU to validate an upload against), **Slang** (the
compute shaders are GLSL compiled by the driver; there is no offline
shader-compilation step to replace), **The Forge** (it would replace the
renderer rather than extend it).

## 2026-09-25 — native engine built and verified for the first time

The native C++ tree had never been compiled. Every previous verification claim
about it was capability-probe reporting, not a build. This pass points a real
compiler at it, fixes what breaks, and records the result.

Fixed, all found by compiling rather than by reading:
- **Volk was pinned to a tag that does not exist.** `GIT_TAG 1.4.328` — Volk
  tags releases `vulkan-sdk-<version>`, so `EMERGENT_FETCH_DEPS=ON` could never
  have succeeded. Repinned to `vulkan-sdk-1.4.328.0`.
- **Jolt was added from the wrong directory.** Its CMake entry point is
  `Build/CMakeLists.txt`; the repository root has no `CMakeLists.txt`.
- **Jolt was added twice** after the tree was restructured, reusing one binary
  directory, which CMake rejects.
- **Jolt's Vulkan compute backend was enabled**, which precompiles HLSL shaders
  with glslc/dxc at build time and hard-fails with no shader compiler installed.
  EMERGENT runs Jolt on the CPU, so `JPH_USE_VK` is off.
- **ozz was configured with the wrong option names.** The switches are
  `ozz_build_samples`/`_howtos`/`_tests`/`_tools`/`_fbx`, not `BUILD_*`; with the
  wrong names ozz's samples were built and configure died looking for OpenGL.
- **`optimization.cpp` did not compile.** `meshopt_analyzeVertexCache` returns a
  `meshopt_VertexCacheStatistics` struct in meshoptimizer >= 0.22, not a float.
- **`navigation.cpp` did not compile.** Recast 1.6 removed
  `dtCrowd::getEditableQuery()`; destinations are now projected through the
  `dtNavMeshQuery` the world owns.
- **VMA was never instantiated.** It ships as a header-only `INTERFACE` target,
  so the link failed on undefined `vmaCreateAllocator`/`vmaCreateImage`/
  `vmaDestroyImage`. Added `native/src/vma_impl.cpp` as the single
  `VMA_IMPLEMENTATION` translation unit, and imported the volk dispatch tables
  via `vmaImportVulkanFunctionsFromVolk` before allocator creation, which the
  `VMA_DYNAMIC_VULKAN_FUNCTIONS` build requires.
- `navigation.cpp` and `vulkan_backend.cpp` were rewritten off the single-line
  function bodies that were producing misleading-indentation warnings.

Verified, by running it:
- `emergent_native` builds and links against all eight pinned upstream
  libraries and runs: meshoptimizer, Flecs and miniaudio all report active, and
  Jolt reports `first_dynamic_y=0.48` for a body seeded at `y=4.0` that fell and
  came to rest.
- `ctest` 3/3 pass, including a 22-assertion Jolt behaviour test covering
  gravity, resting contact, slab geometry, lifecycle and post-shutdown safety.
- `.github/workflows/native.yml` now clones the same pinned revisions, builds and
  runs ctest, so the native tree cannot silently return to never being compiled.
- `docs/ENGINE_VERIFICATION.md` rewritten: the previous record claimed a
  successful configure/build that could not have happened.
- `third_party/` and `build/` are gitignored; the upstream trees are pinned by
  revision and fetched, not vendored.

Still not integrated, and now documented as such rather than implied: **The
Forge** (not added; it would replace the renderer rather than extend it),
**ozz** (its libraries build, but no code includes an ozz header, so the linker
drops them and the binary contains 0 ozz symbols — it animates authored
skeleton/clip data and the repository has no skeleton, clips or importer),
**KTX, Slang, Tracy, Zstandard** (no code in the tree would use them).

Vulkan still cannot execute here: no ICD and no `/dev/dri`, so
`vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` and the engine reports
that rather than fabricating GPU work.

## 2026-09-25 — runtime verification and hot-path pass

Fixed:
- **Startup crash on the first building.** `colorForBuilding` indexed a palette
  with `b.facade % p.length`, and `b.facade` is a float in [0,1), so the index was
  fractional and the lookup returned `undefined`. The game threw on the very
  first static-scene build and could not start. The palette is now indexed with a
  floored, scaled index, hoisted to a frozen module constant instead of being
  rebuilt per building per rebuild.
- `pushLowPolyTree` computed a `variation` value that was never used (and used a
  meaningless float modulo). Trees now apply a deterministic per-tree brightness
  variation to the canopy.
- The static server shadowed its own `viewport()` and `clearColor()` methods with
  instance fields of the same name, and reported `COMPILE_STATUS` from a global
  error counter that later errors could retroactively falsify.

Performance:
- `missionBuilding()` scanned all ~714 businesses linearly, twice per frame (the
  mission beacon and the screen marker). Business and building id maps are now
  built with the spatial index, making the lookup O(1).
- `pushNpc` received a spread copy of the whole NPC object — identity, schedule,
  destination and memory array included — on every visible agent, every rebuild.
  It now takes the five scalars it actually uses.
- `buildStaticScene` called `sunState()` (and its trig) twice per building inside
  the building loop; it is resolved once per build.
- Terrain biome lookup recomputed a region index per quad from a value it did not
  use; the lookup now derives directly from the clamped quad origin.

Added:
- `math3d.mjs`: projection, view, multiply and point-transform math extracted from
  the renderer into a testable module, with 55 unit assertions including frustum
  symmetry, depth-range mapping, associativity and degenerate vertical views.
- `tools/headless_runtime.mjs`: a validating WebGL2 + DOM surface that runs the
  **unmodified** game inside Node. It checks draw calls against buffer bounds,
  scans uploads for non-finite floats, rejects uniforms set against the wrong
  program, and records any GL entry point it does not model so the harness
  cannot silently stop verifying something. Doubles as a CLI benchmark driver.
- `test_headless_game.mjs`: 8 runtime tests / 78 assertions driving the real frame
  loop — boot, long run, keyboard movement, streaming under teleport, every
  quality level, both renderer modes, a full delivery mission, and save/load
  including rejection of a save from a different world seed.
- `tools/serve.mjs`: dependency-free static server binding `0.0.0.0`, with path
  traversal rejected at the resolver.
- `tools/build.mjs`: manifest-driven production build into `dist/` that fails if
  the HTML would not load the entry module, plus a `build.json` stamp.
- `test_tooling.mjs`: 6 checks covering traversal rejection, MIME types, live HTTP
  responses and build output.
- A screen-space mission marker on a dedicated 2D overlay canvas, projected
  through the extracted matrix module.
- `globalThis.EMERGENT`: a small, documented dev/test surface (state getters,
  `teleport`, save/load) so tests assert real simulation state instead of
  scraping the DOM.

`npm test` now runs all five suites in ~10s. Headless benchmark results for both
renderer modes are recorded in `benchmark.md`; on-device FPS, GPU time and VRAM
remain unmeasured and are not claimed.

## Completion pass — 3D game integration

- Preserved the existing deterministic world/simulation data model.
- Split deterministic world generation into `world.mjs` for testability.
- Expanded district/building/vegetation/business generation while preserving seed reproducibility.
- Added coherent gameplay loop with world-state-driven delivery missions.
- Added mission pickup/delivery stages, rewards, progression rank and business consequences.
- Added persistent player/business/NPC/mission/event save state.
- Added spatially indexed building collision checks.
- Added NPC daily scheduling, needs, mood and activity memory.
- Added rain-aware NPC shelter behavior.
- Added lane-aware traffic speed control and rain slowdown.
- Added traffic incident events from close headway in poor weather.
- Added procedural architectural details: roofs, windows, doors, signs, warehouse differences and street furniture.
- Added low-poly 3D character parts, vehicles and vegetation.
- Added animated water movement at the vertex stage.
- Added a geometric sun-shadow pass for nearby architecture.
- Added adaptive dynamic-buffer reuse with distance/importance-based per-NPC update cadence.
- Added renderer quality levels LOW/MEDIUM/HIGH/ULTRA.
- Added mobile touch movement/sprint/interact controls.
- Added player-facing HUD and developer telemetry separation.
- Added benchmark query modes for standard/adaptive comparison.
- Added deterministic and project integrity tests.

## 2026-09-24 — upstream subsystem replacement pass

- Added an actual Recast/Detour native navigation boundary with navmesh loading and crowd-agent movement APIs.
- Added explicit offline source-tree dependency inputs for Jolt, meshoptimizer, RecastNavigation, Volk and VMA.
- Added installed-package discovery for VulkanMemoryAllocator and RecastNavigation.
- Changed the Vulkan bootstrap resource path to VMA allocation/destruction instead of manual Vulkan memory selection/binding when the full upstream Vulkan build is enabled.
- Disabled Recast demos/tests/examples for dependency builds; EMERGENT consumes the libraries rather than rebuilding their sample applications.
- Added engine verification documentation and kept capability reporting truthful when upstream libraries or a Vulkan device are unavailable.
