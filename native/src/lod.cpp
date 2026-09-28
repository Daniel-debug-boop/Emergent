#include "emergent/lod.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace emergent {
namespace {

// ---------------------------------------------------------------------------
// Small vector helpers. Everything here is float, and the only source of NaN
// in this file would be a caller, so the checks below are about catching that
// rather than about numerical tidiness.
// ---------------------------------------------------------------------------

inline uint32_t bitsOf(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

/** Squared distance from a point to a triangle, after Real-Time Rendering §5.1.5. */
float pointTriangleDistanceSq(const float p[3], const float a[3], const float b[3], const float c[3]) {
    const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};

    const float d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2];
    const float d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return ap[0] * ap[0] + ap[1] * ap[1] + ap[2] * ap[2];
    }

    const float bp[3] = {p[0] - b[0], p[1] - b[1], p[2] - b[2]};
    const float d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2];
    const float d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
    if (d3 >= 0.0f && d4 <= d3) {
        return bp[0] * bp[0] + bp[1] * bp[1] + bp[2] * bp[2];
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        const float dx = ap[0] - v * ab[0];
        const float dy = ap[1] - v * ab[1];
        const float dz = ap[2] - v * ab[2];
        return dx * dx + dy * dy + dz * dz;
    }

    const float cp[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
    const float d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2];
    const float d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
    if (d6 >= 0.0f && d5 <= d6) {
        return cp[0] * cp[0] + cp[1] * cp[1] + cp[2] * cp[2];
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        const float dx = ap[0] - w * ac[0];
        const float dy = ap[1] - w * ac[1];
        const float dz = ap[2] - w * ac[2];
        return dx * dx + dy * dy + dz * dz;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const float dx = bp[0] - w * (cp[0] - bp[0]);
        const float dy = bp[1] - w * (cp[1] - bp[1]);
        const float dz = bp[2] - w * (cp[2] - bp[2]);
        return dx * dx + dy * dy + dz * dz;
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    const float dx = ap[0] - (v * ab[0] + w * ac[0]);
    const float dy = ap[1] - (v * ab[1] + w * ac[1]);
    const float dz = ap[2] - (v * ab[2] + w * ac[2]);
    return dx * dx + dy * dy + dz * dz;
}

/**
 * Signed volume of a triangle soup.
 *
 * Used as the closedness test for cone culling. A consistently wound closed
 * surface has a positive volume proportional to what it encloses; a single
 * quad, a plane, or a sheet folded back on itself has ~zero, and a mesh wound
 * inside-out has negative. The clusterizer's cones are only meaningful for the
 * first case -- a cone that bounds the normals of a sheet is a cone that will
 * confidently reject a sheet the player is looking straight at.
 */
float signedVolume(const float *positions, const uint32_t *indices, uint32_t indexCount) {
    double total = 0.0;
    for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
        const float *a = positions + static_cast<size_t>(indices[i + 0]) * 3;
        const float *b = positions + static_cast<size_t>(indices[i + 1]) * 3;
        const float *c = positions + static_cast<size_t>(indices[i + 2]) * 3;
        const double cx = static_cast<double>(c[0]) - a[0];
        const double cy = static_cast<double>(c[1]) - a[1];
        const double cz = static_cast<double>(c[2]) - a[2];
        const double bx = static_cast<double>(b[0]) - a[0];
        const double by = static_cast<double>(b[1]) - a[1];
        const double bz = static_cast<double>(b[2]) - a[2];
        total += static_cast<double>(a[0]) * (by * cz - bz * cy) +
                 static_cast<double>(a[1]) * (bz * cx - bx * cz) +
                 static_cast<double>(a[2]) * (bx * cy - by * cx);
    }
    return static_cast<float>(total / 6.0);
}

/** Hash of a whole vertex, attribute by attribute, over the raw bits. */
struct VertexHash {
    size_t operator()(const std::vector<float> &v) const {
        uint64_t h = kLodHashOffset;
        for (float f : v) h = lodMix(h, bitsOf(f));
        return static_cast<size_t>(h);
    }
};
struct VertexEqual {
    bool operator()(const std::vector<float> &a, const std::vector<float> &b) const {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (bitsOf(a[i]) != bitsOf(b[i])) return false;
        }
        return true;
    }
};

