# EMERGENT upstream stack

EMERGENT uses mature upstream libraries for engine infrastructure wherever they genuinely replace bespoke technology.

Current native runtime dependencies:
- Jolt Physics 5.6.0 — rigid-body physics and collision.
- meshoptimizer 1.2 — mesh optimization.
- Recast/Detour 1.6.0 — navigation and crowd simulation.
- Volk 1.4.328 — Vulkan loader.
- Vulkan Memory Allocator 3.3.0 — Vulkan memory allocation.
- Flecs 4.0.5 — ECS.
- miniaudio 0.11.25 — audio runtime.
- ozz-animation 0.17.0 — skeletal animation runtime and offline builders.
- Zstandard 1.5.7 — compressed content packs and their integrity checks.
- Tracy 0.13.0 — live profiling capture, opt-in via `EMERGENT_ENABLE_TRACY=ON`.

These dependencies are fetched or supplied as source trees. With EMERGENT_REQUIRE_UPSTREAM=ON, missing required targets are configuration errors rather than permission to compile a home-grown substitute.

A dependency is only listed here once something in the tree calls it. Because a static archive nothing references is dropped by the linker without a warning, that is checked directly:

```
$ nm -C build/native/emergent_native | grep -c "ozz::"   # -> 188
$ nm -C build/native/emergent_native | grep -c "ZSTD_"   # -> 450
$ nm -C build/native/emergent_native | grep -c "JPH::"   # -> 4656
```

The Forge remains an upstream renderer/framework candidate, but it is not falsely claimed as integrated until its actual source/build system is present and its renderer replaces the current Vulkan bootstrap rather than merely coexisting with it.

KTX-Software/Basis Universal and Slang are deliberately not integrated. A compressed-texture path needs authored `.ktx2` assets to decode and this tree has none, and a shading-language compiler has no offline compilation step to replace while the compute shaders stay driver-compiled GLSL. Neither is listed as a dependency because neither is used.
