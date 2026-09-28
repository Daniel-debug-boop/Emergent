// Adaptive visibility: the hierarchical, temporally cached visibility field.
//
// The header carries the argument for why this is shaped the way it is. This
// file is the arithmetic, and the one rule it obeys everywhere is the one from
// the header: a test may only ever report a *definite* reject. Every `return
// false` below means "not proven invisible, so keep it", and those are the
// lines that keep the invariant true.

#include "emergent/adaptive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace emergent {

/** Clamp on a double, declared before the anonymous namespace so the cone
 *  helper below can use it without a second overload at every call site. */
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

namespace {

inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void normalize3(float v[3]) {
    const float len = std::sqrt(dot3(v, v));
    if (len > 1e-20f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** Hash mix, so a signature changes when any contributing field changes. */
inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t bitsOf(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return static_cast<uint64_t>(u);
}

/** The scene mesh's interleaved layout: position(3) normal(3) colour(3) material(1) uv(2). */
constexpr uint32_t kPosOffset = 0;
constexpr uint32_t kNormalOffset = 3;
constexpr uint32_t kMaterialOffset = 9;
constexpr uint32_t kMeshFloatsPerVertex = 12;

/** Sphere swept by a cone of half-angle `cosAngle` against a plane normal. */
inline float coneSupportRadius(const Cone &cone) {
    // sin(angle) = sqrt(1 - cos^2), clamped: a cone that accepts everything has
    // no meaningful support and must be treated as unbounded.
    const double c = clampd(cone.cosAngle, -1.0, 1.0);
    return static_cast<float>(std::sqrt(std::max(0.0, 1.0 - c * c)));
}

} // namespace

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

std::string describeInvalidation(uint32_t mask) {
    if (mask == kInvNone) return "none";
    struct Bit { uint32_t bit; const char *name; };
    static const Bit bits[] = {
        {kInvTransform, "transform"}, {kInvGeometry, "geometry"},   {kInvMaterial, "material"},
        {kInvAnimation, "animation"}, {kInvSpawned, "spawned"},     {kInvDestroyed, "destroyed"},
        {kInvEnabled, "enabled"},     {kInvLod, "lod"},             {kInvTerrain, "terrain"},
        {kInvStreaming, "streaming"}, {kInvLighting, "lighting"},   {kInvProjection, "projection"},
        {kInvTeleport, "teleport"},   {kInvOriginShift, "origin"},  {kInvOcclusion, "occlusion"},
        {kInvBudget, "budget"},
    };
    std::string out;
    for (const Bit &b : bits) {
        if (!(mask & b.bit)) continue;
        if (!out.empty()) out += "|";
        out += b.name;
    }
    return out;
}

std::string describeDecision(Decision d) {
    switch (d) {
        case Decision::kReuse: return "REUSE";
        case Decision::kValidate: return "VALIDATE";
        case Decision::kFullRecompute: return "FULL";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

std::string AdaptiveMetrics::describe() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "clusters %u | full %u | validate %u | reuse %u | reuseRatio %.3f | "
                  "frustumRej %u | coneRej %u | hizRej %u | visible %u | lodTrans %u | indirect %u | inval %u",
                  totalClusters, fullRecomputes, validations, reuses, reuseRatio(),
                  frustumRejects, coneRejects, hizRejects, visibleClusters, lodTransitions,
                  indirectDraws, invalidations);
    return buf;
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void CameraState::recomputeSignature() {
    // Every field that changes the visible set goes in. `velocity` does not,
    // because it is derived from consecutive positions, both of which are
    // already in the signature — including it would make the signature change
    // on a frame where the visible set could not have.
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 3; ++i) h = mix(h, bitsOf(position[i]));
    h = mix(h, bitsOf(yaw));
    h = mix(h, bitsOf(pitch));
    h = mix(h, bitsOf(fovY));
    h = mix(h, bitsOf(aspect));
    h = mix(h, bitsOf(zNear));
    h = mix(h, bitsOf(zFar));
    h = mix(h, bitsOf(viewHeightPx));
    h = mix(h, static_cast<uint64_t>(projectionVersion));
    h = mix(h, static_cast<uint64_t>(originVersion));
    signature = h;
}

// ---------------------------------------------------------------------------
// Frustum extraction
// ---------------------------------------------------------------------------

void extractFrustumPlanes(const float *m, Frustum &out) {
    // Column-major: element [c * 4 + r] is row r of column c, which is the
    // layout `mul` produces in math3d.mjs. Getting this backwards yields a
    // frustum that looks plausible and culls the wrong half of the world, so it
    // is the same expression as culling.mjs rather than a fresh one.
    struct Row { float v[4]; };
    auto row = [&](int r) {
        Row result{};
        for (int c = 0; c < 4; ++c) result.v[c] = m[c * 4 + r];
        return result;
    };
    const Row r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    auto set = [&](int index, float a, float b, float c, float d) {
        const float inv = 1.0f / std::sqrt(a * a + b * b + c * c);
        out.planes[index * 4 + 0] = a * inv;
        out.planes[index * 4 + 1] = b * inv;
        out.planes[index * 4 + 2] = c * inv;
        out.planes[index * 4 + 3] = d * inv;
    };
    set(Frustum::kLeft,   r3.v[0] + r0.v[0], r3.v[1] + r0.v[1], r3.v[2] + r0.v[2], r3.v[3] + r0.v[3]);
    set(Frustum::kRight,  r3.v[0] - r0.v[0], r3.v[1] - r0.v[1], r3.v[2] - r0.v[2], r3.v[3] - r0.v[3]);
    set(Frustum::kBottom, r3.v[0] + r1.v[0], r3.v[1] + r1.v[1], r3.v[2] + r1.v[2], r3.v[3] + r1.v[3]);
    set(Frustum::kTop,    r3.v[0] - r1.v[0], r3.v[1] - r1.v[1], r3.v[2] - r1.v[2], r3.v[3] - r1.v[3]);
    set(Frustum::kNear,   r3.v[0] + r2.v[0], r3.v[1] + r2.v[1], r3.v[2] + r2.v[2], r3.v[3] + r2.v[3]);
    set(Frustum::kFar,    r3.v[0] - r2.v[0], r3.v[1] - r2.v[1], r3.v[2] - r2.v[2], r3.v[3] - r2.v[3]);
}

void extractFrustumPlanes(float m[16], Frustum &out) { extractFrustumPlanes(static_cast<const float *>(m), out); }

// ---------------------------------------------------------------------------
// Normal cone
// ---------------------------------------------------------------------------

Cone computeNormalCone(const float *vertices, uint32_t first, uint32_t count,
                       uint32_t floatsPerVertex, bool twoSided, bool closed) {
    // The three cases that must not produce a usable cone, and why. Each of
    // them produces false *negatives* if used, which is the one failure this
    // module cannot have:
    //
    //   twoSided   a two-sided card is visible from behind its normal.
    //   !closed    a road, a fence or a ground quad has no meaningful
    //              "outward" direction, so the average normal is noise.
    //   degenerate a zero-length or non-converging normal average.
    if (twoSided || !closed || vertices == nullptr || count < 3) return Cone::invalid();

    float axis[3] = {0, 0, 0};
    // The worst alignment between any face normal and the eventual mean axis.
    // This is the quantity that makes the cone *tight* rather than merely
    // present: without it every cone would have to be declared as accepting
    // every direction, which is not a cone at all.
    float worst = 1.0f;
    uint32_t used = 0;
    // A second pass is needed because the axis is not known until every normal
    // has been seen. Normals are cheap to re-walk and storing them would mean
    // an allocation per cluster, so they are re-derived instead.
    for (uint32_t v = 0; v < count; v += 3) {
        const uint32_t vi = first + v;
        if (vi + 2 >= first + count) break;
        const float *base = vertices + static_cast<size_t>(vi) * floatsPerVertex;
        const float *a = base + kPosOffset;
        const float *b = base + floatsPerVertex + kPosOffset;
        const float *c = base + 2 * floatsPerVertex + kPosOffset;

        const float e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e0[1] * e1[2] - e0[2] * e1[1],
                      e0[2] * e1[0] - e0[0] * e1[2],
                      e0[0] * e1[1] - e0[1] * e1[0]};
        if (dot3(n, n) < 1e-18f) continue;  // degenerate sliver
        normalize3(n);
        axis[0] += n[0];
        axis[1] += n[1];
        axis[2] += n[2];
        ++used;
    }
    if (used == 0) return Cone::invalid();

    // If the face normals cancel, there is no dominant direction and any axis
    // derived from them is noise. This is the cube case, and it is the single
    // most important rejection in this function.
    const float sumLenSq = dot3(axis, axis);
    if (sumLenSq < 0.25f) return Cone::invalid();
    normalize3(axis);

    for (uint32_t v = 0; v < count; v += 3) {
        const uint32_t vi = first + v;
        if (vi + 2 >= first + count) break;
        const float *base = vertices + static_cast<size_t>(vi) * floatsPerVertex;
        const float *a = base + kPosOffset;
        const float *b = base + floatsPerVertex + kPosOffset;
        const float *c = base + 2 * floatsPerVertex + kPosOffset;
        const float e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e0[1] * e1[2] - e0[2] * e1[1],
                      e0[2] * e1[0] - e0[0] * e1[2],
                      e0[0] * e1[1] - e0[1] * e1[0]};
        if (dot3(n, n) < 1e-18f) continue;
        normalize3(n);
        const float d = dot3(n, axis);
        if (d < worst) worst = d;
    }