/**
 * Which vertices sit on the mesh boundary: touched by an edge that only one
 * triangle uses.
 *
 * This exists because meshopt_SimplifyLockBorder does not do it. Measured
 * directly against meshoptimizer 1.2 with no wrapper in between: asked to halve
 * a box subdivided 8x8 per face with LockBorder set, it keeps the four corners
 * of each face and discards the rest of the rim -- 80 of 192 boundary vertices
 * survive, and the ones that survive are exactly the corners. The straight run
 * of a rim is classified differently from its ends, so the option locks the
 * corners of a shared edge and not the edge.
 *
 * That is not cosmetic. Two adjacent chunks that share an edge each simplify it
 * their own way, the surfaces stop agreeing, and the seam opens into a crack.
 * The one option meant to prevent that prevents the corners of it.
 *
 * meshoptimizer 1.2 has the supported way to do this properly: the vertex_lock
 * array on meshopt_simplifyWithAttributes. So the mask is computed here and
 * passed explicitly. The bit is still set as well -- the two agree on a closed
 * mesh, and on an open one the explicit lock is the one that holds.
 */
std::vector<uint8_t> borderVertexMask(const uint32_t *indices, uint32_t indexCount,
                                      uint32_t vertexCount) {
    std::vector<uint8_t> mask(vertexCount, 0);
    if (indices == nullptr || indexCount < 3 || vertexCount == 0) return mask;

    // Undirected edge keys, sorted, so an edge that occurs once is a boundary
    // edge and one that occurs twice is interior. Sorting rather than hashing
    // because the order of a hash table is not a property worth depending on
    // when the answer decides which vertices may move.
    std::vector<uint64_t> edges(indexCount);
    for (size_t i = 0; i + 2 < indexCount; i += 3) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = indices[i + k];
            const uint32_t b = indices[i + (k + 1) % 3];
            edges[i + k] = 0;  // default for the out-of-range case below
            if (a >= vertexCount || b >= vertexCount) continue;
            const uint32_t lo = a < b ? a : b;
            const uint32_t hi = a < b ? b : a;
            edges[i + k] = (static_cast<uint64_t>(lo) << 32) | hi;
        }
    }
    std::sort(edges.begin(), edges.end());
    for (size_t i = 0; i < edges.size();) {
        size_t j = i;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        if (j - i == 1 && edges[i] != 0) {
            mask[static_cast<uint32_t>(edges[i] >> 32)] = 1;
            mask[static_cast<uint32_t>(edges[i] & 0xffffffffu)] = 1;
        }
        i = j;
    }
    return mask;
}

}  // namespace

// ---------------------------------------------------------------------------
// Welding
// ---------------------------------------------------------------------------

std::vector<uint32_t> weldVertices(const float *attributes, uint32_t vertexCount,
                                   uint32_t attributeCount, uint32_t &welded) {
    welded = 0;
    if (attributes == nullptr || vertexCount == 0 || attributeCount == 0) return {};

    std::unordered_map<std::vector<float>, uint32_t, VertexHash, VertexEqual> seen;
    seen.reserve(vertexCount);
    std::vector<uint32_t> remap(vertexCount, 0);
    std::vector<float> key(attributeCount);

    // Output order follows *input* order, not the hash table's. A hash table's
    // iteration order is an implementation detail; the vertex numbering the GPU
    // sees must not be.
    for (uint32_t v = 0; v < vertexCount; ++v) {
        std::memcpy(key.data(), attributes + static_cast<size_t>(v) * attributeCount,
                    attributeCount * sizeof(float));
        auto it = seen.find(key);
        if (it == seen.end()) {
            const uint32_t next = static_cast<uint32_t>(seen.size());
            seen.emplace(key, next);
            remap[v] = next;
        } else {
            remap[v] = it->second;
            ++welded;
        }
    }
    return remap;
}

// ---------------------------------------------------------------------------
// Deviation
// ---------------------------------------------------------------------------

float maxSurfaceDeviation(const float *referencePositions, uint32_t referenceVertexCount,
                          const uint32_t *referenceIndices, uint32_t referenceIndexCount,
                          const float *candidatePositions, uint32_t candidateVertexCount,
                          const uint32_t *candidateIndices, uint32_t candidateIndexCount) {
    (void)referenceIndices;
    (void)referenceIndexCount;
    if (referencePositions == nullptr || candidatePositions == nullptr) return 0.0f;
    if (referenceVertexCount == 0 || candidateVertexCount == 0) return 0.0f;
    if (candidateIndexCount < 3) return 0.0f;

    float worst = 0.0f;
    for (uint32_t v = 0; v < referenceVertexCount; ++v) {
        const float *p = referencePositions + static_cast<size_t>(v) * 3;
        float best = -1.0f;
        for (uint32_t t = 0; t + 2 < candidateIndexCount; t += 3) {
            const float *a = candidatePositions + static_cast<size_t>(candidateIndices[t + 0]) * 3;
            const float *b = candidatePositions + static_cast<size_t>(candidateIndices[t + 1]) * 3;
            const float *c = candidatePositions + static_cast<size_t>(candidateIndices[t + 2]) * 3;
            const float d = pointTriangleDistanceSq(p, a, b, c);
            if (best < 0.0f || d < best) best = d;
            // A point exactly on a triangle cannot be improved on, and every
            // remaining triangle is a strict upper bound on the answer.
            if (best == 0.0f) break;
        }
        if (best > worst) worst = best;
    }
    return std::sqrt(worst);
}

