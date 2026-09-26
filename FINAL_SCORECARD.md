# FINAL_SCORECARD.md

## Acceptance scorecard

| Category | Evidence | Status / remaining weakness |
|---|---|---|
| Playability | Third-person movement, first-person toggle, sprint, interaction, procedural delivery loop, progression, touch controls | Strong vertical-slice game; still a compact open-world experience rather than a content-heavy commercial game |
| World quality | Height-field terrain, biome variation, river, 50 districts, roads, sidewalks, buildings, vegetation, atmosphere | Strong generated foundation; terrain/architecture remain stylized |
| Procedural generation | Seeded hierarchical generator; building placement biased toward roads/districts; businesses derive from buildings; NPC destinations derive from districts | Strong; road network remains rule-based/grid-like rather than a full road-graph generator |
| NPC behavior | Persistent identity, job, home/work, schedule, goals, energy, money, mood, memory, weather responses | Moderate/strong; no navmesh, pathfinding or deep conversation/relationship simulation |
| Emergent simulation | Stock pressure, price changes, open/close behavior, traffic incidents, weather effects, mission consequences | Strong core interactions; event reactions are intentionally lightweight |
| Traffic | Lane offsets, speed targets, following slowdown, rain slowdown, local incident generation | Moderate; intersection priority/signals and robust collision solving are not implemented |
| Missions | Generated delivery from business/world state, pickup/delivery stages, rewards, stock/reputation consequences | Strong for the slice; currently one main mission archetype |
| Visual quality | Real WebGL2 3D, procedural architecture, tree meshes, 3D characters/cars, fog, water, geometric shadows, day/night | Moderate; no PBR texture pipeline, high-end reflections or advanced character animation |
| Lighting | Directional sun, ambient term, day/night variation, fog and a geometric shadow pass | Strong stylized lighting; not cascaded shadow maps or physically based lighting |
| Weather | Clear/mist/rain; affects movement, NPC behavior, traffic and business demand; audio ambience changes | Good systems integration; weather effects remain visually light |
| Performance | Player-centered streaming, spatial collision index, O(1) mission lookups, adaptive dynamic-buffer update period, importance-driven NPC reuse, quality levels | Measured headlessly (STANDARD 1737 ms vs ADAPTIVE 974 ms for 300 frames); on-device FPS still unverified |
| Adaptive rendering | STANDARD vs ADAPTIVE; distance/motion/weather/importance; temporal reuse of dynamic representations | Real selective 3D recomputation; not pixel-level temporal reprojection |
| Streaming | Render geometry rebuilt around player cells; detail varies with quality and distance | Strong render streaming; full data unloading/abstract simulation is not yet implemented |
| Stability | Real game frame loop executed headlessly against a validating WebGL2 surface (8 tests / 78 assertions); native engine compiled, linked and behaviour-tested (Jolt 22 assertions, ctest 3/3) | Crashes, validation errors, NaN geometry and broken mission/save flows now fail the build; a real GPU device is still untested |
| Save/load | Player, mission, events, discoveries, business state, NPC state, world clock/economy/weather persisted | Strong localStorage persistence; cloud/slot management is not included |
| Overall polish | Coherent gameplay loop, UI, controls, audio ambience, progression and isolated developer telemetry | A polished research-game vertical slice within the current constraints; not AAA content/asset quality |

## Verification record

Passing commands:

```bash
npm test          # world + math + project + tooling + runtime suites
npm run check     # syntax check

# native (requires the pinned upstream trees; see docs/ENGINE_VERIFICATION.md)
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEMERGENT_JOLT_DIR=... -DEMERGENT_MESHOPTIMIZER_DIR=... -DEMERGENT_RECAST_DIR=... \
  -DEMERGENT_VOLK_DIR=... -DEMERGENT_VMA_DIR=... -DEMERGENT_FLECS_DIR=... \
  -DEMERGENT_MINIAUDIO_DIR=... -DEMERGENT_OZZ_DIR=... \
  -DEMERGENT_ZSTD_DIR=... -DEMERGENT_TRACY_DIR=...
cmake --build build/native --target emergent_native emergent_benchmark \
  emergent_physics_test emergent_animation_test emergent_asset_pack_test \
  emergent_profiling_test
ctest --test-dir build/native --output-on-failure
```

`npm test` runs five suites in roughly ten seconds: deterministic world
generation, matrix/projection algebra, project wiring, preview-server and build
tooling, and a runtime suite that boots the **unmodified** game and executes its
real frame loop, simulation, streaming and WebGL draw calls inside Node.

The native side is no longer a source tree of unbuilt code. `emergent_native`
compiles and links against all ten pinned upstream libraries and runs them:
meshoptimizer, Flecs, miniaudio and Jolt report active at runtime, ozz resolves
a real skeletal pose (`head_y=0.518`), and a Zstandard pack round-trips at a
measured 0.1068 stored-to-original ratio. `ctest` passes 6/6 — the original
22-assertion Jolt behaviour test plus 215 new checks across animation (84),
asset packs (83) and profiling (48).

Building it exposed defects that had been invisible precisely because nothing
had ever compiled it — a CMake tag that does not exist upstream, Jolt added from
the wrong directory, a meshoptimizer API change, a Recast API removal, and a VMA
instantiation that was never provided at all. The later passes found more of
the same kind: a rig joint table in the wrong traversal order, an ozz sampling
context sized in the wrong units, and — the one worth remembering — an asset
format that *looked* integrity-checked because `ZSTD_decompress` was used,
when `ZSTD_c_checksumFlag` is off by default and corrupt frames decode
perfectly well. See `docs/ENGINE_VERIFICATION.md`.

Every "integrated" claim for a static archive is checked with
`nm -C emergent_native | grep -c`, because an archive nothing calls is dropped
by the linker and still builds cleanly. ozz sat at 0 symbols in the previous
revision; it is at 188 now.

Measured CPU-side world-generation and headless benchmark timings are recorded in
`benchmark.md`.

No FPS, GPU time or VRAM figures are reported for this build: a real GPU
measurement path has not yet been validated on a target device, so none is
claimed.
