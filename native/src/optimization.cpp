#include "emergent/optimization.hpp"
#include <meshoptimizer.h>
#include <stdexcept>

namespace emergent {
OptimizedMesh optimizeMesh(const std::vector<uint32_t>& indices, uint32_t vertexCount) {
    if (indices.empty() || vertexCount == 0) return {};
    OptimizedMesh out;
    out.vertex_count = vertexCount;
    out.indices.resize(indices.size());
    meshopt_optimizeVertexCache(out.indices.data(), indices.data(), indices.size(), vertexCount);
    // meshoptimizer >= 0.22 returns a statistics struct rather than a bare
    // float; the ACMR figure EMERGENT reports lives in its `acmr` member.
    const meshopt_VertexCacheStatistics stats =
        meshopt_analyzeVertexCache(out.indices.data(), out.indices.size(), vertexCount, 32, 0, 0);
    out.acmr = stats.acmr;
    return out;
}
}