// ---------------------------------------------------------------------------
// The chain
// ---------------------------------------------------------------------------

uint32_t LodChain::triangleCountAt(uint32_t level) const {
    if (level >= levels.size()) return 0;
    return levels[level].triangleCount();
}

LodChain buildLodChain(const float *positions, uint32_t vertexCount,
                       const uint32_t *indices, uint32_t indexCount,
                       const LodBuildOptions &options) {
    LodChain chain;
    if (positions == nullptr || indices == nullptr || vertexCount == 0 || indexCount < 3) {
        chain.warning = "empty input";
        return chain;
    }

    chain.sourceTriangles = indexCount / 3;
    chain.sourceVertexCount = vertexCount;
    chain.vertexCount = vertexCount;
    chain.sourceAcmr = meshopt_analyzeVertexCache(indices, indexCount, vertexCount, 32, 0, 0).acmr;

    // The cone-culling precondition. See signedVolume(): a cone that bounds an
    // open sheet's normals will reject that sheet. Decided once, here, from the
    // whole mesh, and recorded per cluster so the consumer never has to guess.
    const float volume = signedVolume(positions, indices, indexCount);
    const bool closed = volume > 0.0f;

    const size_t kStride = 3 * sizeof(float);
    const float scale = meshopt_simplifyScale(positions, vertexCount, kStride);

    // A zero scale means every vertex is at the origin. meshoptimizer returns
    // 1.0 in that case, but a chain whose error budget is 0 * 1.0 = 0 would
    // then be unable to simplify at all, which is a silent LOD off-switch.
    const float errorScale = scale > 0.0f ? scale : 1.0f;

    const uint32_t requested = options.levelCount == 0 ? 1 : options.levelCount;

    // Level 0 is the input, reordered for the vertex cache and nothing else.
    // Reordering is the one change allowed at level 0: it does not move a
    // vertex, and an unoptimised index buffer makes every later ACMR figure
    // meaningless.
    std::vector<uint32_t> current(indices, indices + indexCount);
    std::vector<uint32_t> reordered(current.size());
    meshopt_optimizeVertexCache(reordered.data(), current.data(), current.size(), vertexCount);
    current.swap(reordered);

    uint32_t previousTriangles = 0;
    for (uint32_t level = 0; level < requested; ++level) {
        LodLevel out;
        const float ratio = level < options.ratios.size() ? options.ratios[level] : 0.0f;
        const float budget = level < options.errorBudgets.size() ? options.errorBudgets[level]
                                                                 : 0.0f;

        if (level == 0) {
            out.indices = current;
            out.error = 0.0f;
            out.acmr = chain.sourceAcmr;
            out.valid = true;
            chain.levels.push_back(std::move(out));
            previousTriangles = current.size() / 3;
            continue;
        }

        if (ratio <= 0.0f || ratio >= 1.0f) {
            ++chain.levelsMissing;
            if (chain.warning.empty()) {
                chain.warning = "level " + std::to_string(level) + " has ratio " +
                                std::to_string(ratio) + ", which is not a reduction";
            }
            continue;
        }

        // Never ask for fewer than one triangle. `target_index_count == 0` is
        // legal input that means "collapse everything", which for a scene slice
        // means a hole where a building was.
        const uint32_t targetTriangles = std::max(1u, static_cast<uint32_t>(
                                                   static_cast<float>(previousTriangles) * ratio));
        const uint32_t targetIndices = targetTriangles * 3;

        if (targetIndices >= current.size()) {
            // The ratio did not reduce anything, so this level is a duplicate of
            // the last one. Copying it would be a level that costs memory and
            // changes nothing; leaving a gap would make the draw path index
            // past the end. Report it and stop.
            ++chain.levelsMissing;
            if (chain.warning.empty()) {
                chain.warning = "level " + std::to_string(level) +
                                " does not reduce the triangle count";
            }
            break;
        }

        // The destination is sized to the *input*, not to the target. The
        // header says so in as many words ("worst case is index_count
        // elements, *not* target_index_count"), and getting it wrong is a heap
        // overflow rather than a wrong answer, because the simplifier stops
        // early on topology constraints and writes back everything it kept.
        // AddressSanitizer is the only reason this was found before shipping.
        std::vector<uint32_t> simplified(current.size());
        // meshopt_simplify takes a *relative* error. The budget here is metres,
        // so it is divided by the scale on the way in and multiplied back on
        // the way out. Getting this backwards produces a chain whose coarsest
        // level is either identical to the source or a bag of spikes, and both
        // look plausible in a screenshot.
        const float relativeBudget = budget / errorScale;
        float relativeError = 0.0f;
        size_t produced;
        if (options.lockBorder) {
            // The explicit lock, with no attributes. meshoptimizer ranks
            // collapses by position alone when attribute_count is zero, which
            // is exactly what meshopt_simplify does, so nothing else about the
            // simplification changes.
            const std::vector<uint8_t> border = borderVertexMask(
                current.data(), static_cast<uint32_t>(current.size()), vertexCount);
            std::vector<uint8_t> lock(vertexCount, 0);
            uint32_t lockedCount = 0;
            for (uint32_t v = 0; v < vertexCount; ++v) {
                if (border[v]) {
                    lock[v] = meshopt_SimplifyVertex_Lock;
                    ++lockedCount;
                }
            }
            produced = meshopt_simplifyWithAttributes(
                simplified.data(), current.data(), current.size(), positions, vertexCount, kStride,
                nullptr, 0, nullptr, 0, lockedCount ? lock.data() : nullptr, targetIndices,
                relativeBudget, static_cast<unsigned>(meshopt_SimplifyLockBorder), &relativeError);
        } else {
            produced = meshopt_simplify(simplified.data(), current.data(), current.size(), positions,
                                        vertexCount, kStride, targetIndices, relativeBudget, 0u,
                                        &relativeError);
        }
        simplified.resize(produced);

        if (simplified.size() < 3) {
            ++chain.levelsMissing;
            if (chain.warning.empty()) {
                chain.warning = "level " + std::to_string(level) + " simplified to nothing";
            }
            break;
        }

        // The request was valid but the *result* is not a reduction.
        //
        // This is not hypothetical. A 12-triangle box is asked for 6, 3 and 1
        // triangles and gets 12, 12 and 12 back, because a closed box has a
        // topology floor -- roughly four triangles -- and the simplifier
        // correctly refuses to go below it. The guard above only checks what
        // was *asked* for, so it waved all three through and the chain came
        // back with four byte-identical levels, three of which cost memory and
        // draw exactly the same pixels as the first.
        //
        // The symptom in a shipped build is a LOD system that is switched on,
        // reports four levels, and is doing nothing: the cost is memory and the
        // frame time, and the reason is not visible from the outside.
        if (simplified.size() >= current.size()) {
            ++chain.levelsMissing;
            if (chain.warning.empty()) {
                chain.warning = "level " + std::to_string(level) +
                                " could not reduce " + std::to_string(current.size() / 3) +
                                " triangles, so the chain stops here";
            }
            break;
        }

        // Prune the slivers that collapse leaves behind. Without this a LOD
        // chain's triangle count is right and its *pixel* count is not: a
        // hundred one-pixel triangles cost as much to raster as a hundred
        // full-screen ones, and they are the ones that shimmer as the camera
        // moves.
        std::vector<uint32_t> pruned(simplified.size());
        const size_t prunedCount = meshopt_simplifyPrune(
            pruned.data(), simplified.data(), simplified.size(), positions, vertexCount, kStride,
            relativeBudget);
        if (prunedCount >= 3) {
            pruned.resize(prunedCount);
            simplified.swap(pruned);
        }

        std::vector<uint32_t> lodReordered(simplified.size());
        meshopt_optimizeVertexCache(lodReordered.data(), simplified.data(), simplified.size(),
                                    vertexCount);
        simplified.swap(lodReordered);
        out.indices = std::move(simplified);
        out.error = relativeError * errorScale;
        out.acmr = meshopt_analyzeVertexCache(out.indices.data(), out.indices.size(), vertexCount,
                                              32, 0, 0)
                       .acmr;
        out.valid = true;

        previousTriangles = out.indices.size() / 3;
        current = out.indices;
        chain.levels.push_back(std::move(out));
    }

    if (chain.levels.empty()) {
        chain.warning = "no level could be built";
        return chain;
    }

    // -- clusters -----------------------------------------------------------
    //
    // Built from level 0 only. Clusters are the culling unit, and a cluster
    // whose bounds were the union of its triangles at every level would be the
    // bounds of the finest level and would therefore cull nothing. The coarser
    // levels are drawn with the same clusters, which is correct: the coarser
    // surface lies inside the finer one's bounds.
    //
    // THE ORDERING PROBLEM, AND WHY LEVEL 0 IS REBUILT HERE.
    //
    // meshopt_buildMeshlets takes a cone_weight and is by far the better
    // partitioner -- it groups triangles by direction as well as by position,
    // which is what makes a cluster's cone tight enough to cull with. It sorts
    // the triangles internally, and its `triangle_offset` is an offset into
    // *its own* order, not into the index buffer it was handed.
    //
    // Taking those offsets as offsets into our buffer is the bug the first
    // version of this file had, twice over: the offsets were off by a factor of
    // three (triangle_offset is already in indices, clusterizer.cpp:269), and
    // they addressed a reordering that had not been applied. The result is a
    // bounding sphere computed over somebody else's triangles, which culls real
    // buildings and shows up in a still screenshot as a city with holes in it.
    //
    // The scan builder fixes the offsets and ruins the cones. Measured on the
    // fixtures in this repo: 12 clusters and 0 usable cones on a 768-triangle
    // box, where the cone-aware builder on the same mesh gives usable cones.
    // Culling that costs a dot product and buys nothing is not a trade worth
    // making when the alternative is to stop trusting the offsets.
    //
    // So neither offset is used. Each meshlet's triangles are unpacked by hand
    // -- `meshlet_vertices[meshlet_triangles[i]]` is the global vertex, which is
    // the layout the header documents -- and level 0's index buffer is rebuilt
    // as the concatenation of the clusters, in cluster order. The offsets are
    // then true by construction rather than true by assumption, the cones come
    // from the good partitioner, and level 0 ends up in the order a
    // cluster-based renderer wants anyway: all of a cluster's triangles
    // together, because that is the unit it draws.
    const std::vector<uint32_t> source = chain.levels[0].indices;
    const uint32_t maxClusterVerts = std::min(256u, std::max(3u, options.clusterMaxVertices));
    const uint32_t maxClusterTris = std::min(512u, std::max(1u, options.clusterMaxTriangles));
    const size_t maxMeshlets =
        meshopt_buildMeshletsBound(source.size(), maxClusterVerts, maxClusterTris);
    std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
    std::vector<uint32_t> meshletVertices(source.size());
    std::vector<uint8_t> meshletTriangles(source.size());

    const size_t meshletCount = meshopt_buildMeshlets(
        meshlets.data(), meshletVertices.data(), meshletTriangles.data(), source.data(),
        source.size(), positions, vertexCount, kStride, maxClusterVerts, maxClusterTris,
        options.clusterConeWeight);

    std::vector<uint32_t> clusterOrdered;
    clusterOrdered.reserve(source.size());
    chain.clusters.reserve(meshletCount);

    for (size_t m = 0; m < meshletCount; ++m) {
        const meshopt_Meshlet &ml = meshlets[m];
        const uint32_t *mv = meshletVertices.data() + ml.vertex_offset;
        const uint8_t *mt = meshletTriangles.data() + ml.triangle_offset;

        // Intra-cluster ordering only. This writes inside one meshlet and never
        // moves a meshlet, and -- unlike the index offsets -- the array it
        // writes is ours to rebuild, so it cannot invalidate anything above.
        std::vector<uint32_t> localVerts(mv, mv + ml.vertex_count);
        std::vector<uint8_t> localTris(mt, mt + static_cast<size_t>(ml.triangle_count) * 3);
        meshopt_optimizeMeshlet(localVerts.data(), localTris.data(), ml.triangle_count,
                                ml.vertex_count);

        LodCluster c;
        c.indexOffset = static_cast<uint32_t>(clusterOrdered.size());
        // c.indexCount is deliberately NOT set from triangle_count here. It was,
        // once, and it was dead: the loop below computes the real length from
        // how many corners were actually appended, and overwrites it. The
        // mutation harness proved the assignment was unreachable-in-effect by
        // multiplying it by three and watching every test still pass -- a
        // control that survived by finding dead code rather than a missing
        // test, which is the better of the two things a control can do.

        uint32_t lo = UINT32_MAX;
        uint32_t hi = 0;
        for (uint32_t t = 0; t < ml.triangle_count; ++t) {
            for (int k = 0; k < 3; ++k) {
                const uint8_t local = localTris[t * 3 + k];
                // A local index outside the meshlet's own vertex array is not
                // a thing the clusterizer produces; if it ever did, indexing
                // with it would be an out-of-bounds read with no diagnostic.
                if (local >= ml.vertex_count) continue;
                const uint32_t global = localVerts[local];
                clusterOrdered.push_back(global);
                lo = std::min(lo, global);
                hi = std::max(hi, global);
            }
        }

        if (clusterOrdered.size() - c.indexOffset < 3) continue;  // nothing usable
        c.indexCount = static_cast<uint32_t>(clusterOrdered.size()) - c.indexOffset;
        c.vertexOffset = lo == UINT32_MAX ? 0 : lo;
        c.vertexCount = lo == UINT32_MAX ? 0 : hi - lo + 1;

        // Bounds from the absolute indices just written, so the sphere and the
        // cone are computed over exactly the triangles the cluster owns. This
        // is the same function whose cone semantics adaptive.hpp's cull is
        // written against, including the radius-aware rejection test.
        const meshopt_Bounds bounds = meshopt_computeClusterBounds(
            clusterOrdered.data() + c.indexOffset, c.indexCount, positions, vertexCount, kStride);

        c.sphereCenter[0] = bounds.center[0];
        c.sphereCenter[1] = bounds.center[1];
        c.sphereCenter[2] = bounds.center[2];
        c.sphereRadius = bounds.radius;
        c.coneApex[0] = bounds.cone_apex[0];
        c.coneApex[1] = bounds.cone_apex[1];
        c.coneApex[2] = bounds.cone_apex[2];
        c.coneAxis[0] = bounds.cone_axis[0];
        c.coneAxis[1] = bounds.cone_axis[1];
        c.coneAxis[2] = bounds.cone_axis[2];
        c.coneCos = bounds.cone_cutoff;
        // Two independent reasons a cone must not be used, and both are
        // recorded rather than one being allowed to mask the other: the mesh is
        // not a closed volume, or this particular cluster's cone is wider than
        // a hemisphere and would reject everything.
        c.twoSided = !closed;
        //
        // The degenerate-cone sentinel, and it is a trap.
        //
        // When a cluster's normals span more than about 168 degrees,
        // meshoptimizer sets cone_cutoff = 1 and RETURNS EARLY -- it never
        // writes cone_axis or cone_apex, which stay zero. Its own guidance is
        // "trivial accept": the cone is not usable.
        //
        // So the usable test is `cutoff > 0 AND cutoff < 1`, not `cutoff > 0`.
        // The first version of this line used the latter, and a cluster with a
        // zero axis and a cutoff of 1 sailed through: an eye 20 m "behind" it
        // computed a dot product of 0 against a unit cutoff, compared false, and
        // the cluster was marked cullable by a cone that does not exist. The
        // assertion "every usable cone rejects an eye behind it" is what found
        // it -- the cone had no direction to be behind.
        const float axisLen2 = bounds.cone_axis[0] * bounds.cone_axis[0] +
                               bounds.cone_axis[1] * bounds.cone_axis[1] +
                               bounds.cone_axis[2] * bounds.cone_axis[2];
        c.coneUsable = closed && bounds.cone_cutoff > 0.0f && bounds.cone_cutoff < 1.0f &&
                       axisLen2 > 0.5f;  // a unit axis is len2 == 1; 0.5 rejects the zero axis

        chain.clusters.push_back(c);
    }

    // Level 0 is now the cluster-ordered buffer, and the clusters index it.
    // The triangle count is unchanged -- the same triangles, rearranged -- but
    // the ACMR is not, so it is measured again rather than carried over from the
    // buffer that no longer exists.
    chain.levels[0].indices = std::move(clusterOrdered);
    chain.levels[0].acmr =
        meshopt_analyzeVertexCache(chain.levels[0].indices.data(), chain.levels[0].indices.size(),
                                   vertexCount, 32, 0, 0)
            .acmr;

    if (chain.clusters.empty()) {
        chain.warning += chain.warning.empty() ? "" : "; ";
        chain.warning += "the clusterizer produced no clusters";
    }

    return chain;
}

}  // namespace emergent
