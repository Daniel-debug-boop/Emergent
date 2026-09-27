# Engine verification — 2026-09-25

This supersedes the 2026-09-24 record, which described a build that had in fact
never been configured against its dependencies.

## What the previous record got wrong

The 2026-09-24 note claimed "CMake Release configure/build with upstream
fetching disabled" and "Ctest: 2/2 tests passed". Configuring with upstream
fetching disabled does not build the native engine: with
`EMERGENT_REQUIRE_UPSTREAM=ON` (the default) the configure step stops with
`FATAL_ERROR` as soon as a required target is missing. Nothing in the native
tree had ever been compiled.

Four defects were hiding in that uncompiled state, all of which a compiler finds
immediately:

1. `CMakeLists.txt` pinned Volk at `GIT_TAG 1.4.328`. Volk tags its releases
   `vulkan-sdk-<version>`, so no such tag exists and `EMERGENT_FETCH_DEPS=ON`
   could never have succeeded.
2. `CMakeLists.txt` pointed at the Jolt repository root, which has no
   `CMakeLists.txt`; Jolt's entry point is `Build/CMakeLists.txt`.
3. `optimization.cpp` cast the result of `meshopt_analyzeVertexCache` to
   `float`. meshoptimizer >= 0.22 returns a `meshopt_VertexCacheStatistics`
   struct, so this was a hard compile error.
4. `navigation.cpp` called `dtCrowd::getEditableQuery()`. Recast 1.6 removed
   it; the world now projects destination points through the `dtNavMeshQuery`
   it owns.

The Vulkan Memory Allocator was also never actually instantiated. VMA ships as
a header-only `INTERFACE` CMake target, so the allocator's symbols must be
emitted by a translation unit that defines `VMA_IMPLEMENTATION`. The link
failed with undefined references to `vmaCreateAllocator`, `vmaCreateImage` and
`vmaDestroyImage` until `native/src/vma_impl.cpp` was added. Because the build
enables `VMA_DYNAMIC_VULKAN_FUNCTIONS`, that unit also needs the volk dispatch
tables imported before the allocator is created, which
`VulkanBackend::initialize()` now does.

## Verified in this environment

Toolchain: GCC 11.4, CMake 3.30.5, Ninja 1.10.1, Vulkan headers 1.3.204.

Pinned upstream trees were cloned at the exact revisions in `CMakeLists.txt`
and passed via the `EMERGENT_*_DIR` variables. Configure, build and test:

```
$ cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DEMERGENT_JOLT_DIR=$PWD/third_party/JoltPhysics ... (all eight)
-- Build files have been written to: .../build/native

$ cmake --build build/native --target emergent_native emergent_benchmark
[154/154] Linking CXX executable emergent_native
```

```
$ ./build/native/emergent_native
meshoptimizer: BOUNDARY_READY
Flecs ECS: ACTIVE entities=2
miniaudio: ACTIVE
Jolt simulation: ACTIVE first_dynamic_y=0.48
Native scene self-test: objects=50000 visible=35514 culled=14486 CPU_ms=0.136892

$ ctest --test-dir build/native
100% tests passed, 0 tests failed out of 3
```

`emergent_physics_test` is a behaviour test, not a smoke test. It drives the
real Jolt world through 22 assertions covering lifecycle and idempotence,
gravity, resting contact height, non-ejection from level geometry, thin static
slabs, finiteness over 600+ steps, rejection of non-positive and zero extents,
ignored non-positive timesteps, and post-shutdown safety.

## Environment limitations that remain

**No Vulkan device.** The container has no ICD (`/usr/share/vulkan/icd.d` is
empty) and no `/dev/dri`, so `vkCreateInstance` returns
`VK_ERROR_INCOMPATIBLE_DRIVER` (`-9`). The Vulkan backend is compiled, linked
and exercised for capability probing; it cannot submit real GPU work here and
no GPU timing is claimed. A machine with a real driver is required to validate
the render path end to end.

**Single CPU core.** The full native build is dominated by Jolt, Flecs and ozz.
That affects build time only, not correctness.

**ozz is now exercised (superseded 2026-09-26).** The 2026-09-25 record
reported zero ozz symbols in the linked binary: the libraries built, nothing
called them, and the linker dropped both archives. That is fixed —
`native/src/animation.cpp` bakes a procedural biped rig and three locomotion
clips and drives them through ozz's runtime jobs, and `NativeEngine` plays a
walk cycle during the self-test.

```
$ nm -C build/native/emergent_native | grep -c "ozz::"    # -> 188
$ nm -C build/native/emergent_native | grep -c "JPH::"    # -> 4656
```

