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


## Native dependency policy

The native engine is upstream-only for infrastructure that has a mature open-source implementation. Jolt Physics, meshoptimizer, Recast/Detour, Volk, Vulkan Memory Allocator and Flecs are required dependencies; the previous custom fallback implementations have been removed. The native renderer uses the real Vulkan SDK headers plus Volk and VMA. The Forge/Ozz/miniaudio/KTX/Slang/Tracy/Zstandard are not claimed as integrated until their upstream sources are supplied and wired against their real APIs.
