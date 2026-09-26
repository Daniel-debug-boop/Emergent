# CHANGELOG

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