The same `nm` check is the reason to distrust any "integrated" claim about a
static archive. It is cheap, and it is the only thing that distinguishes a
dependency from a build cost.

## 2026-09-26 — ozz, Zstandard and profiling integrated

The 2026-09-25 record ended with a list of things the README implied and the
tree did not have. Three are now integrated and verified; two are documented as
exclusions.

**ozz-animation `0.17.0`.** `AnimationLibrary` bakes a ten-joint biped rig and
idle/walk/run clips from joint motion curves expressed in code, using ozz's
offline `SkeletonBuilder` and `AnimationBuilder`. `Animator` and `Pose` play
them through `SamplingJob`, `BlendingJob` and `LocalToModelJob`. 84 behaviour
checks, including that a bone above the hips in the hierarchy ends up above it
in model space, that time actually moves the pose, that looping wraps and a
non-looping clip clamps, and that a zero-weight layer leaves the pose untouched.

Two defects were found by building and running it:

- The rig's joint table was **not in depth-first order**. ozz stores joints in
  the order `IterateJointsDF` visits them and `LocalToModelJob` resolves each
  joint from its parent's earlier index, so a table that reads plausibly but is
  ordered breadth-first animates the wrong bones with no error anywhere. The
  bake now compares the baked joint names against the table and refuses to
  produce a rig that disagrees — which is how this was caught.
- `SamplingJob::Context`'s constructor takes a **track** count and rounds up to
  SoA slots internally. Passing `num_soa_tracks()` (3) instead of `num_tracks()`
  (10) yields a one-slot context, and every `SamplingJob::Run()` fails
  validation with no message.

**Zstandard `v1.5.7`.** `.ezpk` packs: a 48-byte header, one zstd frame per
entry, a trailing frame holding the index. 83 behaviour checks, including
round-trips of text, binary, empty and 256 KiB payloads, and attacks: broken
magic, unsupported version, truncation inside the index, a header with no body,
a file shorter than a header, an impossible entry count, and a flipped payload
byte.

The finding that matters: **`ZSTD_c_checksumFlag` is off by default.** The
obvious implementation — `ZSTD_compress` plus `ZSTD_decompress` — looks like it
has integrity checking and does not; frames produced that way decode cleanly
after corruption. Measured directly:

```
before:  flipping a byte at offset 64 of a payload -> read succeeded, 65536 bytes out
after:   flipping the same byte                    -> "zstd decompression failed: Restored error"
```

The writer now sets `ZSTD_c_checksumFlag` explicitly, and the reader inspects
the frame header with `ZSTD_getFrameHeader` before allocating anything and
refuses any frame that carries no checksum or disagrees with the index.

Header offsets are range-checked by subtraction rather than addition, so a
hostile index cannot carry the sum past `UINT64_MAX` and slip past the check.

**Profiling.** `emergent::profile::Scope` accumulates per-zone call counts and
wall time into a fixed 64-entry table that allocates nothing after start-up; an
over-instrumented run reports `droppedScopes` instead of measuring a subset in
silence. `emergent_native` instruments physics stepping, ECS updates, mesh
optimisation, animation sampling/resolve and the scene cull, and writes
`emergent_profile.json`, which CI uploads as an artifact. 48 behaviour checks,
of which the load-bearing ones drive the real physics and animation subsystems
rather than a synthetic loop.

Tracy `v0.13.0` is wired behind `-DEMERGENT_ENABLE_TRACY=ON`, sharing the zone
names. It is off by default and **no capture is claimed**: Tracy's client
streams to a running Tracy server, and a build container has none. There is
deliberately no `tracyConnected()` predicate, because Tracy exposes no
connection state and reporting one would be a fabricated measurement. CI builds
*and runs* the Tracy-enabled variant so the option cannot decay into a flag that
no longer compiles.

Building that variant is what found a real defect in it. The CMake block forced
`TRACY_ENABLE OFF`, on the reasoning that not building the profiler meant not
turning profiling on. It is the opposite: with `TRACY_ENABLE` off,
`TracyClient.cpp` compiles to a stub defining none of the profiler API, so the
profiling library built cleanly and every consumer then failed to link:

```
undefined reference to `tracy::GetProfiler()'
```

The same block set `TRACY_UPLOAD`, which does not exist in Tracy v0.13.0. Both
fixed. The Tracy-enabled configuration is now verified directly:

```
$ ctest --test-dir build/tracychk        # -DEMERGENT_ENABLE_TRACY=ON -DEMERGENT_BUILD_ENGINE=OFF
100% tests passed, 0 tests failed out of 4
$ ./emergent_profiling_test | grep tracy:
        tracy: compiled in; a capture needs a running Tracy server
