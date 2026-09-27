# EMERGENT — Living World 3D

EMERGENT is the evolved version of the original playable 2D procedural prototype. The simulation and deterministic world model were preserved; the player-facing runtime is now a self-contained WebGL2 3D game.

## Run

```bash
./run.sh          # or: npm run dev
```

Then open the printed local URL in a WebGL2-capable browser. The project has no
runtime dependencies and no install step; `tools/serve.mjs` is a dependency-free
static server that binds `0.0.0.0` on `$PORT` (default 8765).

For a production build, `npm run build` assembles `dist/` from an explicit file
manifest and writes `dist/build.json`.

## Player loop

Explore the world → discover districts → take a job → walk or drive to each stage in turn → complete it → get paid and rank up → the city's stock and your standing move → the next job is drawn from whatever the world now needs.

Jobs are **data-driven**, not hard-coded. `missions.mjs` declares four
archetypes — `delivery`, `restock`, `survey` and `respond` — as stage templates
with a reward curve each. A job is a list of concrete stages, and four stage
types cover them: reach a place, interact with something, hold a position for a
while, or stay away for a while. Which jobs are on offer is read from world
state: a delivery needs a business with surplus and one that is short, a
restock needs a business that has run down, a response needs a live incident.

The economy underneath is a real balance rather than a floor. Every business
consumes stock and resupplies logistically — strongly when its shelves are
empty, not at all when they are full — and each has its own supply and
popularity, so businesses settle at different levels. Some districts are short.
Those are the jobs.

## Controls

Desktop: `WASD` / arrows move, `Shift` sprint, `Space` jump, click + mouse look, `E` interact, `T` standard/adaptive renderer, `Q` quality, `V` first/third-person camera, `F2` save, `F3` load, `F1` developer telemetry, `N` new world.

Mobile: virtual joystick, RUN and E buttons appear automatically on narrow screens.
Gamepad: left stick move, triggers sprint, A jump, X interact, Y camera.

Every control above is a row in one binding table in `input.mjs`, not a key
comparison in the game loop. Keyboard, gamepad and touch write into the same
action state; the game reads `moveAxes`, `isDown` and `wasPressed` and nothing
else. Rebinding (`beginRebind` / `completeRebind` / `resetBindings` /
`serialiseBindings`) operates on the live state, and `EMERGENT.input` exposes it,
so changing a control is a data edit rather than a source edit. A press is
edge-triggered: holding a toggle key fires it once, not once per frame.

## Movement and collision

