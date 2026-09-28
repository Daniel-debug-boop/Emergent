// The world, as a drawable mesh.
//
// This is the missing link between the two halves of the native renderer. The
// world generator produces a description — regions, roads, buildings, trees —
// and the geometry kit produces primitives, but nothing connected them to a
// vertex buffer, so `vulkan_render.cpp` fell back to instancing unit cubes and
// the whole renderer was a debug view wearing a production hat.
//
// This module is the assembler. It takes a `World` and emits the exact bytes
// the shader reads, in one contiguous interleaved buffer, with a material index
// per vertex. One buffer and one draw call, because the material table lives in
// a uniform and the shader resolves the index itself; sorting the mesh by
// material would buy nothing and would break the streaming cache.
//
// ## Why it is a separate library
//
// The assembly is the part that goes wrong silently. A missing facade, a
// building whose windows are on the wrong face, a normal of length zero, an
// index past the end of the buffer — none of these produce a crash. They
// produce a city that looks almost right, which is the worst failure mode
// there is, because it survives a screenshot review. So the assembly is plain
// CPU code with no Vulkan in it, and the test suite can walk the buffer and
// prove the invariants that a GPU would otherwise only report as a black pixel.

#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "emergent/geometry.hpp"
#include "emergent/materials.hpp"
#include "emergent/world_gen.hpp"

namespace emergent {

/**
 * What to build, and how much of it.
 *
 * Radius rather than "everything", because the world is unbounded and a mesh
 * of the whole thing does not fit in any budget. The radius is the streaming
 * radius: the same number the web builder's vertex budget controller steers,
 * so the two halves of the game agree about how much world exists at once.
 */
struct SceneBuildOptions {
    double centerX = 0.0;
    double centerZ = 0.0;
    double radius = 400.0;

    /**
     * Detail level, 0 coarsest. Above 0, facades get window openings, sills and
     * a cornice; above 1, they get balconies, parapets and roof plant. It is
     * clamped to [0, 2] because a detail parameter with no ceiling is a detail
     * parameter with no budget.
     */
    int32_t detailLevel = 1;

    /** Include street furniture, trees and bushes. Off for distant slices. */
    bool includeProps = true;
    /** Include the ground surface. Off for a slice drawn over existing terrain. */
    bool includeGround = true;

    /**
     * Ceiling on emitted triangles. 0 means no ceiling. A ceiling is honoured
     * by dropping whole buildings from the far edge inward, never by cutting a
     * building in half — a building with no facade is worse than no building,
     * because it is a lit box.
     */
    uint32_t maxTriangles = 0;
};

/** What was built, and what was left out. */
struct SceneBuildStats {
    uint32_t vertices = 0;
    uint32_t triangles = 0;
    uint32_t buildings = 0;
    uint32_t buildingsDroppedForBudget = 0;
    uint32_t roads = 0;
    uint32_t props = 0;
    uint32_t trees = 0;
    uint32_t groundQuads = 0;
    /** Buildings that fell outside the build radius and were never candidates. */
    uint32_t buildingsOutsideRadius = 0;
    /** True when the budget was hit before the radius ran out. */
    bool budgetLimited = false;
    /** True when anything was dropped for any reason. Always worth surfacing. */
    bool truncated() const {
        return budgetLimited || buildingsDroppedForBudget > 0;
    }
};

/**
 * A finished, GPU-ready mesh.
 *
 * `vertices` is the interleaved 12-float layout, ready to memcpy into a
 * VkBuffer. There is no index buffer: the emitters produce triangles directly
 * and an index buffer for a static mesh that is never re-uploaded only saves
 * memory the machine does not need.
 */
struct SceneMesh {
    std::vector<float> vertices;
    SceneBuildStats stats;
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};

    bool empty() const { return vertices.empty(); }
    uint32_t vertexCount() const { return static_cast<uint32_t>(vertices.size() / kFloatsPerVertex); }

    /** Read one vertex, for tests and for the bounds recompute. */
    void readVertex(uint32_t index, float out[kFloatsPerVertex]) const;

    void clear() {
        vertices.clear();
        stats = SceneBuildStats{};
    }
};

/**
 * Assemble a world slice into a mesh.
 *
 * Deterministic: the same world and options always produce the same bytes, and
 * every per-building decision is driven by `Building::rngState` rather than by
 * a global generator, so a building keeps its facade when the slice moves.
 */
SceneMesh buildSceneMesh(const World &world, const MaterialTable &materials,
                         const SceneBuildOptions &options);

/**
 * A scene build that can be finished over several frames.
 *
 * WHY. buildSceneMesh() is a single synchronous call, and the profiler says it
 * costs 35 ms for a 420 m slice around a city -- 210,628 triangles assembled
 * inside one frame. A 60 Hz frame has 16.7 ms, so that is a dropped frame every
 * time the player crosses a chunk boundary, which is every ~50 seconds of
 * walking. It is the one measured stall in the native frame, and it is the
 * difference between a world that feels solid and one that twitches.
 *
 * WHY IT IS SAFE. Stepping emits the same buildings, in the same distance
 * order, with the same per-building decisions as the one-shot call, so the
 * bytes are identical. That is not an aspiration -- `the amortized mesh is
 * byte-identical to the one-shot mesh` is asserted, at several budgets, because
 * an amortizer that quietly changed the output would be worse than the stall.
 *
 * What changes is only *when* the work happens, and how much of it any one
 * frame is allowed. Ground, roads and water are emitted first so the first
 * step is a complete, drawable world rather than an empty frame.
 */
class SceneBuilder {
public:
    /**
     * Start a build. Discards any previous one.
     *
     * LIFETIME. `world` and `materials` are held by reference for the whole
     * build, which spans frames, so both must outlive it. Passing a temporary
     * compiles and then reads freed memory: the reference is valid for the
     * duration of the call and dangling by the time step() is called. Every real
     * caller owns both (the engine holds the world and shares one material
     * table), and the cost of copying a 2,800-building world at every chunk
     * boundary is not worth paying to make a misuse impossible -- but it is the
     * first thing to check when a streamed slice comes back empty.
     */
    void begin(const World &world, const MaterialTable &materials, const SceneBuildOptions &options);

    /**
     * Emit until roughly `triangleBudget` more triangles exist, or the build
     * finishes. Returns true once the mesh is complete.
     *
     * The budget is a soft ceiling on the work done in this call, not on the
     * total: one building is emitted whole or not at all, so a call can overshoot
     * by the size of the building in flight and never by more than that.
     */
    bool step(uint32_t triangleBudget);

    /** True once step() has emitted everything. */
    bool complete() const { return complete_; }
    /** How many steps have run. For tests and for the frame-cost report. */
    uint32_t stepCount() const { return steps_; }
    /** The largest number of triangles any single step emitted. */
    uint32_t largestStep() const { return largestStep_; }

    /** The mesh. Only meaningful once complete() is true. */
    const SceneMesh &mesh() const { return mesh_; }

    /**
     * Release the mesh and restart.
     *
     * Called when the player crosses a boundary mid-build: the partial slice is
     * for ground the player has already left, and finishing it would spend the
     * budget on geometry nobody will see.
     */
    void reset();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    SceneMesh mesh_;
    bool complete_ = true;
    uint32_t steps_ = 0;
    uint32_t largestStep_ = 0;
};

}  // namespace emergent
