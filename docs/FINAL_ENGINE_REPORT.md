# EMERGENT Final Engine Report

Status: **IN PROGRESS — not an acceptance-complete native renderer**

This document is intentionally a living report. A feature is not marked complete unless the implementation exists and its runtime behavior can be measured.

## Current truth

### Exists
- Existing procedural world and gameplay systems preserved.
- Existing WebGL2 renderer retained as a reference path.
- Native C++ build system.
- Native engine abstraction.
- Vulkan loader/device capability probe.
- Adaptive scheduler prototype.
- GPU culling shader prototypes.
- Native CPU reference benchmark.
- Research decision report.

### Not yet complete
- Vulkan surface/swapchain.
- Actual Vulkan graphics pipeline.
- Device-local GPU scene buffers.
- GPU-generated indirect draw commands wired into rendering.
- Meshlet asset preprocessing.
- `VK_EXT_mesh_shader` graphics pipeline.
- Temporal reprojection/history buffers.
- Native world rendering.
- Native streaming/resource residency.
- PBR frame graph.

## Measurements

Only measurements produced by an executable on a real environment belong here. No FPS/GPU-time values are filled in until a Vulkan device is actually available.

The native CPU reference benchmark reports object counts, visibility and wall time. These values are CPU measurements and must not be interpreted as GPU performance.

## Acceptance status

| Requirement | Status |
|---|---|
| Preserve existing world | PASS |
| Native executable | PASS |
| Vulkan capability detection | PARTIAL/PASS |
| Vulkan swapchain | TODO |
| GPU scene | TODO |
| GPU culling | SHADER PROTOTYPE |
| Indirect rendering | TODO |
| Meshlets | TODO |
| Mesh shaders | TODO |
| Adaptive computation | PROTOTYPE |
| Temporal reuse | TODO |
| Streaming residency | TODO |
| Native gameplay | TODO |
| Benchmark lab | PARTIAL |
| Final acceptance loop | TODO |

## Known limitation

The current build environment exposes the Vulkan loader but no usable Vulkan physical device/ICD to this project. The native executable therefore refuses to claim Vulkan rendering and falls back to the CPU self-test.

## Native renderer pass — 2026-09-24

The native build was rebuilt successfully and CTest coverage was added for the native self-test and benchmark smoke executable. The WebGL2 runtime also gained explicit chunk-residency bookkeeping and NPC simulation LOD: near agents update every frame, medium/far agents tick at reduced cadence, and very-far agents contribute low-cost aggregate state. This is an actual simulation decision path rather than a telemetry-only label.

Open-source research selected Volk, Vulkan Memory Allocator, Jolt Physics, Recast/Detour, EnTT, meshoptimizer, fastgltf, and miniaudio as preferred infrastructure candidates, subject to local SDK/toolchain availability. See `docs/OPEN_SOURCE_DECISIONS.md`.

The browser smoke/benchmark attempt in this environment could not produce a trustworthy gameplay metric because Chromium could not complete the WebGL game loop under the available headless configuration. No FPS or GPU timing is claimed from that run.

## Native renderer pass — 2026-09-23

Implemented a real Vulkan device bootstrap path in `native/src/vulkan_backend.cpp`. When a compatible Vulkan ICD exists, the backend now attempts physical-device selection, queue-family selection, logical-device creation, queue acquisition, command-pool creation, and a headless offscreen image-clear submission. This is deliberately a bootstrap/rendering proof rather than a claim of completed game rendering.

The current execution environment has `libvulkan.so.1` but `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER`, so no GPU rendering benchmark is claimed. The CPU reference benchmark and existing JS world tests still pass.
