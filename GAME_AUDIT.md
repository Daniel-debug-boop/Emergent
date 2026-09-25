# GAME_AUDIT.md

## Scope

Audit of the supplied EMERGENT-3D codebase before the completion pass, followed by verification of the evolved build.

## Pre-completion findings

| Priority | Finding | Player impact | Frequency | Severity | Feasibility | Action |
|---|---|---:|---:|---:|---:|---|
| 1 | Interaction was effectively a business purchase, not a gameplay loop | High | High | High | High | Replaced with world-state delivery missions + NPC/business interactions |
| 2 | Dynamic 3D geometry rebuilt too aggressively | High | High | High | High | Added adaptive dynamic-buffer reuse and per-NPC temporal update cadence |
| 3 | Player collision checked every building | Medium | High | High | High | Replaced full-world scan with spatial building index |
| 4 | Persistence omitted most live NPC/business/player state | High | Medium | High | High | Save/load now serializes player, businesses, NPC state, mission, events and world state |
| 5 | Traffic was straight-line movement without following/braking | Medium | High | Medium | Medium | Added lane offsets, target speed, following slowdown and incident events |
| 6 | Renderer was visually skeletal | High | High | Medium | High | Added architectural variation, windows/doors/signs, sidewalks, low-poly trees, cars, characters, water and geometric shadows |
| 7 | Player-facing UI was mostly telemetry | High | High | Medium | High | Replaced with objective/location/currency/energy HUD and isolated developer telemetry |
| 8 | No mobile control path | Medium | Medium | Medium | High | Added touch joystick and action buttons |
| 9 | No explicit quality scalability | Medium | Medium | Medium | High | Added LOW/MEDIUM/HIGH/ULTRA modes |
| 10 | No reproducible 3D benchmark query mode | High | Low | High | High | Added standard/adaptive benchmark URLs and JSON results |

## Completion-pass verification

Source checks pass with Node syntax validation and deterministic world tests. The reference seed currently produces 64 regions, 50 districts, 672 roads, 3,077 buildings, 1,838 trees, 714 businesses, 520 NPCs and 150 vehicles.

The local HTTP server serves `index.html`, `world.mjs` and `game3d.js` successfully.

The browser smoke path could load the page only after a local-page policy was bypassed for static testing; the available Chromium runtime then failed before game initialization because it could not create WebGL2. Therefore browser FPS and GPU timings are intentionally left as on-device measurements.

## Biggest remaining risks

The largest remaining player-facing limitations are stylized low-poly visuals, point-to-point NPC navigation instead of pathfinding, simplified road intersections, limited mission variety, lack of true interiors, and lack of browser GPU timing in the provided environment.


## Autonomous execution pass — 2026-09-24

### Verified
- `npm test`: PASS.
- `npm run check`: PASS.
- CMake Release build: PASS.
- Native executable: PASS in capability-probe/self-test mode.
- Native benchmark: PASS; CPU-only measurements were produced.
- CTest: PASS after adding native self-test and benchmark smoke tests.
- HTTP serving of the game entry point: PASS.

### Implemented in this pass
- Explicit render-chunk residency cache: active chunks are materialized from the spatial index, reused while resident, and evicted when they leave the stream window.
- Stream state now accounts for cell, radius, quality, and renderer mode instead of only the player cell.
- Fixed business spatial-index insertion to use the actual business building position.
- NPC simulation LOD now has four levels: near/full, medium/reduced cadence, far/reduced cadence, and very-far aggregate state.
- Native build now has CTest entries for executable self-test and benchmark smoke coverage.

### Still blocked by environment
- A real Vulkan physical device/ICD is not available to the supplied runtime, so swapchain/presentation and GPU rendering cannot honestly be claimed as executed here.
- Browser WebGL gameplay timing could not be captured reliably in the available headless Chromium configuration.

### Next highest-value implementation target
Replace the bespoke native Vulkan symbol/memory bootstrap with a proper Vulkan SDK path using Volk + Vulkan Memory Allocator when the repository is built on a machine with Vulkan headers/ICD, then wire the first actual native triangle/scene pipeline before adding GPU culling or mesh-shader claims.