$ nm -C emergent_profiling_test | grep -c "tracy::"   # -> 477
```

Both documented configurations are therefore exercised: the default one
(6/6 tests, all upstream libraries linked) and the Tracy-enabled standalone one
(4/4 tests, Tracy linked, no capture claimed).

**Verified run** (GCC 11.4, CMake 3.30.5, Ninja 1.10.1, Vulkan headers 1.3.204):

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

`head_y=0.518` is a model-space joint position resolved through
`LocalToModelJob` one second into a walk cycle; the rest-pose height is `0.52`.
`ratio=0.1068` is the stored-to-original byte ratio of a pack written during
that run.

## 2026-09-26 — the frame loop, and a renderer that says what it did

### What was added

`native/include/emergent/frame_loop.hpp`, `native/src/frame_loop.cpp`, and
`native/tests/frame_loop_test.cpp` (125 behaviour checks).

The loop runs a **fixed** 1/60s physics step and a **variable** render rate,
joined by an interpolation fraction:

- Real time accumulates and drains in whole 1/60s steps, so the number of
  steps taken over one second of simulated time is 60 whatever the frame rate.
- A frame renders at the leftover fraction `alpha`, with the character's
  transform interpolated between the last two simulation states.
- Animation is advanced **once per frame by the real delta**, never per physics
  substep, and is resolved into joint matrices for the renderer.
- Input is applied per **substep**, so input integrated over 50ms produces the
  same result whether that 50ms arrived as one frame or three.

Two guards, both tested:

- A frame's real delta is clamped to `maxFrameDelta` (250ms) and its step count
  to `maxSubSteps` (8). Hitting the cap sets `stepsDropped` and discards the
  backlog, because carrying it forward is the start of the spiral of death.
- A non-finite or negative delta is treated as zero, not clamped. An infinite
  delta is a failed clock, not a stall, and clamping it would simulate time that
  never happened.

`JoltPhysicsWorld` gained stable body handles plus `bodyPosition`,
`bodyVelocity`, `setBodyVelocity` and `setBodyPosition`. A frame loop cannot
read or drive a body through `firstDynamicY()`.

### Verified here

`emergent_frame_loop` — 125 checks, all passing, 12 consecutive runs, no flakes.
The three that matter most:

- **Frame-rate independence.** 60 frames at 1/60s and 30 frames at 1/30s take
  the same 60 physics steps and end with the character in the same place to
  1e-4. Confirmed end to end through the shipped binary as well: 4 seconds of
  `--walk` at 30Hz, 90Hz and 144Hz all end at `pos z=16.11`.
- **Determinism.** The same delta sequence and input sequence over 500 frames
  produces bit-identical positions and bit-identical joint matrices. Jolt's
  multi-threaded solve is deterministic for a fixed thread count, which is what
  this run has.
- **Interpolation correctness.** The rendered position equals
  `lerp(previous, next, alpha)` exactly. On a frame that takes no step —
  half of all frames at 144Hz — the window collapses so `previous == next ==
  position` and nothing is drawn behind the simulation.

Also verified: alpha stays in [0,1) over 400 irregular frames and over 4000
144Hz frames; animation advances exactly one real delta on a four-substep
frame; locomotion selects run/walk/idle from speed with hysteresis; a diagonal
input is normalised so it is not 41% faster than a cardinal; a jump leaves the
ground and holding the button does not re-trigger it; walking into the prop
ring stops the character; `reset()` returns it to spawn.

### Three real bugs found while building this

Recorded because each of them was invisible until something was actually
asserted, and each is the kind of thing that ships.

- **`endFrame()` counted rejected frames as submitted.** `NullRenderBackend`
  incremented `submittedFrames` on every `endFrame()` regardless of whether
  `drawFrame()` had accepted the frame. Four malformed frames turned one
  submitted frame into five. A frame counter that over-reports is worse than
  no counter, because it is the number people believe. `endFrame()` now
  consults whether the frame was actually drawn.
- **`alpha` could equal exactly 1.0.** The documented contract is `[0,1)`. On
  a frame that took no step, `accum` is provably below `fixedDelta`, but
  dividing `(fixedDelta - 1ulp) / fixedDelta` rounds to exactly `1.0` in
  double. This is the *common* case at 144Hz, not a corner. Harmless to an
  interpolator, which produces the same point either way, and a violation of
  the contract to everyone else. The loop now pulls it back to the largest
  double below 1, and a test asserts it over four thousand 144Hz frames.
- **The character spawned at Jolt's seeded height, not the configured one.**
  `playerSpawnY` was only applied by `reset()`, so a fresh loop started with the
  body four units up and fell out of frame one. Every test that settled the
  character first hid it; the real-time run showed `y=3.63` after 0.28s and made
  it obvious.

Two smaller ones: a test expected infinity to be clamped rather than discarded
(a failed clock is not a stall, and clamping it would simulate time that never
happened), and `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` has no
`currentFormat` — the format comes from the surface's format list.

### The renderer

`native/include/emergent/render_backend.hpp` defines `RenderBackend` and
`SurfaceProvider`. `NullRenderBackend` is a real implementation, not a stub: it
validates every `FrameState` (NaN transforms, alpha outside [0,1), a joint
count without matrices, a scene box count without boxes) and refuses to count a
rejected frame as submitted. That check is what catches a broken loop on a
machine with no GPU instead of on someone else's.

`native/src/vulkan_render.cpp` implements `VulkanRenderBackend`: render pass,
depth target, graphics pipeline, instanced draw of the scene boxes and the
ozz skeleton driven by the pose matrices, per-frame uploads, an orbit camera,
and acquire/render/submit/present. It picks a real swapchain when a
`SurfaceProvider` supplies a `VkSurfaceKHR` and an offscreen colour+depth target
when one does not.

`native/src/frame_loop.cpp` also owns the drawable scene: `FrameState` carries
`sceneBoxes`, entry 0 being the character at its interpolated position.

### What is NOT verified

**No line of the Vulkan render path has ever executed.** There is no Vulkan ICD
and no `/dev/dri` in this environment, so `vkCreateInstance` returns
`VK_ERROR_INCOMPATIBLE_DRIVER` and `VulkanRenderBackend::open()` returns false
with a status explaining why. The 500 lines below `open()` are compile- and
link-verified against Vulkan 1.3 headers and nothing more. Treat them as
unexercised until someone runs them on a machine with a driver.

The GLSL in `native/shaders/` is **not compiled** here: no `glslc` and no
`glslangValidator` are installed, so no `.spv` files exist. The CMake step that
compiles them is wired in and skips cleanly when the tool is absent, and
`open()` reports `shader module unavailable` by name. `loadSpirvModule` itself
is tested directly, including the truncated and wrong-magic cases.

There is still no window. `SurfaceProvider` is the single extension point and
no implementation of it exists in this tree, so the swapchain branch of
`VulkanRenderBackend` is unexercised. The offscreen branch does not need a
window and will be the first thing to work on a GPU machine.

## 2026-09-27 — the renderer stopped drawing cubes

### What changed

`VulkanRenderBackend` used to draw instanced unit cubes. It worked, it was
visible on screen, and nothing reported a problem — because the cube path was
never wrong, it was just not the game. Three layers were missing between the
world generator and the pixels, and all three are now in place:

1. **`scene_mesh` — the world, as a drawable mesh.** `buildSceneMesh()` takes a
   `World` and emits the exact 48-byte interleaved buffer `scene_pbr.vert`
   reads: ground with real slope normals, roads, buildings with plinths, window
   openings, sills, string courses, doors, signage, parapets and roof plant,
   street furniture, trees, and water. One draw call, because the material table
   is a uniform and the index is per vertex, so there is nothing to sort by.
2. **`worldCentre()`** — the generated world is not centred on the origin. For
   seed 7 the nearest building to `(0,0)` is 424 m away and downtown is at
   `(728, 1288)`. A streamer centred on the origin streams nothing, a spawn
   point at the origin is in a field, and the player has to walk to the city
   before seeing any of it. The centre is now computed from a two-pass density
   grid, and `FrameLoop::setSpawn()` puts the player there.
3. **A second Vulkan pipeline** — `scene_pbr` with the 48-byte vertex layout,
   three `sampler2DArray` maps, a material table UBO and a per-frame scene
   uniform block, drawn before the debug boxes so a physics problem is visible
   against the world rather than floating in a void.

### Verified here

`ctest`: **14/14**, including two new suites.

- **`emergent_atmosphere`** (14 tests, 7,317 checks) — a Preetham/Wallner
  single-scattering sky with Mie, ozone and a night model that cross-fades
  rather than switches. Asserts the physical properties, not a golden image:
  the zenith is bluer than it is red, the horizon is brighter than the zenith
  and whiter, a low sun warms the haze in its own direction and not the
  opposite one, twilight does not pop, and direct sunlight is *exactly* zero at
  midnight.
- **`emergent_scene_mesh`** (12 tests, 33.5M checks) — walks the whole vertex
  buffer and asserts what a GPU would otherwise only report as a black pixel:
  every material index inside the table, every normal unit length, every
  component finite, the bounds containing every vertex, the radius actually
  bounding the slice, the triangle budget actually firing, and the facade detail
  levels actually increasing geometry.
- **`emergent_native_scene_stream`** — runs the shipped binary and asserts a
  city reached the renderer: `28 buildings, 210,628 triangles` for seed 7.
- **`test_native_shaders.mjs`** (61 checks) — parses all six GLSL files with a
  real parser and asserts the things that make a pipeline uncreatable.

Three defects were found by these tests while they were being written, and each
is worth recording because each was invisible before:

| Defect | Symptom it would have had |
|---|---|
| `saturate()` around `asin(sinAlt)` in `sunDirection` | the sun could never set; the world was lit at midnight |
| `pow(negative, -1.253)` in the air-mass fit | NaN for every downward-looking direction |
| The budget compared an *emission count* against a *triangle* ceiling | the budget never once fired, and the test meant to prove it worked proved nothing |

### Two shader defects, found without a GPU

The scene shaders had never been referenced by any code, so neither had ever
been compiled by anything. `glslc` is optional here and absent, and the
renderer's response to missing SPIR-V is to report "shader module unavailable"
— which is exactly what a shader with a syntax error also looks like. So
`test_native_shaders.mjs` parses them in Node instead.

It immediately found two things that would have failed on real hardware:

- **The material table was in push constants.** 64 materials × 2 vec4 is
  2 KB. The Vulkan guaranteed minimum for `maxPushConstantsSize` is 128 bytes,
  and plenty of drivers cap at 128 or 256. Moved to a UBO at set 1 binding 1.
- **A `mat4 view` no stage read.** With the table gone the block was still 152
  bytes, over the guarantee on its own. Removing the unused member brings it to
  96.

Neither would have shown up as "wrong colours". Both are
`vkCreateGraphicsPipelines` failures, or a pass on the one machine whose limit
happens to be high and a failure everywhere else.

### FINAL HARDWARE VALIDATION REQUIRED

None of the following has ever executed. There is no `/dev/dri` and no
`/usr/share/vulkan/icd.d` in this environment, so there is no driver to
execute against:

- Every line of `vulkan_render.cpp`'s scene path: descriptor sets, the material
  table upload, the three array images, the push constants, the draw call.
- Whether the city is *visible*. The tests assert the buffer is correct; they
  cannot assert the camera is pointed at it.
- Whether back-face culling is wound correctly. `MeshBuilder` documents the
  convention and the pipeline sets `VK_CULL_MODE_BACK_BIT`, but a winding error
  is only observable as an inside-out building.
- Frame cost. 210k triangles for a 420 m slice is a number, not a measurement.

### What is still not integrated

Nothing in the material path. The 26 CC0 Poly Haven materials are now in the
native renderer, described in the next section.

## 2026-09-27 — KTX2 decoding and the 26 CC0 materials in the native renderer

### What changed

The native scene pipeline used to allocate three `sampler2DArray` maps of the
right shape and fill them with physically neutral values: mid-grey albedo, the
tangent-space neutral normal, AO 1 / roughness 0.5 / metal 0. That is the
average material, not a wrong one, so the scene lit, fogged and shaded
correctly from the first frame — but it was the average material, and the whole
point of a CC0 material library is to not ship the average. Four pieces:

1. **`tools/assets/png.mjs`** — a PNG decoder for exactly what the bake emits:
   78 files, all 1024×1024, 8-bit, colour type 3. All five scanline filters,
   Adam7 rejected by name rather than mis-decoded, plus a box resampler and a
   mip chain. It exists because the alternatives are a dependency, a WASM
   transcoder, or a build host that is not available from this environment.
2. **`tools/assets/ktx2.mjs`** — the container. Header, level index, data
   format descriptor, key/value block, supercompression global data, and a full
   mip chain per layer, zstd-compressed with the content checksum **on**.
3. **`native/src/ktx2.cpp`** — the reader. No KTX dependency: it parses the
   header, level index, DFD and KVD itself and uses the vendored `libzstd` for
   the payload. It accepts supercompression scheme 2 and none, and names
   everything it rejects.
4. **`assets/materials.gen.hpp`** — the 26 materials as a C++ table, generated
   from the bake manifest so the native side needs no JSON parser and a
   malformed manifest is a build failure rather than a wall that comes out grey.
   Layer indices come from the manifest, not from filename order: a re-bake that
   reordered the list would otherwise repoint every wall at the wrong texture
   with no error anywhere.

CMake now runs the packer as part of the build, so a clean configure produces
the real 50 MB of packs and the real table, and `ctest` validates them.

### Verified here

`ctest`: **17/17**. `test_asset_pack.mjs`: **94 checks**.

- **`emergent_ktx2_test`** (76 checks) — decodes a committed fixture and
  compares every texel byte-exact, then corrupts the file at eight different
  offsets and requires every one to be rejected. Three of those offsets are
  *inside* the zstd frame, which is only detectable because the writer enables
  the content checksum. It also decodes the three real 50 MB packs and validates
  layer count, dimensions, mip count and the DFD against what the table expects.
- **`test_asset_pack.mjs`** (94 checks) — round-trips all five PNG filters
  against an encoder written independently in the test file, so a shared
  misunderstanding of the spec cannot pass; checks the DFD is 92 bytes (a real
  KTX2 file's `0x5C`, and 24 would overflow the buffer by four bytes), the level
  index, and that the writer is deterministic. The KVD block is *walked* using
  only its own length fields rather than at hard-coded offsets, which is both
  stronger (a wrong length desyncs the walk) and the reason a dropped entry
  produces a named failure instead of an out-of-range exception that takes the
  rest of the suite with it.

Three controls, because a test that cannot fail proves nothing. Making the
Paeth predictor return `a` instead of `c` fails filter 4 and only filter 4.
Dropping the `KTXwriter` entry fails three named checks. Padding the KVD to two
bytes instead of four makes the walk report a 2,002,277,451-byte entry — which
is what a reader would actually see from a misaligned container.

Two more ctests cover the last piece of the plumbing, and they exist because of a
real silence: the binary found the packs through a path relative to the working
directory, so launching it from anywhere but the repository root loaded nothing
and reported nothing. An untextured city looks like a material bug rather than a
missing file, and the two have completely different fixes. `NativeEngine` now
owns the directory, forwards it to the Vulkan backend when the backend is
created, takes `--assets DIR`, and prints the resolved path. One ctest asserts
the default, one asserts the override, and the control — making the setter a
no-op — fails the second and only the second.

### Three defects found while building this

| Defect | Symptom it would have had |
|---|---|
| zstd written without `ZSTD_c_checksumFlag` | corruption anywhere inside the compressed frame was silently accepted and decoded as plausible garbage — the worst failure shape there is, because it looks like data |
| The KVD's key assumed to start at byte 8 | WebGL-oriented habit; KTX2 has one length per entry, so the key is at byte 4. Caught by the test, not by eye |
| `metallic: -1` treated as a value | the "read metalness from the texture" sentinel became a uniform roughness of -1, i.e. a surface with no reflection at all |

A fourth was found before any of this: `pavement`, `terrain` and `wall_render`
are named by the scene builder and the geometry kit but are **not** among the 26
baked materials, and `MaterialTable::index` aborts the process on an unknown id.
Building a city with the real table would have crashed on the first frame. The
ids now name real baked materials, and `emergent_scene_mesh` asserts that every
id the scene and the kit name exists in the generated table — confirmed to bite
by renaming a kit id, which exits 134 with "material: unknown id".

### FINAL HARDWARE VALIDATION REQUIRED

None of the following has ever executed, and the first one is the one to check
first:

- **Whether KTX-Software accepts these files.** The writer was implemented from
  the KTX2 specification, not against a reference encoder, and no KTX tooling
  exists in this environment. Every *texel* is proven correct, and the
  supercompression is proven correct, because the reader is the only thing that
  ever has to read them — but the container has never been opened by `libktx`,
  `toktx` or any third-party tool. The descriptor is the part most likely to be
  subtly wrong. The check is one command on a machine with the SDK:
  `toktx --validate build/native-assets/albedo.ktx2`.
- **The upload itself** — `createSceneMaterialMaps` staging, the three array
  images, the view format, the sampler anisotropy, the transition from
  "neutral fill" to "real material". `RenderFrameStats::materialMapsLoaded` and
  `materialMapNote` are now printed on the render-target line, so a missing pack
  says so instead of looking like a material bug.
- **Whether the materials look right on a surface.** The reader proves the bytes
  arrive; nothing here proves a brick wall is tiled at a believable scale. Tile
  scale is authored, and authored means wrong somewhere.
- **Whether a normal map lights correctly.** The bake validated that blue
  dominates the normal maps, which catches an OpenGL/DirectX convention swap.
  It cannot show a tangent frame built from a planar projection.

### Note on the format choice

The packs are zstd-supercompressed **RGBA8**, not BCn or ASTC. That is
deliberate: zstd is a compression layer over an uncompressed payload, so every
texel stays byte-recoverable and therefore verifiable in CI. A BC6H pack could
not be decoded or checked by anything in this environment, so a GPU-compressed
build would have been 50 MB of unverifiable bytes. Swapping in `toktx` later is
a change to the packer's last step; the reader already dispatches on the DFD and
the supercompression scheme.

The cost of that choice, measured: **47.5 MB on disk, 416 MB of VRAM** for the
three arrays with complete mip chains. zstd shrinks the download and leaves
VRAM untouched, because decompression happens on the CPU and the GPU still
receives RGBA8. A BCn build is the single largest remaining win available on
hardware that has `toktx`, and it is worth roughly an order of magnitude on
both numbers. Until then, 416 MB of texture for a 420 m city slice is a
deliberate over-provision: the layer count is fixed at 26 and the resolution at
512 px, and both are authored rather than discovered.

**Slang.** A shading-language compiler. The two compute shaders in
`native/shaders/` are GLSL, compiled by the driver at load time; there is no
offline shader-compilation step for Slang to replace, and no GPU to validate
generated SPIR-V against. Not attempted.

**The Forge.** A full rendering framework with its own RHI, windowing and build
system. EMERGENT's renderer is raw Vulkan + Volk + VMA; adding The Forge would
replace the renderer rather than extend it.

## 2026-09-27 — the clean checkout could not run the game

Found by pushing the work above and reading what CI did with it. None of it is
native; it is all the web build, and it is the part of the repository that gets
shipped.

### What was actually broken

A fresh clone could not start the game. `interiors.mjs` imports
`assets/models.gen.mjs`, a 28 MB generated artefact that is deliberately not in
git, so the module graph had an unresolvable edge. `test_models.mjs` imported it
too, which is how CI found this: that suite had only ever passed on a machine
that had run the bake.

Worse, `tools/build.mjs` reported **success**. Three faults, all the same shape
-- something claimed to handle the case and did not:

| Fault | Why it was invisible |
|---|---|
| `ASSET_FILES` was declared, with a comment explaining that a missing entry is a 404, and **nothing ever read the array** | The build shipped a dist whose `interiors.mjs` could not resolve its own import |
| The hand-written list of "every module the page imports" had rotted | It named `gltf.mjs`, `lod.mjs` and `interiors.mjs`, none of which anything reaches from `index.html`, and 28 MB of payload rode along with them |
| A failed build did `rm -rf dist` before copying | Any error partway through destroyed a working artifact and left a half-written one where a deploy would find it |

The comments were the worst of it. A comment describing a check that does not
exist is worse than no comment, because it tells the next reader the case is
handled.

### What replaced it

**The file list is now derived, not declared.** `walkModuleGraph()` starts at
`index.html` and follows `src=`, `import=`, static imports, re-exports, side-
effect imports and `import(...)`, resolving relative specifiers and refusing
bare ones the import map does not cover. A module the page needs and does not
have fails the build by name. A module nothing needs is not shipped.

That took the dist from 96 files to 14 and 102 MB to 75 MB, and it makes the
class of bug impossible rather than absent.

Three things about the walker were wrong before they were right, and each was
caught by the number it printed rather than by reading the code:

- Requiring a `./` prefix on the document's own `src` found **one** file and
  shipped a dist with no game in it, while still reporting success.
- Matching imports without stripping comments matched the prose
  `nothing distinguishes "converged" from "pinned"` -- the word `from` followed
  by a quoted string is exactly the shape of an import statement.
- Checking the import map's *values* instead of its *keys* turned
  `@dimforge/rapier3d-compat` into an unsatisfiable bare specifier.

And a `catch` around the file read reported a typo in the walker itself as a
missing `index.html`, sending the reader to the repository instead of the build
script. It now only translates `ENOENT`.

**`assets/models.index.mjs`** holds the format -- room enum, vertex stride, LOD
decode -- in a file small enough to commit, and the payload is injected into it
by `setModelSet()`, which validates the stride and the room names rather than
trusting them. That is the data *shape*, not the content, and a consumer needs
all three whether or not a model has loaded. With nothing injected, `MODELS` is
empty and a building gets a shell and no furniture: a visibly emptier room, not
a crash and not a wrong one.

**`test_models.mjs`** no longer imports the payload. Its importer, simplifier,
LOD and interior checks run everywhere; the thirteen bake checks announce
themselves as skipped -- by name, and in a count -- rather than passing quietly.

**The build stages and swaps.** It writes `.dist-staging/`, renames the old
`dist` aside, renames the new one in, and only then removes the old. A failed
build cannot destroy what worked.

### Verified here

- `test_tooling.mjs` **11 checks** (was 7), including four new ones that assert
  the artifact against itself: the dist is self-contained, nothing unreachable
  ships, the payload stays out, and a missing module fails the build *without*
  taking the previous dist with it.
- The graph walk in the test is a **second implementation**. Importing the
  build's would make a bug in the walker invisible to the one check that exists
  to find it.
- A clean clone: `npm install`, `npm test` and `npm run build` all pass with no
  generated model payload present.
- `ctest` **17/17**, and **10/10** in the Tracy configuration.

### And the CI bug that was hiding three of them

The native job builds test targets from a named list. The atmosphere, ktx2 and
scene_mesh executables were not on it, and ctest scores a test whose executable
was never built as **"Not Run"** -- which reads like a pass in a summary line and
is nothing of the kind. The workflow's own comment records this trap biting an
earlier job. It builds `all` now.

Underneath were two more of the same shape: `emergent_ktx2_test` linked only
`emergent_ktx2`, so nothing pulled in the packer and its real-pack checks
skipped; and the packer's custom command declared only the generated header as
its output, so the 50 MB of KTX2 it also produces were untracked side effects.
Deleting them left the build reporting itself up to date. A build system cannot
track what it is not told about. The three packs are declared outputs now.

Verified by deleting the packs: the build repacks them, the test finds them, and
reports zero skips.


## 2026-09-27 — the one measured stall, spread over fourteen frames

### What the profiler said

One zone, out of nine, had anything to say:

| Zone | Calls | Total | Max |
|---|---|---|---|
| `frame.scene_stream` | 120 | 35.04 ms | **35.03 ms** |
| `frame.physics` | 180 | 1.18 ms | 0.04 ms |
| `frame.animation` | 180 | 0.16 ms | 0.003 ms |
| `scene.cull` | 1 | 0.08 ms | 0.08 ms |

One call of 35 ms in a 16.7 ms frame. That is the whole streamed slice -- 210,628
triangles assembled inside a single frame -- and it fires every time the player
crosses a chunk boundary, which is every ~50 seconds of walking. It has been
there since the streamer was first wired up, and nothing about it was visible in
a screenshot.

### What changed

`SceneBuilder` makes the build resumable. `begin()` starts a slice; `step()`
emits until a triangle budget is spent. The streamer calls it once per frame
with a 12,000-triangle budget, which is about a third of a frame at this class
of machine, leaving room for the physics step, the animation sampling and the
draw submission that share it.

**Measured: 14 frames, heaviest step 29,728 triangles, mesh unchanged at 631,884
vertices.**

The previous mesh stays on screen until the new one is ready, rather than
swapping to an empty vertex buffer and rendering nothing for 14 frames.

### Why it is safe, and how that is known

Only the *timing* changes. Both paths drive one set of phase functions --
`emitGround`, `emitRoads`, `emitWater`, `planBuildings`, `emitBuildingsRange`,
`emitProps` -- so they cannot disagree about what a building is made of. And the
byte-identity test is the real check, because "same triangle count" is exactly
the kind of assertion that passes while the city comes out subtly wrong:

`the amortized build is byte-identical to the one-shot build` runs at five
budgets, from 500 triangles to more than the whole slice, and compares **every
vertex**.

The control is what makes it worth having: emitting the buildings in a different
order leaves the count identical and fails only the byte comparison.

### Three things I got wrong, all caught by output rather than by reading

| Symptom | Cause |
|---|---|
| Every slice came back with 0 vertices and 28 buildings counted | The per-phase slice boundaries swallowed the original function's tail, so `emitProps` moved the vertex buffer out from under `finishScene` |
| `largestStep()` reported 4,294,757,004 | Measured after the final step drained the builder, so `after - before` underflowed. Measured inside `advance()` now |
| The first version of the test aborted on a material lookup | `begin()` holds the world and material table by reference across frames. Right for the engine, which owns both; a caller passing a temporary reads freed memory. The lifetime requirement is now on the declaration |

### Where the assertion lives, and why

The amortization bound is a C++ assertion
(`no single step is the whole slice`), not a ctest regex, because "the heaviest
step is under a quarter of the slice" is arithmetic and CMake cannot do
arithmetic. That distinction is not academic: a control that made the build stop
early still produced three frames, **passed the ctest**, and was caught twice by
the C++ test.

`emergent_native_scene_stream` now runs 240 frames rather than 5, because the
build needs 14 and a test that runs 5 and asserts a finished mesh asserts
nothing.

`ctest` **18/18**.

## Keeping this honest

`.github/workflows/native.yml` clones the same pinned revisions, configures,
builds all five test executables, runs `ctest`, uploads the profile the
self-test produced, and separately configures, builds *and tests* the
Tracy-enabled variant. If any of the defects above is ever reintroduced, that
job fails instead of the problem resurfacing as a mysterious runtime failure.
It has already earned its place: the Tracy leg is what caught the
`TRACY_ENABLE` inversion described above, which no default-configuration test
could ever have found.
