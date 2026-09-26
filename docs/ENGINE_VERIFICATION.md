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

## Still not integrated

**KTX / Basis Universal.** A compressed-texture path needs authored `.ktx2`
assets to decode. This tree has no texture assets, and there is no GPU here to
validate an upload against, so the result would be a decoder with nothing to
decode. Not attempted.

**Slang.** A shading-language compiler. The two compute shaders in
`native/shaders/` are GLSL, compiled by the driver at load time; there is no
offline shader-compilation step for Slang to replace, and no GPU to validate
generated SPIR-V against. Not attempted.

**The Forge.** A full rendering framework with its own RHI, windowing and build
system. EMERGENT's renderer is raw Vulkan + Volk + VMA; adding The Forge would
replace the renderer rather than extend it.

## Keeping this honest

`.github/workflows/native.yml` clones the same pinned revisions, configures,
builds all five test executables, runs `ctest`, uploads the profile the
self-test produced, and separately configures, builds *and tests* the
Tracy-enabled variant. If any of the defects above is ever reintroduced, that
job fails instead of the problem resurfacing as a mysterious runtime failure.
It has already earned its place: the Tracy leg is what caught the
`TRACY_ENABLE` inversion described above, which no default-configuration test
could ever have found.