    Cone cone;
    cone.axis[0] = axis[0];
    cone.axis[1] = axis[1];
    cone.axis[2] = axis[2];
    cone.cosAngle = worst;
    // A half-angle at or beyond 90 degrees is refused. See the header: the
    // frustum-versus-cone test is not sound for a shape that spans more than a
    // hemisphere, and a false negative is the one thing this module forbids.
    if (cone.cosAngle <= Cone::kMinUsableCos) return Cone::invalid();
    return cone;
}

// ---------------------------------------------------------------------------
// The conservative reject
// ---------------------------------------------------------------------------

bool definitelyNotVisible(const Cluster &c, const Frustum &frustum, const float eye[3],
                          float drawDistance, bool shadowDomain) {
    // Domain separation. A cluster can be irrelevant to the camera and still
    // cast a shadow, and reusing camera visibility for the shadow pass is how
    // a building loses its own shadow and nobody notices until the sun angle
    // makes it obvious. In the shadow domain only a non-caster can be rejected
    // on the strength of its own flag.
    if (shadowDomain) return !c.castsShadow;

    // Sphere against the frustum. This is the exact test for the sphere, so a
    // reject here is a proof.
    if (!frustum.intersects(c.sphere)) return true;

    // Distance. The sphere test against the far plane covers most of this, but
    // an explicit radial cut is cheaper and is the thing that keeps the
    // evaluate loop bounded when the far plane is generous.
    const float dx = c.sphere.center[0] - eye[0];
    const float dy = c.sphere.center[1] - eye[1];
    const float dz = c.sphere.center[2] - eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float reach = drawDistance + c.sphere.radius;
    if (d2 > reach * reach) return true;

    // Normal cone: a back-facing test.
    //
    // The first version of this asked whether the cone's support along each
    // frustum plane normal cleared the plane -- i.e. `dot(n, centre) + d +
    // radius * sin(angle) < 0`. That formulation is dead code, and proving so
    // is why it is worth recording: `radius * sin(angle)` is at most `radius`,
    // so the cone's reach is never wider than the bounding sphere's own reach,
    // and the sphere test on the line above has already rejected everything it
    // could. The test could never fire. The test suite's "one-sided cluster is
    // rejected by its cone" assertion is what exposed it.
    //
    // What a normal cone is actually for is facing. A bounding sphere has no
    // facing, so a sphere test cannot tell the back of a building from its
    // front; a cone can. The sound test is the one meshoptimizer uses, and
    // meshoptimizer is already a dependency of this project:
    //
    //     visible  <=>  dot(axis, normalize(centre - eye)) <= sin(angle)
    //
    // The direction `centre - eye` points *away* from the camera, and the cone
    // axis points along the surface normal. So for a wall whose normal faces
    // the camera those two are opposed and the dot product is near -1, and for
    // a wall turned away they agree and it is near +1. Rejecting when the dot
        // exceeds `sin(angle)` therefore rejects exactly the back-facing case.
    //
    // Getting this sign backwards rejects precisely the walls that are looking
    // at you, which is the most spectacular possible way for a culler to be
    // wrong and the first thing the "a wall facing the camera is kept"
    // assertion caught.
    //
    // Gated hard, because the same three cases make the test unsound rather
    // than merely imprecise: no cone, a cone of 90 degrees or more, and
    // two-sided geometry. All three fall through to "keep it".
    if (c.cone.valid() && !c.twoSided) {
        const float vx = c.sphere.center[0] - eye[0];
        const float vy = c.sphere.center[1] - eye[1];
        const float vz = c.sphere.center[2] - eye[2];
        const float viewD2 = vx * vx + vy * vy + vz * vz;
        if (viewD2 > 1e-12f) {
            const float invD = 1.0f / std::sqrt(viewD2);
            const double cosA = clampd(c.cone.cosAngle, -1.0, 1.0);
            const double sinA = std::sqrt(std::max(0.0, 1.0 - cosA * cosA));
            const double facing = (vx * invD) * c.cone.axis[0] + (vy * invD) * c.cone.axis[1] +
                                  (vz * invD) * c.cone.axis[2];
            if (facing > sinA) return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------

AdaptiveVisibility::AdaptiveVisibility() = default;

void AdaptiveVisibility::clear() {
    clusters_.clear();
    cellBounds_.clear();
    cellFirstCluster_.clear();
    cellClusterCount_.clear();
    sectorBounds_.clear();
    sectorFirstCell_.clear();
    sectorCellCount_.clear();
    visible_.clear();
    metrics_.reset();
    meshletCount_ = 0;
}

void AdaptiveVisibility::buildFromScene(const float *vertices, uint32_t vertexCount,
                                        uint32_t floatsPerVertex, uint32_t materialStride,
                                        float cellSize, uint32_t clusterTargetVertices) {
    clear();
    if (vertices == nullptr || vertexCount == 0) return;
    if (floatsPerVertex == 0) floatsPerVertex = kMeshFloatsPerVertex;
    if (materialStride == 0) materialStride = kMaterialOffset;
    if (cellSize <= 0.0f) cellSize = 64.0f;
    if (clusterTargetVertices == 0) clusterTargetVertices = 1536;
    cellSize_ = cellSize;

    // Cut the mesh into contiguous clusters. A cluster is a vertex range, so
    // the draw call stays `vkCmdDrawIndirect` over the existing interleaved
    // buffer and nothing about the scene mesh changes.
    for (uint32_t start = 0; start + 2 < vertexCount; start += clusterTargetVertices) {
        const uint32_t end = std::min(vertexCount, start + clusterTargetVertices);
        // Never cut mid-triangle: a cluster that is not a whole number of
        // triangles produces a draw that renders half a triangle, which is both
        // visually wrong and an out-of-bounds read at the end of the buffer.
        uint32_t n = end - start;
        n -= n % 3;
        if (n < 3) break;
        const uint32_t count = std::min(n, clusterTargetVertices);
        const uint32_t stop = start + count;

        Cluster c;
        c.vertexOffset = start;
        c.vertexCount = count;
        c.bounds.reset();
        c.sphere.radius = 0.0f;
        uint32_t material = 0;
        bool haveMaterial = false;
        for (uint32_t v = start; v < stop; ++v) {
            const float *base = vertices + static_cast<size_t>(v) * floatsPerVertex;
            c.bounds.grow(base + kPosOffset);
            const uint32_t m = static_cast<uint32_t>(base[materialStride]);
            if (!haveMaterial) { material = m; haveMaterial = true; }
        }
        c.material = material;
        c.bounds.pad(0.001f);
        c.bounds.center(c.sphere.center);
        c.sphere.radius = c.bounds.radius();
        // Open geometry by default. A conservative default that costs a little
        // time and cannot cost a missing building; `computeNormalCone` is the
        // only way a cluster acquires a usable cone.
        c.cone = Cone::invalid();
        c.lod = 0;
        c.lodTriangles = count / 3;
        c.confidence = 1.0f;
        c.priority = static_cast<uint8_t>(RenderPriority::kMedium);
        // Cells are assigned in the hierarchy pass below, once the cell grid
        // exists. Using the cluster's own centre is enough to key it.
        clusters_.push_back(c);
    }

    rebuildHierarchy();

    // Attach a cone to each cluster that is closed and one-sided. The mesh
    // does not record closure, so this is attempted only where the normal
    // average converges to a direction, which is itself the test: a road or a
    // ground quad's normals fan out and the average falls apart, and
    // `computeNormalCone` returns invalid for exactly that case.
    for (Cluster &c : clusters_) {
        c.cone = computeNormalCone(vertices, c.vertexOffset, c.vertexCount, floatsPerVertex,
                                   c.twoSided, /*closed=*/true);
    }

    metrics_.totalClusters = static_cast<uint32_t>(clusters_.size());
}

void AdaptiveVisibility::rebuildHierarchy() {
    cellBounds_.clear();
    cellFirstCluster_.clear();
    cellClusterCount_.clear();
    sectorBounds_.clear();
    sectorFirstCell_.clear();
    sectorCellCount_.clear();
    if (clusters_.empty()) return;

    const float inv = 1.0f / cellSize_;
    // Cells are keyed by their integer grid position. The first version of this
    // built the map with a linear scan over the keys already seen, which is
    // O(clusters * cells) -- quadratic, and on a dense slice that is tens of
    // thousands squared. A hash map is not an optimisation here, it is the
    // difference between building the hierarchy and not.
    std::unordered_map<uint64_t, uint32_t> keyToCell;
    keyToCell.reserve(clusters_.size() * 2);

    for (uint32_t i = 0; i < clusters_.size(); ++i) {
        Cluster &c = clusters_[i];
        const int64_t cx = static_cast<int64_t>(std::floor(c.sphere.center[0] * inv));
        const int64_t cz = static_cast<int64_t>(std::floor(c.sphere.center[2] * inv));
        const uint64_t key = (static_cast<uint64_t>(cx) << 32) | static_cast<uint32_t>(cz);
        auto it = keyToCell.find(key);
        if (it == keyToCell.end()) {
            const uint32_t id = static_cast<uint32_t>(cellBounds_.size());
            keyToCell.emplace(key, id);
            cellBounds_.emplace_back();
            cellFirstCluster_.push_back(0);
            cellClusterCount_.push_back(0);
            c.cell = id;
        } else {
            c.cell = it->second;
        }
        c.sector = c.cell / 16;  // 16 cells to a sector
        cellBounds_[c.cell].grow(c.bounds);
        cellClusterCount_[c.cell]++;
    }

    // Sectors are contiguous runs of 16 cells in first-seen cell order. This is
    // a coarse grouping, not a spatial one, and it is fine: the sector level
    // exists to reject large regions cheaply, and a bad grouping costs a little
    // redundant work, never a wrong answer.
    const uint32_t sectorCount = (static_cast<uint32_t>(cellBounds_.size()) + 15) / 16;
    sectorBounds_.assign(sectorCount, Aabb{});
    sectorFirstCell_.assign(sectorCount, 0);
    sectorCellCount_.assign(sectorCount, 0);
    for (uint32_t i = 0; i < cellBounds_.size(); ++i) {
        const uint32_t s = i / 16;
        if (sectorCellCount_[s] == 0) sectorFirstCell_[s] = i;
        sectorCellCount_[s]++;
        sectorBounds_[s].grow(cellBounds_[i]);
    }
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

CameraMotion AdaptiveVisibility::classifyMotion(const CameraState &camera, const CameraState &previous) {
    // Order matters. A teleport is not a fast move, it is a discontinuity: no
    // amount of "the camera moved a lot" reasoning makes a cached answer from
    // the old position meaningful, so it is detected first and on its own
    // signal rather than by thresholding speed.
    if (camera.originVersion != previous.originVersion) { motion_ = CameraMotion::kOriginShift; return motion_; }
    if (camera.projectionVersion != previous.projectionVersion) {
        motion_ = CameraMotion::kProjectionChanged;
        return motion_;
    }

    float moved = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = camera.position[i] - previous.position[i];
        moved += d * d;
    }
    moved = std::sqrt(moved);

    float turned = std::fabs(camera.yaw - previous.yaw) + std::fabs(camera.pitch - previous.pitch);

    // A teleport is a jump that no plausible walking speed could produce in one
    // frame. The threshold is deliberately far above the fast-movement band, so
    // a genuinely fast camera is classified as fast (which still invalidates)
    // rather than as a teleport (which invalidates harder than necessary).
    if (moved > 200.0f) { motion_ = CameraMotion::kTeleport; return motion_; }

    if (moved <= config_.stationarySpeed && turned <= config_.rotationThreshold) {
        motion_ = CameraMotion::kStationary;
    } else if (turned > config_.rotationThreshold && moved <= config_.stationarySpeed) {
        motion_ = CameraMotion::kRotating;
    } else if (moved > config_.slowSpeed) {
        motion_ = CameraMotion::kFast;
    } else {
        motion_ = CameraMotion::kSlow;
    }
    return motion_;
}

float AdaptiveVisibility::effectiveTrustRadius() const {
    // This is the heart of the camera-velocity policy, so the scaling is stated
    // rather than tuned in place. The reasoning:
    //
    //   Stationary  A camera that has not moved has a stable visible set, so the
    //               cached answer is very likely still right. Trust it far.
    //   Slow        The visible set changes at the edges of the frustum, and the
    //               clusters that are entering it are the near ones anyway, so a
    //               long trust radius is safe and saves the most work.
    //   Rotating    Rotation sweeps the frustum across new geometry faster than
    //               translation does, because a distant cluster leaves the view
    //               when the *edge* of the frustum passes it, not the centre.
    //               This is the case that punishes a long trust radius, so it
    //               gets the shortest one for a non-discontinuous motion.
    //   Fast        A fast camera is about to be somewhere else. Re-deriving now
    //               is cheaper than correcting a pop later.
    //
    // `classifyMotion` records the class; this reads it. They are separate
    // functions because "how did the camera move" and "how much do I trust the
    // cache" are different questions with different answers.
    switch (motion_) {
        case CameraMotion::kStationary: return config_.trustRadius * 2.0f;
        case CameraMotion::kSlow: return config_.trustRadius;
        case CameraMotion::kRotating: return config_.trustRadius * 0.5f;
        case CameraMotion::kFast: return config_.trustRadius * 0.25f;
        // A discontinuity is not a trust question: everything is invalidated,
        // so the radius is irrelevant and is reported as zero.
        case CameraMotion::kTeleport:
        case CameraMotion::kProjectionChanged:
        case CameraMotion::kOriginShift:
            return 0.0f;
    }
    return config_.trustRadius;
}

// ---------------------------------------------------------------------------
// LOD
// ---------------------------------------------------------------------------

float AdaptiveVisibility::screenSpaceError(const Cluster &c, const CameraState &camera) const {
    // Projected size in pixels, from the same projection the renderer uses.
    // The focal term is viewHeight / (2 tan(fovY/2)), which is the standard
    // pinhole conversion from a world-space extent to pixels at unit distance.
    const float focal = camera.viewHeightPx / (2.0f * std::tan(0.5f * camera.fovY));
    const float d = std::sqrt((c.sphere.center[0] - camera.position[0]) * (c.sphere.center[0] - camera.position[0]) +
                              (c.sphere.center[1] - camera.position[1]) * (c.sphere.center[1] - camera.position[1]) +
                              (c.sphere.center[2] - camera.position[2]) * (c.sphere.center[2] - camera.position[2]));
    if (d <= 1e-4f) return 1e9f;
    // Geometric error is taken as the cluster's radius: the distance from its
    // surface to its centre is the largest deviation any point can have from
    // the exact silhouette, so it is the right conservative bound and it is
    // available without a per-LOD error table.
    return (c.sphere.radius * focal) / d;
}

uint32_t AdaptiveVisibility::lodFor(const Cluster &c, const CameraState &camera) const {
    const float err = screenSpaceError(c, camera);
    // Four levels, matching the mesh's existing lod range. The threshold is the
    // error a viewer could not resolve; anything coarser is not allowed.
    const float limit = config_.lodErrorPixels;
    uint32_t lod = 0;
    if (err > limit * 1.0f) lod = 1;
    if (err > limit * 4.0f) lod = 2;
    if (err > limit * 16.0f) lod = 3;

    // Hysteresis. Without it a cluster sitting on a threshold flips every frame,
    // and each flip is a state change that costs as much as the decision it
    // caused. The band is asymmetric: stepping down (to finer LOD) needs
    // stronger evidence than stepping up, because stepping down late is a
    // visible pop while stepping up early is merely a wasted triangle.
    const uint32_t prev = c.lod;
    if (prev > lod) {
        // Hysteresis, in the only direction that does anything.
        //
        // Stepping from `prev` to `prev - 1` normally needs the error below
        // `limit * 4^(prev-1)`. To make that step *harder* -- which is the
        // entire point -- the error has to fall below a *lower* number, so the
        // factor divides. The first version multiplied, which raised the bar
        // above the plain threshold, made the condition true everywhere it
        // applied, and therefore changed no outcome at all. The test that
        // compares the two configurations reported identical transition counts,
        // which is what exposed it.
        const float riseLimit = limit * std::pow(4.0f, static_cast<float>(prev - 1)) /
                                std::max(config_.lodHysteresis, 1.0001f);
        if (err < riseLimit) lod = prev;  // hold the coarser level
    }
    return lod;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

void AdaptiveVisibility::evaluate(const CameraState &camera, const Frustum &frustum) {
    visible_.clear();
    // Metrics are reset per frame rather than accumulated across the session.
    // A cumulative counter is more flattering and less useful: a rate is what a
    // budget controller and a benchmark both need, and a rate that cannot be
    // computed from a running total is a rate nobody computes.
    AdaptiveMetrics &metrics = metrics_;
    metrics.reset();
    metrics.totalClusters = static_cast<uint32_t>(clusters_.size());
    const uint32_t frame = camera.frame;

    // A discontinuity invalidates everything, and it does so here rather than
    // relying on the caller to have called `invalidateAll`. The two paths that
    // can jump -- a teleport and a projection or origin change -- are exactly
    // the two where a forgotten invalidation shows a city from the last place
    // the player was, so the guarantee belongs inside `evaluate` where it cannot
    // be forgotten.
    if (motion_ == CameraMotion::kTeleport) {
        invalidateAll(kInvTeleport);
    } else if (motion_ == CameraMotion::kProjectionChanged) {
        invalidateAll(kInvProjection);
    } else if (motion_ == CameraMotion::kOriginShift) {
        invalidateAll(kInvOriginShift);
    }

    // The confidence decay is also motion-dependent. A stationary camera may
    // let a cluster's confidence sag, because re-confirming it will produce the
    // same answer it already has; a fast camera must not, because the answer it
    // would re-confirm may already be stale.
    float decay = config_.confidenceDecay;
    switch (motion_) {
        case CameraMotion::kStationary: decay = std::pow(config_.confidenceDecay, 0.25f); break;
        case CameraMotion::kSlow: break;
        case CameraMotion::kRotating: decay = std::pow(config_.confidenceDecay, 0.5f); break;
        case CameraMotion::kFast: decay = 1.0f; break;
        case CameraMotion::kTeleport:
        case CameraMotion::kProjectionChanged:
        case CameraMotion::kOriginShift:
            decay = 0.0f;
            break;
    }

    const float trust = effectiveTrustRadius();
    const float trust2 = trust * trust;

    // Budget is a *count* of full recomputes allowed, allocated to the clusters
    // that most need one. It is never allowed to skip a cluster that an
    // invalidation bit has marked: deferring those would show stale geometry,
    // and a frame-time target is not worth a missing building.
    uint32_t fullBudget = static_cast<uint32_t>(static_cast<float>(clusters_.size()) * config_.fullRecomputeBudget);
    if (fullBudget == 0 && !clusters_.empty()) fullBudget = 1;

    // Priority order: the loop visits high-priority clusters first so that when
    // the budget runs out it is the low-priority tail that is deferred, which
    // is where deferral is both safe and invisible.
    std::vector<uint32_t> order(clusters_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return clusters_[a].priority > clusters_[b].priority;
    });

    for (uint32_t oi = 0; oi < order.size(); ++oi) {
        const uint32_t i = order[oi];
        Cluster &c = clusters_[i];

        // -- choose the rung ------------------------------------------------
        Decision decision;
        if (c.invalidation != kInvNone) {
            decision = Decision::kFullRecompute;
        } else if (c.answerSignature == camera.signature && c.confidence >= config_.validateThreshold) {
            // Nothing about this cluster or the camera that produced its answer
            // has changed. The answer is the same answer.
            decision = Decision::kReuse;
        } else {
            decision = Decision::kValidate;
        }

        // Distance overrides everything: beyond the trust radius a cached
        // answer is not worth carrying, because the sphere test that would
        // replace it is cheap and a wrong answer at the horizon is a pop.
        float dx = c.sphere.center[0] - camera.position[0];
        float dy = c.sphere.center[1] - camera.position[1];
        float dz = c.sphere.center[2] - camera.position[2];
        const float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > trust2) decision = Decision::kFullRecompute;

        // Budget deferral: the only cluster permitted to wait is one that could
        // safely have waited, which means it is not invalidated, is not the
        // player's own position, and already has a confident answer.
        if (decision == Decision::kFullRecompute && fullBudget > 0) --fullBudget;
        else if (decision == Decision::kFullRecompute && c.invalidation == kInvNone &&
                 c.confidence > config_.validateThreshold) {
            decision = Decision::kValidate;  // defer the exact test, keep validating
            c.invalidation |= kInvBudget;    // and remember, so it is not lost
        }

        // -- run the rung ---------------------------------------------------
        bool visible;
        if (decision == Decision::kReuse) {
            visible = c.visible;
            ++metrics.reuses;
            ++metrics.cacheHits;
        } else {
            visible = !definitelyNotVisible(c, frustum, camera.position, config_.drawDistance, false);
            if (decision == Decision::kValidate) {
                ++metrics.validations;
                ++metrics.partialRecomputes;
                ++metrics.cacheMisses;
            } else {
                ++metrics.fullRecomputes;
            }
        }

        // The reject reason is recorded from the exact test so the counters say
        // *why* a cluster left, not merely that it did. Recomputing the reason
        // for a reused cluster would be a per-frame cost for a debug number, so
        // only the rungs that actually ran a test record one.
        if (decision != Decision::kReuse && !visible) {
            if (!frustum.intersects(c.sphere)) ++metrics.frustumRejects;
            else ++metrics.coneRejects;
        }

        // -- occlusion -------------------------------------------------------
        //
        // Occlusion is decided on its own rung, and deliberately not gated on
        // the spatial one. The spatial answer may legitimately be reused -- the
        // camera has not moved -- but the Hi-Z pyramid is rebuilt from the
        // previous frame's depth buffer every single frame, and the world keeps
        // moving when the player does not. A car pulling out from behind a
        // building changes what is occluded with a perfectly still camera, so
        // reusing the previous occlusion answer is a stale answer.
        //
        // The first version of this gated occlusion on `decision != kReuse`
        // and therefore never occlusion-tested a stationary scene at all. The
        // test "an occluder in front produces occlusion rejects" failed with a
        // still camera, which is what exposed it.
        c.coneTested = false;
        if (visible && occlusion_ != nullptr) {
            const float depth = occlusion_->sampleDepth(c.bounds, frustum, c.sphere);
            if (depth >= 0.0f) {
                // A depth this far in front of the cluster's nearest point is a
                // genuine occluder. Anything else leaves it visible.
                float nearest = 0.0f;
                for (int p = 0; p < Frustum::kPlaneCount; ++p) {
                    const float *pl = frustum.planes + p * 4;
                    const float d = pl[0] * c.sphere.center[0] + pl[1] * c.sphere.center[1] +
                                    pl[2] * c.sphere.center[2] + pl[3];
                    nearest = std::max(nearest, d);
                }
                if (depth < nearest - c.sphere.radius * 0.0f) {
                    visible = false;
                    c.occlusionState = static_cast<uint8_t>(OcclusionState::kOccluded);
                    ++metrics.hizRejects;
                    // One occluded frame is not a fact. Confidence drops so the
                    // next frames re-confirm before it is believed.
                    c.confidence = clampf(c.confidence * (1.0f - config_.uncertaintyPenalty), 0.0f, 1.0f);
                    c.invalidation |= kInvOcclusion;
                } else {
                    c.occlusionState = static_cast<uint8_t>(OcclusionState::kVisible);
                }
            } else {
                c.occlusionState = static_cast<uint8_t>(OcclusionState::kUncertain);
            }
        }

        // -- commit ----------------------------------------------------------
        c.previouslyVisible = c.visible;
        c.visible = visible;
        c.decision = decision;
        c.lastTestedFrame = frame;
        c.answerSignature = camera.signature;
        if (visible) c.lastVisibleFrame = frame;

        if (c.visible == c.previouslyVisible) ++c.stableFrames;
        else c.stableFrames = 0;

        // Confidence rises with stability and decays with staleness. Both terms
        // matter: stability alone would let a cluster that is confidently wrong
        // (a wall that moved into view and the transform bump was missed) stay
        // confident forever.
        c.confidence = clampf(c.confidence * decay + 0.01f, 0.0f, 1.0f);

        // LOD, and the transition counter.
        const uint32_t lod = lodFor(c, camera);
        if (lod != c.lod) {
            ++metrics.lodTransitions;
            c.lod = lod;
            ++c.lodVersion;
        }

        if (c.visible) {
            visible_.push_back(c);
            ++metrics.visibleClusters;
        } else {
            ++metrics.rejectedClusters;
        }
    }

    metrics.indirectDraws = static_cast<uint32_t>(visible_.size());
}

void AdaptiveVisibility::recomputeAll(const CameraState &camera, const Frustum &frustum,
                                      std::vector<uint8_t> &visibleOut) const {
    // The oracle. No cache, no confidence, no budget: every cluster gets the
    // exact conservative test, every frame. The adaptive path is only correct
    // if it agrees with this, which is the assertion the test suite makes.
    visibleOut.assign(clusters_.size(), 0);
    for (uint32_t i = 0; i < clusters_.size(); ++i) {
        const Cluster &c = clusters_[i];
        visibleOut[i] = definitelyNotVisible(c, frustum, camera.position, config_.drawDistance, false) ? 0u : 1u;
    }
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

void AdaptiveVisibility::invalidateCluster(uint32_t clusterId, uint32_t reason) {
    if (clusterId >= clusters_.size()) return;
    Cluster &c = clusters_[clusterId];
    c.invalidation |= reason;
    c.confidence = 0.0f;
    c.stableFrames = 0;
    c.answerSignature = 0;  // forces a different rung than REUSE
    metrics_.invalidations++;
}

void AdaptiveVisibility::invalidateCell(uint32_t cellId, uint32_t reason) {
    if (cellId >= cellBounds_.size()) return;
    for (uint32_t i = 0; i < clusters_.size(); ++i) {
        if (clusters_[i].cell != cellId) continue;
        Cluster &c = clusters_[i];
        c.invalidation |= reason;
        c.confidence = 0.0f;
        c.stableFrames = 0;
        c.answerSignature = 0;
        metrics_.invalidations++;
    }
}

void AdaptiveVisibility::invalidateSector(uint32_t sectorId, uint32_t reason) {
    if (sectorId >= sectorBounds_.size()) return;
    const uint32_t first = sectorFirstCell_[sectorId];
    const uint32_t n = sectorCellCount_[sectorId];
    for (uint32_t k = 0; k < n; ++k) invalidateCell(first + k, reason);
}

void AdaptiveVisibility::invalidateAll(uint32_t reason) {
    for (Cluster &c : clusters_) {
        c.invalidation |= reason;
        c.confidence = 0.0f;
        c.stableFrames = 0;
        c.answerSignature = 0;
    }
    metrics_.invalidations += static_cast<uint32_t>(clusters_.size());
}

void AdaptiveVisibility::setCellPriority(uint32_t cellId, RenderPriority p) {
    if (cellId >= cellBounds_.size()) return;
    for (Cluster &c : clusters_) {
        if (c.cell == cellId) c.priority = static_cast<uint8_t>(p);
    }
}

void AdaptiveVisibility::setClusterShadowCaster(uint32_t clusterId, bool casts) {
    if (clusterId >= clusters_.size()) return;
    clusters_[clusterId].castsShadow = casts;
}

std::string AdaptiveVisibility::describe() const {
    return metrics_.describe() + " | cells " + std::to_string(cellBounds_.size()) +
           " sectors " + std::to_string(sectorBounds_.size());
}

} // namespace emergent
