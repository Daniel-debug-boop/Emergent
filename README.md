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

Explore the world → discover districts → find businesses → pick up an emergent delivery → travel to the target → deliver → receive money/progression → alter local business stock → continue exploring.

The delivery target is selected from world state. Businesses with low stock can become mission targets, and successful delivery changes their stock, reputation and openness.

## Controls

Desktop: `WASD` / arrows move, `Shift` sprint, click + mouse look, `E` interact, `T` standard/adaptive renderer, `Q` quality, `V` first/third-person camera, `F2` save, `F3` load, `D` developer telemetry, `N` new world.

Mobile: virtual joystick, RUN and E buttons appear automatically on narrow screens.

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
| World | `npm run test:world` | Generation is deterministic per seed and varies across seeds; required populations exist |
| Math | `npm run test:math` | Projection, view, multiply and point-transform algebra, including degenerate cases |
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
something). It covers boot, a long run, keyboard movement, streaming under
teleport, every quality level, both renderer modes, a full delivery mission, and
save/load including rejection of a save from a different world.

## Current limitations

The game remains intentionally stylized and low-poly. The strongest remaining technical gaps are full navmesh/pathfinding, more sophisticated intersection traffic logic, true GPU-time instrumentation, advanced PBR materials/reflections, interior traversal, and pixel-level temporal reprojection. Those are not represented as completed features.

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
ozz animation: ACTIVE joints=10 clips=3 head_y=0.518
Zstandard packs: ACTIVE entries=2 ratio=0.1068 manifest=round-tripped
Native scene self-test: objects=50000 visible=35514 culled=14486 CPU_ms=0.133482
Profiling: 6 zones, total 1.47402 ms -> emergent_profile.json
Tracy: not compiled in (-DEMERGENT_ENABLE_TRACY=ON)

$ ctest --test-dir build/native
100% tests passed, 0 tests failed out of 6
```

`first_dynamic_y=0.48` is a real measurement: the seeded body starts at `y=4.0`, falls under gravity, and comes to rest on the ground plane at `y≈0.5`. `head_y=0.518` is a model-space joint position resolved through ozz's `LocalToModelJob` one second into a walk cycle; the rest-pose height is `0.52`. `ratio=0.1068` is the real stored-to-original byte ratio of a pack written during that run.

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
- **KTX / Basis Universal** — not integrated. A compressed-texture path needs authored `.ktx2` assets to decode; the tree has no texture assets and no GPU here to validate an upload against, so a KTX integration would be a decoder with nothing to decode. Not claimed.
- **Slang** — not integrated. It is a shading-language compiler, and the two compute shaders in `native/shaders/` are GLSL compiled by the driver at load time. There is no offline shader-compilation step to replace, and no GPU to validate generated SPIR-V against. Not claimed.

### Vulkan runtime status

The Vulkan backend compiles, links, and is exercised for capability probing, but it cannot *execute* in a container with no GPU: there is no ICD and no `/dev/dri`, so `vkCreateInstance` correctly returns `VK_ERROR_INCOMPATIBLE_DRIVER` and the engine reports that rather than fabricating GPU work. A real GPU device is required to validate the render path.
