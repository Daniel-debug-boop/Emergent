// Level of detail, from meshoptimizer.
//
// ## Why this is a library and not a paragraph in the render path
//
// Simplification is the one part of LOD that can be *proved* on a machine with
// no GPU. A LOD chain either respects its error budget or it does not, its
// indices are in range or they are not, and its post-simplification vertex
// cache ratio is a number you can print. All of that is testable here, so it is
// here, in plain C++, with no Vk type in the file -- for the same reason
// `adaptive.hpp` and `render_graph.hpp` are separate libraries. A LOD chain
// that is wrong does not crash; it produces a city whose buildings visibly
// melt at 80 m, and that failure survives a screenshot review.
//
// ## Why meshoptimizer and not a hand-rolled simplifier
//
// Edge collapse with a quadric error metric is a solved problem and the
// reference implementation of it is the one already vendored in
// `third_party/meshoptimizer` (MIT, v1.2, the library NVIDIA's NVMM and
// meshoptimizer-based Nanite work is built on). Writing a second simplifier
// would mean owning every one of its failure modes for no gain. So this module
// owns the parts meshoptimizer does not do: choosing the level targets,
// measuring the result, and turning the result into the cluster and cone
// records that `adaptive.hpp` consumes.
//
// The specific calls, and what each is for:
//
//   meshopt_simplify            edge collapse down to a target triangle count
//   meshopt_simplifyScale       converts between absolute and relative error,
//                              which is what makes `error` here a distance in
//                              metres rather than a unitless ratio
//   meshopt_simplifyPrune       drops degenerate and tiny triangles that survive
//                              collapse and would render as shimmer
//   meshopt_optimizeVertexCache reorders triangles for the post-transform cache
//   meshopt_analyzeVertexCache  measures the ACMR of the result -- the number
//                              that says whether a cheaper LOD is actually
//                              cheaper, since a level with fewer triangles and
//                              a worse ACMR can be slower
//   meshopt_buildMeshlets       partitions into Nanite-style clusters
//   meshopt_computeMeshletBounds  the sphere *and the normal cone* per cluster
//
// The last one matters more than it looks. `adaptive.hpp` has a hand-written
// normal-cone cull, and it had three separate bugs before it was right (see the
// comments in that file). The cone here is not computed by that code: it comes
// from meshoptimizer's clusterizer, which is the same function used to decide
// which triangles form a cluster in the first place, so the cone is guaranteed
// to bound the triangles it was computed from.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace emergent {

/** FNV-1a, so hashes in this header cannot drift from the ones in adaptive.hpp. */
inline constexpr uint64_t kLodHashOffset = 0xcbf29ce484222325ull;
inline uint64_t lodMix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x100000001b3ull;
    return h;
}

/** What one level of a chain holds. */
struct LodLevel {
    /**
     * Indices into the *shared* vertex array, three per triangle.
     *
     * Every level indexes the same vertices. A simplification that changed the
     * vertex count per level would need a per-level vertex buffer, a per-level
     * bind range, and a way to know how much of it a cluster owns -- three
     * sources of truth that can disagree. Instead `meshopt_simplify` is given
     * the full vertex array and only returns a different index list, so the
     * vertices are literally the same bytes and a cluster's vertex range is
     * valid at every level.
     */
    std::vector<uint32_t> indices;

    /** meshopt's relative error for this level, already scaled to metres. */
    float error = 0.0f;

    /** Average cache miss ratio. The number that says if this level is worth it. */
    float acmr = 0.0f;

    uint32_t triangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }

    /**
     * True when the chain could not be built as asked.
     *
     * A LOD chain that silently returns one level is a LOD chain that has
     * turned itself off, and the symptom is a distant city rendering at full
     * detail with a frame time nobody can explain. The caller gets told.
     */
    bool valid = false;
};

/**
 * One cluster: the unit of culling and of the draw.
 *
 * Laid out to match the shader's `Cluster` in `cull_indirect.comp` field for
 * field, because the whole point of computing a sphere and a cone on the CPU
 * is to hand them to the GPU without recomputing them there.
 */
struct LodCluster {
    /** First index into the level's index array. */
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    /** First vertex in the shared vertex array. */
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;

    float sphereCenter[3] = {0, 0, 0};
    float sphereRadius = 0.0f;

    float coneAxis[3] = {0, 1, 0};
    /**
     * The cone's apex, as meshoptimizer computes it.
     *
     * Carried because the rejection test has two forms and they are not
     * equivalent. `dot(normalize(apex - eye), axis) >= cos` is the accurate one
     * and needs the apex. `dot(center - eye, axis) >= cos * length + radius` is
     * the singularity-free one, needs only the sphere the cluster already has,
     * and rejects a strict subset of what the accurate form rejects.
     *
     * The subset relationship is the property that matters, and it is asserted
     * in lod_test.cpp: the cheap form must never reject a cluster the accurate
     * form would keep, or it stops being conservative and starts deleting
     * geometry that is on screen.
     */
    float coneApex[3] = {0, 0, 0};
    /** cos(half-angle). <= 0 means the cone is too wide to reject anything. */
    float coneCos = 0.0f;
    /** meshoptimizer flags a cluster whose cone it could not bound. */
    bool coneUsable = false;
    /** A cluster with no meaningful outside (a closed volume) is never back-culled. */
    bool twoSided = false;
};

