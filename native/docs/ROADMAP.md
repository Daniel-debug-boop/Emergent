# EMERGENT Native Renderer Roadmap

## Implemented in this pass

- Native C++ engine entry point retained.
- Real Vulkan loader/instance probing.
- Physical-device and queue-family discovery.
- Logical Vulkan device + queue creation when a compatible Vulkan ICD exists.
- Native command-pool creation.
- Headless GPU bootstrap path that creates an offscreen Vulkan image, records a real image clear, submits it to a Vulkan queue, and waits for completion.
- Core indirect-draw capability is represented only after a physical Vulkan device is found.
- Existing WebGL2 world/gameplay remains untouched as the reference implementation.
- Existing CPU adaptive-culling benchmark remains available.

## Next native rendering gates

1. Add an official Vulkan-Headers dependency/vendor path when a network/package source is available.
2. Add a real shader compilation pipeline (GLSL -> SPIR-V) and checked-in shader build artifacts.
3. Create device-local persistent scene/object buffers.
4. GPU compute culling -> indirect command generation.
5. Native offscreen graphics pipeline with depth, PBR materials and GPU timestamps.
6. Add a platform window/surface backend (SDL/GLFW) and swapchain.
7. Port the existing procedural world into the native scene database without deleting the JS reference implementation.
8. Meshlet generation and GPU LOD/visibility.
9. `VK_EXT_mesh_shader` feature-query + mesh/task shader pipeline where the hardware actually exposes the required features.
10. Temporal history, motion vectors, adaptive computation and measured quality/performance experiments.

## Honesty rule

A capability is advertised only after the corresponding Vulkan object/feature is actually created or queried. The current environment returns `VK_ERROR_INCOMPATIBLE_DRIVER` from `vkCreateInstance`, so no GPU rendering result is claimed here.
