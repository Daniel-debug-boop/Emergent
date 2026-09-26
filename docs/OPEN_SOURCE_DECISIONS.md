# EMERGENT Open-Source Integration Decisions

Research date: 2026-09-24.

The current repository already contains a working WebGL2 game and a native Vulkan capability/bootstrap path. The next native-engine work should reuse mature infrastructure rather than growing bespoke plumbing.

| Subsystem | Decision | Rationale | License / notes |
|---|---|---|---|
| Vulkan loading | ADAPT → Volk | The current backend manually resolves Vulkan symbols. Volk is a mature meta-loader that dynamically loads Vulkan entry points and extension entry points. | MIT. Keep the current loader only as a bootstrap fallback until the native renderer is converted. |
| Vulkan memory | REUSE → Vulkan Memory Allocator | Replace manual image/device-memory allocation once Vulkan headers/SDK are available. | MIT; current upstream documentation reports v3.4.0. |
| Physics | REUSE → Jolt Physics | Mature C++ rigid-body/collision library suitable for games; avoids a custom physics engine. | MIT. |
| Navigation | REUSE → Recast/Detour | Mature navmesh + query stack; tiled navmeshes support larger dynamic environments and streaming. | Zlib. |
| ECS | ADAPT → EnTT | Header-only C++20 ECS can provide native entity/component storage without a custom ECS. | MIT; integration is lightweight. |
| Mesh processing | REUSE → meshoptimizer | Provides vertex/index optimization and meshlet-related processing needed for GPU-driven rendering. | MIT. |
| glTF | REUSE → fastgltf | Lightweight native glTF loading is preferable to a custom asset parser. | MIT. |
| Audio | REUSE → miniaudio | Single-source cross-platform audio layer with low integration cost. | Public domain or MIT-0. |

## Current environment constraint

The supplied environment has `libvulkan.so.1`, but the native executable reports `vkCreateInstance` failure with `VK_ERROR_INCOMPATIBLE_DRIVER`. There is therefore no honest GPU rendering benchmark in this environment. The native build and CPU reference benchmark are still executable.

## Integration rule

Do not vendor or integrate a dependency solely because it is on this table. Before each native subsystem is wired in, verify the local SDK/toolchain availability, required platform support, version/API compatibility, and license notice requirements.

## Upstream-only enforcement

The native build now treats Jolt, meshoptimizer, Recast/Detour, Volk, VMA and Flecs as required infrastructure rather than optional fallbacks. Missing upstream dependencies fail configuration when `EMERGENT_REQUIRE_UPSTREAM=ON`; no custom physics/navigation/mesh-optimization substitute is compiled.

Additional mature components identified for the production stack were The Forge (renderer/framework), KTX-Software/Basis Universal, Slang, Tracy and Zstandard. ozz-animation, miniaudio, Tracy and Zstandard are now integrated and listed in `docs/UPSTREAM_STACK.md` with the `nm` counts that prove something calls them. The Forge, KTX-Software and Slang remain not integrated, each for a stated reason: The Forge would replace the renderer rather than extend it, KTX has no `.ktx2` assets in this tree to decode, and Slang has no offline shader-compilation step to replace. None of the three is represented as present.