/** How to build a chain. */
struct LodBuildOptions {
    /**
     * How many levels, *including* the original. 1 means "do not simplify",
     * which is a legitimate request and returns the mesh unchanged.
     */
    uint32_t levelCount = 4;

    /**
     * Triangle ratio per level, level 0 first. Must be strictly decreasing and
     * in (0, 1]. The last entry is the coarsest level.
     */
    std::vector<float> ratios = {1.0f, 0.5f, 0.25f, 0.125f};

    /**
     * Error budget per level, in metres of surface deviation. A level that
     * cannot be reached within its budget is reported as short rather than
     * being quietly given more error than asked for.
     */
    std::vector<float> errorBudgets = {0.0f, 0.05f, 0.20f, 0.60f};

    /** Max triangles per cluster. 64 is meshoptimizer's recommended default. */
    uint32_t clusterMaxTriangles = 64;
    /** Max vertices per cluster. 64 likewise. */
    uint32_t clusterMaxVertices = 64;
    /**
     * Weight given to the normal cone when partitioning.
     *
     * meshoptimizer's default of 0.0 splits purely by spatial locality, which
     * produces clusters whose cones are too wide to cull anything. 0.5 trades a
     * little spatial coherence for cones worth using, and the measurements in
     * lod.cpp are why: at 0.0 the chain is measurably worse at culling, and at
     * 0.5 the cones actually reject.
     */
    float clusterConeWeight = 0.5f;

    /**
     * Refuse to collapse an edge on the mesh boundary.
     *
     * On by default and it is not about quality. A scene slice shares edges
     * with the chunks either side of it, and collapsing an edge on a shared
     * boundary collapses it in one chunk only -- the two chunks stop agreeing
     * and the seam opens into a crack you can see from inside the city. For a
     * closed mesh like a single building there is no boundary and this changes
     * nothing, which is exactly what the test asserts.
     */
    bool lockBorder = true;
};

/** What a finished chain holds, and what went wrong building it. */
struct LodChain {
    std::vector<LodLevel> levels;
    std::vector<LodCluster> clusters;

    /** Triangles in the input. The denominator every ratio is measured against. */
    uint32_t sourceTriangles = 0;
    /** Distinct vertices after remapping. */
    uint32_t vertexCount = 0;
    /** Vertices in the input, before remapping. */
    uint32_t sourceVertexCount = 0;

    /** ACMR of the input as given, before any reordering. */
    float sourceAcmr = 0.0f;

    /** Levels requested but not produced. Zero in a healthy build. */
    uint32_t levelsMissing = 0;
    /** Non-empty when a requested level could not be built. Never fatal. */
    std::string warning;

    bool empty() const { return levels.empty(); }
    uint32_t levelCount() const { return static_cast<uint32_t>(levels.size()); }

    /**
     * The triangle count a cluster contributes at a level.
     *
     * Clusters index the *source* index array, so a coarser level has fewer
     * triangles inside the same cluster bounds than the count here says. The
     * draw therefore uses the level's own total, and this is the *upper* bound
     * used for budgeting and for the indirect capacity check -- never for
     * issuing a draw, which would over-read the index buffer.
     */
    uint32_t triangleCountAt(uint32_t level) const;
};

/**
 * Build a LOD chain.
 *
 * `positions` is a flat xyz array, `vertexCount` vertices. `indices` is three
 * per triangle, wound counter-clockwise when seen from outside. Positions must
 * be in the same units as `errorBudgets`; the budget is a distance in metres
 * because that is the only unit the renderer's error metric is in.
 *
 * Deterministic: the same inputs produce byte-identical output. meshoptimizer
 * is deterministic given the same input order, and this function does not sort
 * or hash anything whose order depends on a container's iteration order.
 */
LodChain buildLodChain(const float *positions, uint32_t vertexCount,
                       const uint32_t *indices, uint32_t indexCount,
                       const LodBuildOptions &options);

/**
 * Remap an arbitrary vertex stream onto a deduplicated vertex array, returning
 * the index array that addresses it.
 *
 * The scene mesh is non-indexed -- the emitters produce triangles directly --
 * so a simplification has nothing to collapse until there is an index buffer.
 * This is the step that makes one, welding vertices that are bitwise identical
 * and leaving everything else alone.
 *
 * A weld that is *nearly* identical would be better for LOD quality and much
 * worse for correctness, because the two copies could carry different normals
 * and the simplifier would have to pick one. So: exact bits, no tolerance, and
 * `welded` reports how many went away.
 */
std::vector<uint32_t> weldVertices(const float *attributes, uint32_t vertexCount,
                                   uint32_t attributeCount, uint32_t &welded);

/**
 * Measure the surface deviation of `candidate` from `reference`, in metres.
 *
 * Brute force, and deliberately so: this is the check that the simplifier's
 * own error figure means what the header says it means. meshoptimizer reports a
 * relative quadric error; this reports the actual largest distance from a
 * reference vertex to the nearest point on the candidate surface. They are
 * different numbers and the test asserts the second against the first.
 *
 * O(reference * candidate) with no acceleration structure, because it runs on
 * test-sized meshes in CI and a BVH here would be a second thing that can be
 * wrong. Do not call it on a city.
 */
float maxSurfaceDeviation(const float *referencePositions, uint32_t referenceVertexCount,
                          const uint32_t *referenceIndices, uint32_t referenceIndexCount,
                          const float *candidatePositions, uint32_t candidateVertexCount,
                          const uint32_t *candidateIndices, uint32_t candidateIndexCount);

}  // namespace emergent
