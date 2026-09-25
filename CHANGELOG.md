# CHANGELOG

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
