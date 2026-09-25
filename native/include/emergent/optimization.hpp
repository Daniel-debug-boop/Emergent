#pragma once
#include <cstdint>
#include <vector>

namespace emergent {
struct OptimizedMesh {
    std::vector<uint32_t> indices;
    uint32_t vertex_count = 0;
    float acmr = 0.0f;
};

// meshoptimizer is a mandatory native asset-pipeline dependency.
OptimizedMesh optimizeMesh(const std::vector<uint32_t>& indices, uint32_t vertexCount);
}