The player is simulated by [Rapier](https://rapier.rs) 0.21.0
(`@dimforge/rapier3d-compat`, Apache-2.0), not by bespoke code. `physics.mjs`
wraps the library and owns no collision algorithm: swept collision, autostep,
snap-to-ground, slope limits, character mass and dynamic-body pushing are all
Rapier's.

What that buys over the point test it replaced:

- **You cannot walk through a building**, at any speed. A 40-unit-per-step move
  is split into sub-sweeps no larger than the character radius, and the
  anti-tunnelling guarantee holds — asserted, not assumed.
- **You can walk up a kerb** (0.9-unit autostep) but not a wall.
- **You can walk up a ramp** and slide back down a slope you cannot climb.
- **You can jump**, and holding the key does not make you bunny-hop, because a
  jump is a press edge.
- **You are simulated, not placed.** The terrain is a Rapier heightfield sampled
  from the same `terrainHeight` the renderer displaces vertices with, rebuilt
  incrementally around the player as they travel.

The simulation runs on a fixed 1/60s step with an accumulator, so gravity, jump
height and sliding do not change with the frame rate.

### The crowd

NPCs and cars within 260 units of the player get kinematic bodies, so you walk
*around* people rather than through them, and traffic is solid rather than
decorative. Beyond that radius nothing can be touched, so a collider out there
would cost solver time and buy nothing.

`test_physics.mjs` runs the real engine — real WASM, no mocks — as part of
`npm test`, so all of the above is checked on every CI run on a machine with no
GPU and no browser.

## World

The world is deterministic from a seed and generated hierarchically:

`WORLD → REGIONS → BIOMES → DISTRICTS → ROADS → BUILDINGS → BUSINESSES → POPULATION → TRAFFIC → EVENTS`

The reference seed `173927` currently generates 64 regions, 50 districts, 672 roads, 3,077 buildings, 1,838 vegetation instances, 714 businesses, 520 NPCs and 150 vehicles.

The exact counts are seed-dependent. `world.mjs` is the source of truth for generation and is tested for deterministic reproduction.

## Simulation

Persistent NPCs have identity, role, home, workplace, schedule, goals, energy, money, mood and a small event/activity memory. They choose between work, home, social and shelter behavior, with rain affecting movement and activity.

Businesses consume stock through simulated demand, change prices from economy/stock pressure, open/close as supply changes, and respond to player deliveries.

Traffic has lanes, acceleration/braking, same-lane following slowdown and rain-aware speeds. Close headway under rain can produce a traffic incident event.

Day/night, weather, business demand, traffic speed, NPC behavior and generated missions are connected rather than isolated visual toggles.

## 3D renderer

### Frustum culling

The static world is uploaded as **one** GPU buffer, but laid out as one
contiguous run per 480-unit cell, each with its own bounds. Every frame the six
frustum planes are extracted from the view-projection matrix and each cell is
tested; a rejected cell costs a plane test and nothing else — no rebind, no
re-upload, no per-object draw call.

Before this, the entire streaming radius — up to a 1160-unit disc of city — went
to the GPU every frame no matter where the camera pointed. Measured in the
headless harness at the default view:

| | |
|---|---|
| Cells built | 34 |
| Cells submitted | 18 |
| **Static vertices culled before the GPU** | **33%** (7,272 of 22,038) |
| Rejected set changes with camera | asserted, not assumed |

The plane arithmetic lives in `culling.mjs` as pure functions and is unit
tested against ground truth rather than inspected in a screenshot, because a
wrong plane sign does not crash — it silently deletes a third of the city at the
screen edge, or nothing at all, forever. The safety property asserted
exhaustively is one-directional: **an on-screen point is never culled.** A box
straddling a plane is deliberately kept, because part of it is on screen.

`culling.mjs` is pure — no GL, no DOM, no world state — so it is tested
directly (71 assertions) and runs in Node with no browser.

### Shading and shadows

Lighting is a single directional term with distance fog, and the sun drives
ambient, fog colour and clear colour from a full day/night cycle. Shadows are
projected quads on the terrain, not a shadow map — see *Current limitations*.

The renderer uses WebGL2 directly. It has real perspective projection, depth testing, vertex buffers, 3D terrain/buildings/trees/NPCs/vehicles, directional lighting, atmospheric fog, animated water geometry, and a geometric sun-shadow pass.

The adaptive renderer is deliberately described as **selective 3D recomputation**, not pixel temporal reprojection. It uses distance, player motion, weather and district importance to decide how much geometry/detail to update. Dynamic representations also reuse prior per-NPC positions for longer intervals when an entity is visually unimportant or far away.

## Streaming and scalability

World data is retained deterministically, while only a player-centered render window is converted into detailed GPU geometry. Quality levels change stream radius, terrain detail, vegetation density, dynamic visibility and detail.

Quality modes: `LOW`, `MEDIUM`, `HIGH`, `ULTRA`.

## Save/load

Auto-save occurs periodically and is also available through `F2` / `F3` or the HUD buttons. Saved state includes player state, world clock/weather/economy, discoveries, active events, mission state, all business state and persistent NPC state.

## Benchmark

The fastest reproducible measurement runs the real game headlessly, in Node, with
no browser:

```bash
npm run bench
npm run bench:adaptive
```

For on-device numbers, use the actual game page with:

- `?benchmark=standard&seed=173927&quality=HIGH`
- `?benchmark=adaptive&seed=173927&quality=HIGH`

After about 8 seconds the page writes a JSON result to `document.body.dataset.benchmark`.

Reported measurements include FPS, CPU-side render wall time, dynamic-buffer build timing, recomputation/reuse counts, visible-object counts, geometry counts and streaming operations.

GPU time and VRAM are not fabricated. They require a trustworthy browser/GPU measurement path on the target device.

See `benchmark.md` for the reproducible procedure and measured container results, and `FINAL_SCORECARD.md` for the completion audit.

## Engineering verification

```bash
npm test          # everything below
npm run check     # syntax check
```

| Suite | Command | What it proves |
|---|---|---|
| World | `npm run test:world` | Generation is deterministic per seed and varies across seeds; required populations exist; terrain is continuous, walkable and has relief |
| Math | `npm run test:math` | Projection, view, multiply and point-transform algebra, including degenerate cases |
| Culling | `npm run test:culling` | Frustum plane extraction and AABB rejection, including the degenerate cases |
| Missions | `npm run test:missions` | The job rules: stage types, proximity vs keypress, expiry ordering, reward curve, archetype availability, legacy save shapes |
| Physics | `npm run test:physics` | The real Rapier engine: swept collision at speed, wall sliding, step-up, slope limits, jump gating, determinism, terrain streaming, no body leaks |
| Project | `npm run test:project` | Required renderer/simulation systems exist in source, HTML wiring is intact, terrain is finite |
| Tooling | `npm run test:tooling` | Static server rejects path traversal and serves correct MIME types; the build emits every file the page needs |
| Runtime | `npm run test:runtime` | The real game boots and runs its real frame loop for hundreds of frames |

The runtime suite is the important one: it imports `game3d.js` unmodified and
executes its actual frame loop, simulation, streaming and WebGL draw calls inside
Node against a validating GL surface. It fails on a thrown frame, a WebGL
validation error, non-finite geometry uploaded to a buffer, a draw call that
reads past the end of a buffer, a uniform set on the wrong program, a broken
mission delivery flow, a save/load that does not round-trip, or a GL entry point
the harness does not model (so the harness cannot silently stop verifying
something). It covers boot, a long run, keyboard movement, walking into a
building and being stopped by it, walking into an NPC and being stopped by it,
streaming under
teleport, every quality level, both renderer modes, a full delivery mission, and
save/load including rejection of a save from a different world.

## Assets, materials and the detail kit

EMERGENT had no textures at all before this pass. The vertex format was
position, normal and colour and the fragment shader was a Lambert term, so every
surface in the world was a flat colour. There is now a full asset pipeline, and
it is the same pipeline the project already had — extended, not replaced.

**Acquisition.** `assets/database.mjs` is a hand-curated list of 26 materials,
each with a written reason it belongs in this city. Nothing is downloaded
blindly. `tools/assets/fetch.mjs` pulls them from Poly Haven, verifies each file
against the provider's own published MD5, and writes `assets/provenance.json`
with the source URL, the named creator, the licence and the fetch date.

**Processing.** `tools/assets/bake.mjs` resamples to a uniform 512px, packs
AO/roughness/metalness into one array, and — before it will accept a material —
validates channel order, seam continuity, albedo degeneracy and the declared
metalness against the provider's own measurements. It rejected six materials
from the first cut:

- three genuinely did not tile (`stone_tile_wall` at 10.2x its interior edge
  difference, `wood_plank_wall` at 7.8x, `wood_planks` at 8.3x) and were replaced
  with measured alternatives;
- three declared a metalness their own maps contradicted — rusted iron measures
  0.001 metallic, correctly, because rust is an oxide and an oxide is a
  dielectric. The database was wrong, not the texture.

**Legal.** Every asset is CC0 and every one is named in the manifest.
`tools/assets/audit.mjs` runs in CI and fails the build if any asset stops being
CC0, loses its provenance, is unreferenced, or is referenced but absent from
`dist/`. Redistribution rights are checked, not assumed.

**Runtime.** `materials.mjs` merges the 26 baked materials with 17 untextured
PBR surfaces into one index space uploaded as two uniform arrays, so the shader
has a single code path. Texture coordinates are world-planar box mapping derived
in the vertex shader from the material's recorded role — no seams, no
unwrapping, no UV attribute, and world-locked so a brick wall's bricks stay put
while the player walks past.

The shader is a GGX + Smith microfacet BRDF: dielectric and metal reflectance
mixed per material, a hemispherical sky/ground ambient so a shaded wall is not
black, an environment term through a roughness-aware Fresnel, and emissive
strength for lit windows and lamps. Moving geometry deliberately uses solid
materials, because a world-locked projection on a car at 20 m/s slides its paint
across its own body.

**Geometry.** `geometry.mjs` is a detail kit: window units with reveals, frames,
transoms and sills; doors with steps and awnings; cornices, parapets, gable and
sawtooth roofs; fire escapes; street lamps with arms over the carriageway;
traffic lights; containers, pallets, crates, bins, hydrants, benches, shelters.
`city.mjs` composes it per building and per road. The kit is the single vertex
emission point, so a material index passed into a colour argument is a throw,
not a silently wrong vertex.

**Measured here:**

| | naive detail | with LOD |
|---|---|---|
| Static vertices | 1,420,680 | 218,226 |
| Static buffer | 54.2 MB | 8.3 MB |
| Static build | 668 ms | 99 ms |
| Dynamic build | 10.11 ms | 2.79 ms |

### The shader, and why it has its own test suites

The whole material system once ran fully green against a fragment shader that
read only position, normal and colour. The textures were decoded, uploaded and
bound every frame and never sampled; the material index was uploaded as vertex
attribute 3 and never read. Every test passed, because every test asserted the
*upload* — which was fine — rather than that anything was drawn with it.

The harness cannot catch that class of bug, and cannot catch a GLSL syntax error
either, because it stores shader source and reports `COMPILE_STATUS` true for
everything. So there are two suites that can:

- `test_shaders.mjs` parses all four shaders with `@shaderfrog/glsl-parser` and
  asserts the wiring in both directions — every uniform the renderer sets is
  declared in a shader, and every uniform a shader declares is set — plus that
  the vertex shader reads all four attributes the buffers upload.
- `test_shading.mjs` transliterates the BRDF into JS and asserts its behaviour,
  because a shader that parses can still be wrong. Lambert's cosine law, the
  specular lobe actually widening with roughness rather than merely dimming,
  4% dielectric reflectance against a metal's albedo, and a 7,776-case sweep
  for non-finite or negative output.

`docs/ASSET_PIPELINE.md` is the full reference, including the harness texture
model, what it cannot verify, and what is not done: no interiors, no downloaded
models, no decals, no triplanar terrain blending, no vegetation impostors, and
no measured frame rate.

## Current limitations

The world is no longer untextured primitives: 26 curated CC0 PBR materials are processed, validated and shipped as three texture arrays, and the buildings, roads, vehicles, street furniture and vegetation are built from a detail kit of real small solids with distance LOD. The strongest remaining gaps are full navmesh/pathfinding, more sophisticated intersection traffic logic, true GPU-time instrumentation, screen-space reflections, interior traversal, and pixel-level temporal reprojection. Those are not represented as completed features.

Two specific known gaps in what is built:

- **Frustum culling is per-cell, not per-object.** A 480-unit cell is drawn if
  *any* part of it is on screen, so a cell straddling the view edge submits all
  of its geometry. That is the standard trade and it is the right one at this
  cell size — roughly 33% of static geometry is rejected at the default view —
  but it is a floor, not a maximum. Per-object culling would need a per-object
  draw or an indirect/multi-draw path.
- **Shadows are projected quads, not a shadow map.** They are real geometry
  depth-tested against the terrain, so they are correct as a shadow *shape*,
  but they do not fall on other buildings and cannot self-shadow. A real
  shadow map is blocked on the headless harness, which now models and validates
  textures but still models no framebuffers or renderbuffers — see
  `tools/headless_runtime.mjs`.

On-device FPS, GPU time and VRAM are still unmeasured: the runtime is verified headlessly, but a real GPU measurement path must be validated on the target device before any of those numbers are quoted.


## Native engine

The native engine is upstream-only for infrastructure that has a mature open-source implementation. There are no hand-rolled replacements for physics, navigation, ECS, mesh optimisation, audio or Vulkan memory management.

### What is actually built and verified

`emergent_native` compiles and links against all eight pinned upstream libraries, and its behaviour is covered by `ctest`. This was verified by actually running the build, not by inspection:

| Subsystem | Library | Pinned at | State |
|---|---|---|---|
| Rigid-body physics | Jolt Physics | `v5.6.0` | Compiled, linked, **22 behaviour tests pass** — gravity, resting contact, slab geometry, lifecycle |
| Skeletal animation | ozz-animation | `0.17.0` | **188 symbols in the linked binary.** Bakes a procedural biped rig and idle/walk/run clips, samples and blends them through ozz's runtime jobs. **84 behaviour checks pass** |
| Asset packs | Zstandard | `v1.5.7` | **450 symbols in the linked binary.** Packs are written, read and integrity-checked on every self-test run. **83 behaviour checks pass**, including corruption, truncation and header attacks |
| Profiling | Tracy (opt-in) + built-in zone accounting | `v0.13.0` | Zone accounting always on and verified; Tracy compiles in with `-DEMERGENT_ENABLE_TRACY=ON`. **48 behaviour checks pass** |
| Frame loop | Jolt + ozz, behind `FrameLoop` | — | **125 behaviour checks pass.** Fixed 1/60s physics, variable-rate presentation, interpolated render state. Frame-rate independence and bit-exact determinism both asserted |
| Renderer | Vulkan 1.3 via Volk + VMA | `vulkan-sdk-1.4.328.0` | **Compiled and linked only — never executed.** No ICD and no `/dev/dri` here. Headless backend's frame-state validation is tested; the GPU path is not |
| Mesh optimisation | meshoptimizer | `v1.2` | Compiled, **runs at runtime** (`BOUNDARY_READY`) |
| Entity-component world | Flecs | `v4.0.5` | Compiled, **runs at runtime** (`ACTIVE`, entities created and updated) |
| Audio | miniaudio | `0.11.25` | Compiled, **engine initialises at runtime** (`ACTIVE`) |
| Navigation | Recast/Detour | `v1.6.0` | Compiled and linked; crowd + navmesh-query boundary |
| Vulkan loader dispatch | Volk | `vulkan-sdk-1.4.328.0` | Compiled and linked |
| GPU memory allocation | Vulkan Memory Allocator | `v3.3.0` | Compiled and linked; VMA implementation TU + volk function import |

Symbol counts are `nm -C emergent_native | grep -c`, not a claim: a static archive that nothing calls is dropped by the linker, which is exactly how ozz sat at 0 symbols in the previous revision.

```
$ ./build/native/emergent_native
meshoptimizer: BOUNDARY_READY
Jolt simulation: ACTIVE first_dynamic_y=0.48
Flecs ECS: ACTIVE entities=2
miniaudio: ACTIVE
frame loop: ACTIVE frames=120 steps=2 sim=2.00s alpha=0.000000 clip=run pos=(0.00,0.49,13.69) joints=10 boxes=25
ozz animation: ACTIVE joints=10 clips=3 head_y=0.518
Zstandard packs: ACTIVE entries=2 ratio=0.1068 manifest=round-tripped
Native scene self-test: objects=50000 visible=35514 culled=14486 CPU_ms=0.108119
Profiling: 8 zones, total 3.19182 ms -> emergent_profile.json
Tracy: not compiled in (-DEMERGENT_ENABLE_TRACY=ON)

$ ctest --test-dir build/native
100% tests passed, 0 tests failed out of 9
```

`first_dynamic_y=0.48` is a real measurement: the seeded body starts at `y=4.0`, falls under gravity, and comes to rest on the ground plane at `y≈0.5`. `head_y=0.518` is a model-space joint position resolved through ozz's `LocalToModelJob` one second into a walk cycle; the rest-pose height is `0.52`. `ratio=0.1068` is the real stored-to-original byte ratio of a pack written during that run.

### Frame loop

`native/src/frame_loop.cpp` is the thing that makes the native engine an engine
rather than a self-test. Physics runs at a fixed 1/60s step and presentation
runs at the display rate, and the two are joined by an interpolation fraction.

The split is deliberate and is the difference between a loop that is smooth and
one that is reproducible:

- **Physics is fixed.** Real time accumulates in an accumulator and drains in
  whole 1/60s steps. A solver is a function of its timestep, so feeding it a 4ms
  step one frame and a 31ms step the next produces different contact resolution
  for the same input, and the difference compounds.
- **Presentation is variable.** A frame renders at whatever real time has
  passed, with the leftover fraction `alpha`, and the character is drawn
  interpolated between the last two simulation states. Nothing that affects the
  simulation is driven from that variable delta.
- **Animation is variable too, once per frame.** A clip is a pure function of
  elapsed time with no state that can diverge, so driving it at the
  presentation rate is safe and keeps motion smooth at 144Hz. Driving it inside
  the substep loop would make a 144Hz display animate in visible 60Hz steps.

Two guards, both tested. A stalled frame is clamped to 250ms and to 8 physics
steps, and the leftover backlog is **discarded rather than carried** — carrying
it is the start of the spiral of death, where each frame falls further behind
and costs more to catch up. A non-finite delta is a failed clock, not a stall,
and is discarded rather than clamped, because clamping infinity would simulate
time that never happened.

```
$ ./build/native/emergent_native --hz 144 --frames 576 --walk
Requested 144Hz: each frame is 6.9444ms, the physics step is 16.6667ms
Frame loop: ran=576 frame=575 steps=0 sim=3.983s alpha=0.99999999999999001 (16.667ms to the next step) ...

$ ./build/native/emergent_native --hz 30 --frames 120 --walk
Requested 30Hz: each frame is 33.3333ms, the physics step is 16.6667ms
Frame loop: ran=120 frame=119 steps=2 sim=4.000s alpha=0 (0.000ms to the next step) ...
```

`alpha=0.99999999999999001` on the 144Hz line is worth a note: the accumulator has landed one unit in the last place under a step, so `alpha` is the largest double below 1. Every fixed-point format rounds that to `1`, which would read as a broken invariant. The loop pulls it back explicitly rather than relying on the division, and a test asserts `alpha < 1` across four thousand 144Hz frames.

Same four seconds, same 240 physics steps, same ending position — from 120
frames of 33ms and from 576 frames of 7ms. That is the property the loop exists
to provide, and it is asserted directly too: 60 frames at 1/60s and 30 frames
at 1/30s take the same 60 steps and end in the same place, and the same delta
and input sequence over 500 frames is bit-identical in position and pose.

One subtlety worth recording, because it is a bug that looks fine: on a frame
that takes **no** physics step — half of all frames at 144Hz — the
interpolation window has to be collapsed onto the current state. Left alone,
`previous` still holds the state from before the last stepping frame and the
renderer is handed a position *behind* the simulation.

### Rendering

`RenderBackend` (`native/include/emergent/render_backend.hpp`) is the interface
between the loop and whatever draws it, and `FrameState` is the contract: the
interpolated character transform, both simulation states it was interpolated
from, the velocity, the resolved ozz joint matrices, and the drawable scene
boxes with entry 0 being the character.

`VulkanRenderBackend` implements it for real: render pass, depth target,
graphics pipeline, an instanced draw of the scene boxes and the skeleton driven
straight from the pose matrices, per-frame buffer uploads, and
acquire/submit/present. It takes a swapchain when a window is attached and an
offscreen colour+depth target when one is not.

`NullRenderBackend` is the headless implementation and is not a stub. It
validates every frame state — non-finite transforms, alpha outside `[0,1)`, a
joint count without matrices, a scene box count without boxes — and refuses to
count a rejected frame as submitted. A loop that produces a NaN fails there, on
a machine with no GPU, instead of becoming a lost device on someone else's.

The GLSL in `native/shaders/` is compiled to SPIR-V at build time when `glslc`
or `glslangValidator` is present, and skipped cleanly when it is not. The
engine, the tests and CI all build and run without a shader toolchain; the
renderer reports `shader module unavailable` by name rather than failing to
start. The SPIR-V loader itself is tested directly, including truncated and
wrong-magic modules — the checks that stop a corrupt asset from reaching
`vkCreateShaderModule`, where it becomes undefined behaviour inside a driver
rather than an error.

**No line of the Vulkan render path has ever executed.** See below.

### Skeletal animation

`native/src/animation.cpp` owns the authoring half of ozz: a ten-joint biped rig and three locomotion clips, generated from joint motion curves in code and baked once through `ozz::animation::offline::SkeletonBuilder` and `AnimationBuilder`. Playback uses `SamplingJob`, `BlendingJob` and `LocalToModelJob`.

That means there is no skeleton asset to ship, no offline tool to run, and no binary blob that can go stale — and the rig is still a real rig, evaluated by the real runtime. The depth-first joint ordering ozz requires is asserted at bake time, because a wrong order silently animates the wrong bones.

`AnimationLibrary`, `Animator` and `Pose` expose only names, floats and matrices; no ozz type crosses the header boundary.

### Content packs

`native/src/asset_pack.cpp` defines `.ezpk`: a 48-byte header, one zstd frame per entry, and a trailing zstd frame holding the index. Entries are independently compressed, so one bad entry does not damage the rest.

Integrity comes from zstd, not from a checksum EMERGENT invented. One catch worth recording: **`ZSTD_c_checksumFlag` is off by default**, so the obvious `ZSTD_compress()` produces frames that decode happily after corruption. The writer explicitly requests the checksum and the reader refuses any frame that does not carry one, which is what makes the corruption tests real rather than decorative.

Every offset in the header is range-checked by subtraction rather than addition, so a hostile index cannot overflow the check and steer a read outside the buffer.

### Profiling

`emergent::profile::Scope` times a named region and accumulates calls, total, minimum and maximum. It allocates nothing after start-up: a fixed 64-entry table, and a run that overflows it reports `droppedScopes` rather than quietly measuring a subset.

The engine instruments its real subsystems and writes `emergent_profile.json` on every self-test run, which CI publishes as an artifact. Tracy live capture is wired behind `-DEMERGENT_ENABLE_TRACY=ON` and shares the zone names. It is **off by default and no capture is claimed**: Tracy's client streams to a running Tracy server, and a build container has none. CI builds *and runs* the Tracy-enabled variant so the option cannot rot — which it already had, in the form of a `TRACY_ENABLE` inversion that made the library build and every consumer fail to link.

### What is not integrated, and why

- **The Forge** — not integrated. It is a full rendering framework with its own RHI, windowing and build system. EMERGENT's renderer is raw Vulkan + Volk + VMA, and swapping in The Forge would be a replacement of the renderer, not an addition to it.
- **KTX / Basis Universal** — not integrated natively. The web build ships uncompressed RGBA8 arrays: a browser cannot transcode Basis at load time without a WASM decoder, and shipping 78 uncompressed tiles already costs 17.4 MB. This is the right trade for the web target and the wrong one for a native shipping build, where GPU-compressed BCn plus a real `ktx2` path is required. Not claimed either way here.
- **Slang** — not integrated. It is a shading-language compiler, and the two compute shaders in `native/shaders/` are GLSL compiled by the driver at load time. There is no offline shader-compilation step to replace, and no GPU to validate generated SPIR-V against. Not claimed.

### Vulkan runtime status

The Vulkan backend compiles, links, and is exercised for capability probing, but it cannot *execute* in a container with no GPU: there is no ICD and no `/dev/dri`, so `vkCreateInstance` correctly returns `VK_ERROR_INCOMPATIBLE_DRIVER` and the engine reports that rather than fabricating GPU work. A real GPU device is required to validate the render path.

This applies to `VulkanRenderBackend` in full. Its `open()` returns false here, with a status saying why, and nothing below it has run. The 500-odd lines under `open()` are compile- and link-checked against Vulkan 1.3 headers and nothing more; they should be treated as unexercised until someone runs them where a driver exists. The offscreen branch needs no window and will be the first thing to work there.

### No windowing dependency

There is no GLFW, SDL or XCB in this tree, and that is a decision rather than a gap. A window library owns a display connection, an event queue and a native surface handle; all three are platform-specific and none can be stubbed without inventing a fake display server. `SurfaceProvider` reduces a window to the four things a renderer actually asks of one, and creating the platform's `VkSurfaceKHR` is the provider's job — which is also why `VulkanBackend::initialize()` takes the instance extensions up front, since a surface extension cannot be added after `vkCreateInstance`.

Adding a real window is one class. Nothing in the frame loop, the physics, the animation or the render path changes to accommodate it. The trade is stated in full in `docs/OPEN_SOURCE_DECISIONS.md`.
