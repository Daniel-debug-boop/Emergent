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

**ozz is unexercised.** Its libraries do build (`libozz_base_r.a`,
`libozz_animation_r.a`), but nothing in `native/` includes an ozz header, so the
linker drops both archives and the finished binary contains zero ozz symbols:

```
$ nm -C build/native/emergent_native | grep -c "ozz::"   # -> 0
$ nm -C build/native/emergent_native | grep -c "JPH::"   # -> 4656
```

ozz animates authored skeleton and clip data produced by its offline tooling,
and this repository has no skeleton, no animation clips and no importer. It is
wired into the build and ready, and is explicitly not counted as an integrated
feature. Until an asset pipeline exists it is build-time cost for no runtime
behaviour.

## Keeping this honest

`.github/workflows/native.yml` clones the same pinned revisions, configures,
builds `emergent_native` and `emergent_physics_test`, and runs `ctest`. If any
of the four defects above is ever reintroduced, that job fails instead of the
problem resurfacing as a mysterious runtime failure.
