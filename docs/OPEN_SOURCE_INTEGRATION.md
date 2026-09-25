# EMERGENT upstream subsystem integration

EMERGENT uses mature upstream libraries at subsystem boundaries when they genuinely replace a custom implementation. The project does not claim an upstream library is active unless the build actually links and executes it.

## Active native integrations

- **Jolt Physics 5.6.0**: rigid-body simulation, collision detection, constraints and multithreaded physics. The wrapper owns engine lifetime and exposes the simulation to EMERGENT.
- **meshoptimizer 1.2**: native index/vertex-cache optimization and mesh analysis. Asset processing is delegated to the upstream algorithms instead of a home-grown optimizer.
- **Volk**: Vulkan entry-point loading in the full Vulkan build.
- **Vulkan Memory Allocator 3.4.0**: Vulkan image/buffer allocation in the full Vulkan build. The renderer's bootstrap image now uses `vmaCreateImage`/`vmaDestroyImage` rather than manual `VkDeviceMemory` selection and binding.
- **Recast/Detour 1.6.0**: native navigation runtime boundary. `DetourNavigationWorld` supports loading Detour navmesh data, agent registration, target requests, crowd updates and agent state extraction. This replaces a bespoke native pathing implementation where native navigation is used.

## Integration policy

The dependency graph supports three real deployment modes:

1. **Pinned FetchContent** for online builds.
2. **Explicit source-tree paths** for offline/air-gapped builds (`EMERGENT_*_DIR`).
3. **Installed CMake packages** when the platform already provides the upstream package.

If none is present, the corresponding feature is unavailable. EMERGENT never reports the fallback as an upstream implementation.

The Forge is deliberately not reported as integrated: it is a complete rendering framework, and replacing the existing renderer with it without a verified architectural fit would be a migration rather than a genuine subsystem replacement.
