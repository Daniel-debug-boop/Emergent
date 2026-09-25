# EMERGENT — Living World 3D

EMERGENT is the evolved version of the original playable 2D procedural prototype. The simulation and deterministic world model were preserved; the player-facing runtime is now a self-contained WebGL2 3D game.

## Run

```bash
./run.sh
```

Then open the printed local URL in a WebGL2-capable browser. `run.sh` uses Python's static HTTP server, so no npm install is required.

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

Use the actual game page with:

- `?benchmark=standard&seed=173927&quality=HIGH`
- `?benchmark=adaptive&seed=173927&quality=HIGH`

After about 8 seconds the page writes a JSON result to `document.body.dataset.benchmark`.

Reported measurements include FPS, CPU-side render wall time, dynamic-buffer build timing, recomputation/reuse counts, visible-object counts, geometry counts and streaming operations.

GPU time and VRAM are not fabricated. They require a trustworthy browser/GPU measurement path on the target device.

See `benchmark.md` for the reproducible procedure and `FINAL_SCORECARD.md` for the completion audit.

## Engineering verification

```bash
npm test
npm run check
node test_project.mjs
```

These tests cover deterministic world reproduction, required world populations, required renderer/game systems in the source, HTML/JS references, and finite terrain sampling.

## Current limitations

The game remains intentionally stylized and low-poly. The strongest remaining technical gaps are full navmesh/pathfinding, more sophisticated intersection traffic logic, true GPU-time instrumentation, advanced PBR materials/reflections, interior traversal, and pixel-level temporal reprojection. Those are not represented as completed features.


## Native dependency policy

The native engine is upstream-only for infrastructure that has a mature open-source implementation. Jolt Physics, meshoptimizer, Recast/Detour, Volk, Vulkan Memory Allocator and Flecs are required dependencies; the previous custom fallback implementations have been removed. The native renderer uses the real Vulkan SDK headers plus Volk and VMA. The Forge/Ozz/miniaudio/KTX/Slang/Tracy/Zstandard are not claimed as integrated until their upstream sources are supplied and wired against their real APIs.
