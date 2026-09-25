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
- Ozz Animation 0.17.0 — skeletal animation runtime/toolchain.

These dependencies are fetched or supplied as source trees. With EMERGENT_REQUIRE_UPSTREAM=ON, missing required targets are configuration errors rather than permission to compile a home-grown substitute.

The Forge remains an upstream renderer/framework candidate, but it is not falsely claimed as integrated until its actual source/build system is present and its renderer replaces the current Vulkan bootstrap rather than merely coexisting with it.

KTX-Software/Basis Universal, Slang and Tracy are build/asset/tooling integrations that require their actual upstream source/tool binaries before being marked active.
