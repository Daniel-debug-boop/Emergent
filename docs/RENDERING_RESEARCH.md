# EMERGENT Rendering Research & Technology Decision Report

Date: 2026-09-23
Status: research baseline; implementation claims are tracked separately.

## Evidence discipline

- **FACT** = documented behavior/specification or directly observed runtime behavior.
- **ASSUMPTION** = engineering assumption awaiting target-hardware validation.
- **HYPOTHESIS** = expected benefit that must be benchmarked.
- **BENCHMARK RESULT** = measured by EMERGENT's test harness on a named environment.

## 1. Vulkan

**FACT:** Vulkan provides explicit device/queue/resource/synchronization control and compute/graphics pipelines. EMERGENT should use Vulkan as the primary native RHI.

**Decision:** Use a thin RHI above Vulkan. Keep world/simulation independent from Vulkan handles.

**Reason:** A backend abstraction lets D3D12/Metal be added later without contaminating gameplay code.

## 2. GPU-driven culling + indirect drawing

**FACT:** GPU compute can update indirect draw command buffers. Sascha Willems' Vulkan examples demonstrate frustum/LOD culling by compute and consumption of indirect commands without synchronizing the full object list back to the CPU. See: https://github.com/SaschaWillems/Vulkan/tree/master/examples/computecullandlod and https://github.com/SaschaWillems/Vulkan/tree/master/examples/indirectdraw

**Decision:** This is the primary non-mesh-shader path.

**HYPOTHESIS:** For dense EMERGENT scenes, reducing CPU draw submission and CPU visibility work should improve CPU frame time and scalability. The GPU cost of culling must be measured; for small scenes it can lose.

## 3. Mesh shaders / task shaders

**FACT:** `VK_EXT_mesh_shader` adds task and mesh shader stages and corresponding synchronization stages. https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html

**FACT:** Meshlets are small application-defined geometry groups; AMD's mesh-shader research describes their use for culling, LOD and procedural geometry. https://gpuopen.com/learn/mesh_shaders/mesh_shaders-from_vertex_shader_to_mesh_shader/

**Decision:** Implement mesh shaders as a capability-gated rendering path, not as the only renderer.

**HYPOTHESIS:** Mesh/task shaders can reduce geometry-stage overhead and enable fine-grained meshlet selection, especially for high geometric complexity. They are not automatically faster than indirect indexed rendering.

## 4. Meshlets

**FACT:** Meshlets provide bounded geometry units with bounds/topology suitable for GPU visibility and LOD. AMD documents cone/frustum culling and meshlet compression techniques. https://gpuopen.com/learn/mesh_shaders/mesh_shaders-meshlet_compression/

**Decision:** Meshlet preprocessing is shared by the mesh-shader and future meshlet-aware fallback paths.

**HYPOTHESIS:** Meshlets should reduce the amount of geometry work when visibility/LOD rejection is high.

## 5. Temporal reuse

**Decision:** Start with motion vectors, history validity and temporal stability metadata before attempting predictive lighting. History is invalidated on disocclusion, major camera changes, transform changes and lighting/weather transitions.

**HYPOTHESIS:** Temporal reuse can reduce repeated work for slowly changing distant content, but ghosting/disocclusion errors define the usable confidence threshold.

## 6. Adaptive computation

**Decision:** Use a bounded priority model, not an unconstrained multiplicative score that can collapse to zero. Inputs are normalized and clamped. The manager produces update intervals, LOD pressure and streaming priority.

**HYPOTHESIS:** Adaptive scheduling improves total work/quality when scene importance varies substantially. It is not expected to help uniformly dynamic scenes.

## 7. Async compute

**Decision:** Defer until timestamps show a real overlap opportunity. Visibility is initially scheduled on the graphics queue for simpler synchronization.

**Reason:** Async compute adds queue ownership/synchronization complexity and can regress performance when queues share execution resources.

## 8. Frame graph

**Decision:** Build a small explicit pass/dependency graph before adding advanced post-processing. Each pass declares reads/writes and the renderer derives ordering/barriers.

## 9. Virtualized geometry / Nanite-like architecture

**Decision:** Do not attempt a full virtualized-geometry system now. EMERGENT first needs stable GPU scene, streaming, meshlets and culling. Full virtualized geometry is a later research track.

## 10. PBR / lighting

**Decision:** HDR + PBR material data + physically sensible exposure are the first visual upgrades. Volumetrics, virtual shadows and advanced GI remain later, budgeted passes.

## 11. Rejected / deferred approaches

| Technique | Decision | Reason |
|---|---|---|
| CPU visibility + thousands of draws | Replace | Does not scale with dense procedural scenes |
| Mesh shaders everywhere | Reject | Hardware support is not universal and performance is workload-dependent |
| Async compute immediately | Defer | Requires evidence of overlap benefit |
| Full virtualized geometry immediately | Defer | Too much complexity before base GPU scene is validated |
| Predictive lighting before history infrastructure | Defer | Error/validity tracking must exist first |
| Fake GPU metrics | Reject | Invalidates research conclusions |

## 12. Current EMERGENT architecture decision

```text
World / Simulation / Gameplay
             |
          GPU Scene
             |
     Adaptive Computation
             |
      +------+------+
      |             |
 Visibility       Streaming
      |
 +----+------------------+
 |                       |
Mesh Shader path     Indirect fallback
 |                       |
 +-----------+-----------+
             |
         Frame Graph
             |
      PBR / Lighting
             |
       Temporal stage
             |
          Present
```

## Sources

- Khronos Vulkan documentation: https://docs.vulkan.org/
- VK_EXT_mesh_shader: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_mesh_shader.html
- Sascha Willems Vulkan examples: https://github.com/SaschaWillems/Vulkan
- Compute culling + LOD example: https://github.com/SaschaWillems/Vulkan/tree/master/examples/computecullandlod
- Indirect draw example: https://github.com/SaschaWillems/Vulkan/tree/master/examples/indirectdraw
- AMD GPUOpen mesh shader series: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-index/
- AMD meshlet compression: https://gpuopen.com/learn/mesh_shaders/mesh_shaders-meshlet_compression/
- NVIDIA mesh shader technical material: https://developer.nvidia.com/blog/using-turing-mesh-shaders-nvidia-asteroids-demo/
