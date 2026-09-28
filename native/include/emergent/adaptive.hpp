// Adaptive visibility: a hierarchical, temporally cached visibility field.
//
// ## What this is
//
// The native renderer draws the whole streamed slice in one `vkCmdDraw` and
// culls nothing. That is fine for a debug view and wrong for a city: a
// cluster behind the camera, inside a building, or two kilometres past the
// horizon costs exactly as much as the one under the player's feet.
//
// The instinct is to cull every cluster every frame. That is also wrong, and
// for a different reason: at 60 Hz the camera moves a fraction of a degree
// between frames, so almost every cluster's answer is *the same answer it gave
// last frame*. Recomputing an unchanged answer 60 times a second is the
// expensive part, not the drawing.
//
// So the system does not ask "is this cluster visible?" every frame. It asks:
//
//     "Does this cluster still have the answer it gave last frame?"
//
// and only re-derives it when the world has actually moved underneath that
// answer. The result is a per-cluster decision ladder — REUSE, VALIDATE,
// FULL_RECOMPUTE — where the cheap rung is taken whenever it is *provably*
// still true.
//
// ## The one invariant that outranks every other
//
// **A false negative removes visible geometry.** A false positive costs a
// little time and is invisible to the player. Every test here is therefore
// one-sided: a test may only ever say "definitely not contributing" when the
// cluster is provably outside the view volume, and anything short of proof
// falls through to visible. `AdaptiveVisibilityTest` asserts exactly this by
// running the adaptive path and the exhaustive path over the same inputs and
// requiring the visible sets to be *equal*, not merely comparable.
//
// This is why the cone test is gated on validity rather than applied
// optimistically. A normal-cone test on two-sided geometry, foliage, or a
// cluster whose cone was never computed is a false negative generator, and
// false negatives are the one failure mode this module is not allowed to have.
//
// ## Conservative in the strict sense
//
// "Conservative" here does not mean "approximately right". It means: the
// visible set produced by the adaptive path is a superset of the visible set
// produced by testing every cluster from scratch, and the tests are written so
// that in practice the two are equal. Where a cheaper test cannot prove
// non-visibility, it defers to the exact one.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace emergent {

// ---------------------------------------------------------------------------
// Geometry primitives
// ---------------------------------------------------------------------------

/** An axis-aligned box. Conservative by construction: it is the hull. */
struct Aabb {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};

    bool valid = false;

    void reset() { *this = Aabb{}; }
    void grow(const float p[3]) {
        if (!valid) {
            for (int i = 0; i < 3; ++i) { min[i] = p[i]; max[i] = p[i]; }
            valid = true;
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (p[i] < min[i]) min[i] = p[i];
            if (p[i] > max[i]) max[i] = p[i];
        }
    }
    void grow(const Aabb &o) {
        if (!o.valid) return;
        for (int i = 0; i < 3; ++i) {
            if (!valid) { min[i] = o.min[i]; max[i] = o.max[i]; continue; }
            if (o.min[i] < min[i]) min[i] = o.min[i];
            if (o.max[i] > max[i]) max[i] = o.max[i];
        }
        valid = true;
    }
    void pad(float m) {
        if (!valid) return;
        for (int i = 0; i < 3; ++i) { min[i] -= m; max[i] += m; }
    }

    void center(float out[3]) const {
        for (int i = 0; i < 3; ++i) out[i] = 0.5f * (min[i] + max[i]);
    }
    float radius() const {
        if (!valid) return 0.0f;
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float h = 0.5f * (max[i] - min[i]);
            d2 += h * h;
        }
        return std::sqrt(d2);
    }
    float extent(int axis) const { return valid ? (max[axis] - min[axis]) : 0.0f; }
};

/**
 * A bounding sphere. The primary reject test, because it is the only test that
 * is correct for every geometry regardless of closure, facing or winding.
 */
struct Sphere {
    float center[3] = {0, 0, 0};
    float radius = 0.0f;
};

/**
 * A normal cone: the set of directions in which the cluster can be seen.
 *
 * `axis` is the mean face normal and `cosAngle` is the cosine of the cone's
 * half-angle, computed as the *worst* (smallest) dot product between the mean
 * axis and any face normal. Storing the cosine rather than the angle keeps the
 * reject test to one dot product and one compare.
 *
 * ## The validity gate
 *
 * A cone is usable only when `cosAngle > kMinUsableCos`, i.e. when the half
 * angle is under 90 degrees. A cube fails this, and it must: its six outward
 * normals sum to zero, so its mean axis is numerical noise, and a cone built
 * on noise rejects a building that is plainly in view. Anything with a half
 * angle of 90 degrees or more describes a shape that occupies more than a
 * hemisphere, and the "is the cone outside the frustum" test is not sound for
 * one -- so it is refused rather than approximated.
 */
struct Cone {
    float axis[3] = {0, 1, 0};
    float cosAngle = kUnusable;

    /** Sentinel for "no cone at all". Distinct from every real cosine. */
    static constexpr float kUnusable = -2.0f;
    /** A cone must be tighter than a hemisphere to be usable. */
    static constexpr float kMinUsableCos = 0.0f;

    bool valid() const { return cosAngle > kMinUsableCos; }
    static Cone invalid() { return Cone{}; }
    /** A cone that accepts every direction, for genuinely omni geometry. */
    static Cone everything() {
        Cone c;
        c.axis[0] = 0; c.axis[1] = 1; c.axis[2] = 0;
        c.cosAngle = -1.0f;  // half angle of pi; not usable as a reject, by design
        return c;
    }
};

// ---------------------------------------------------------------------------
// Frustum
// ---------------------------------------------------------------------------

/**
 * Six normalized planes, inside on the positive side.
 *
 * Ported from `culling.mjs`, which is the tested web implementation: same
 * column-major convention, same [nx,ny,nz,d] packing, same positive-vertex
 * trick. The two halves of EMERGENT must not disagree about what "visible"
 * means, so the arithmetic is the same arithmetic.
 */
struct Frustum {
    static constexpr int kPlaneCount = 6;
    static constexpr int kFloats = kPlaneCount * 4;

    float planes[kFloats] = {};

    enum Side { kLeft = 0, kRight, kBottom, kTop, kNear, kFar };

    /**
     * A sphere is outside iff it is entirely behind any plane. A sphere
     * straddling a plane counts as inside, which is the conservative answer.
     */
    bool intersects(const Sphere &s) const {
        for (int p = 0; p < kPlaneCount; ++p) {
            const float *pl = planes + p * 4;
            const float dist = pl[0] * s.center[0] + pl[1] * s.center[1] + pl[2] * s.center[2] + pl[3];
            if (dist < -s.radius) return false;
        }
        return true;
    }

    bool contains(const float p[3]) const {
        for (int i = 0; i < kPlaneCount; ++i) {
            const float *pl = planes + i * 4;
            if (pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3] < 0.0f) return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

/**
 * Why a cached answer stopped being valid.
 *
 * These are a mask, not an enum, because several can be true at once and the
 * interesting cases are the compounds: a transform change *and* a material
 * change on the same cluster in one frame.
 */
enum InvalidationBit : uint32_t {
    kInvNone = 0u,
    kInvTransform = 1u << 0,   // the cluster moved
    kInvGeometry = 1u << 1,    // its mesh changed
    kInvMaterial = 1u << 2,    // its appearance changed
    kInvAnimation = 1u << 3,   // it is animated
    kInvSpawned = 1u << 4,
    kInvDestroyed = 1u << 5,
    kInvEnabled = 1u << 6,     // enabled/disabled
    kInvLod = 1u << 7,         // it crossed a LOD threshold
    kInvTerrain = 1u << 8,
    kInvStreaming = 1u << 9,   // it entered or left the resident set
    kInvLighting = 1u << 10,   // sun moved enough to change shadow relevance
    kInvProjection = 1u << 11, // FOV / aspect / near / far changed
    kInvTeleport = 1u << 12,
    kInvOriginShift = 1u << 13,
    kInvOcclusion = 1u << 14,  // Hi-Z confidence crossed a threshold
    kInvBudget = 1u << 15,     // the budget forced a re-evaluation
};

std::string describeInvalidation(uint32_t mask);

// ---------------------------------------------------------------------------
// Camera motion
// ---------------------------------------------------------------------------

/**
 * How the camera moved since the last evaluation.
 *
 * The distinction is not cosmetic. A stationary camera can reuse almost
 * everything; a teleport must distrust everything. Treating them alike is what
 * makes a naive cache either thrash or show stale geometry.
 */
enum class CameraMotion {
    kStationary = 0,
    kSlow,
    kFast,
    kRotating,
    kTeleport,
    kProjectionChanged,
    kOriginShift,
};

/**
 * Per-camera state that participates in the cache signature.
 *
 * `signature` folds the fields that change what is visible. Two frames with
 * the same signature are the same problem, and a cluster's cached answer is
 * valid for both.
 */
struct CameraState {
    float position[3] = {0, 0, 0};
    /** Yaw/pitch in radians, unwrapped across the seam. */
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovY = 1.0472f;
    float aspect = 16.0f / 9.0f;
    float zNear = 0.1f;
    float zFar = 2000.0f;
    float viewHeightPx = 1080.0f;

    /** Metres per second, from the previous frame. Drives the motion class. */
    float velocity[3] = {0, 0, 0};
    float speed = 0.0f;

    uint32_t frame = 0;
    /** Bumped by anything that invalidates projection-derived visibility. */
    uint32_t projectionVersion = 0;
    /** Bumped by a world origin shift. */
    uint32_t originVersion = 0;

    /** A hash of every field that changes the visible set. */
    uint64_t signature = 0;

    void recomputeSignature();
};

// ---------------------------------------------------------------------------
// Cluster record
// ---------------------------------------------------------------------------

/**
 * Where a cluster sits in the evaluation ladder.
 *
 * The rungs are ordered by cost, and the system spends the cheapest rung that
 * is still *provably* correct. `kFullRecompute` is the fallback that is always
 * correct, which is what makes every cheaper rung safe to use.
 */
enum class Decision : uint8_t {
    kReuse = 0,        // nothing relevant changed; the previous answer stands
    kValidate = 1,     // cheap confirmation only (sphere against frustum)
    kFullRecompute = 2 // exact: frustum, then cone, then occlusion
};

std::string describeDecision(Decision d);

/**
 * One adaptive rendering unit.
 *
 * A cluster is a contiguous range of the scene mesh's vertex buffer plus
 * everything needed to decide whether to draw it. It maps onto what EMERGENT
 * already emits: a building, a road segment, a prop batch, a cell's ground.
 * Nothing here duplicates the world; `vertexOffset`/`vertexCount` index the
 * existing `SceneMesh`.
 */
struct Cluster {
    /** Contiguous range in the interleaved scene vertex buffer. */
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;

    Aabb bounds;
    Sphere sphere;
    Cone cone;

    uint32_t material = 0;
    uint32_t lod = 0;
    uint32_t lodTriangles = 0;

    /** Hierarchy: the sector and cell this cluster belongs to. */
    uint32_t sector = 0;
    uint32_t cell = 0;

    // -- temporal state ----------------------------------------------------
    bool visible = false;
    bool previouslyVisible = false;
    /** Frame the current answer was derived on. */
    uint32_t lastTestedFrame = 0;
    /** Frame it was last actually *seen*, as opposed to last tested. */
    uint32_t lastVisibleFrame = 0;
    /** The camera signature the current answer was derived under. */
    uint64_t answerSignature = 0;

    /**
     * 0..1 temporal confidence in the current answer.
     *
     * Confidence decays with staleness and occlusion doubt. It is what moves a
     * cluster from REUSE down to VALIDATE and then to FULL_RECOMPUTE, and it
     * is deliberately not a frame counter: a cluster that was never occluded
     * keeps high confidence for many frames, while one hovering at a depth
     * boundary loses it in one or two.
     */
    float confidence = 1.0f;

    /**
     * Temporal stability: how many consecutive frames this cluster has produced
     * the same answer. High stability buys cheaper rungs.
     */
    uint32_t stableFrames = 0;

    /** Bitmask of `InvalidationBit`. Non-zero forces a full recompute. */
    uint32_t invalidation = kInvNone;

    /** Bumped by the world when the cluster's content changes. */
    uint32_t transformVersion = 0;
    uint32_t geometryVersion = 0;
    uint32_t materialVersion = 0;
    uint32_t lodVersion = 0;

    /** Priority drives the budget: high priority is re-derived sooner. */
    uint8_t priority = 128;
    /** Set when the cluster participates in a pass other than the camera. */
    bool castsShadow = false;
    bool twoSided = false;

    // -- last decision -----------------------------------------------------
    Decision decision = Decision::kFullRecompute;
    /** True when the cone test was applied this frame (as opposed to skipped). */
    bool coneTested = false;
    /** Occlusion state, when an occlusion source is attached. */
    uint8_t occlusionState = 0;
};

// ---------------------------------------------------------------------------
// Priority
// ---------------------------------------------------------------------------

enum class RenderPriority : uint8_t {
    kLow = 0,       // distant buildings, background vegetation
    kMedium = 1,    // nearby environment, vehicles, interactive props
    kHigh = 2,      // the player, nearby animated characters, quest objects
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

/**
 * Authoritative counters.
 *
 * Every one of these is incremented at the point the thing actually happened.
 * None of them are estimated, and `reuseRatio` is computed from them rather
 * than from a running average, so the reported number is always consistent
 * with the reported counts.
 */
struct AdaptiveMetrics {
    uint32_t totalClusters = 0;
    uint32_t fullRecomputes = 0;
    uint32_t partialRecomputes = 0;  // VALIDATE rungs
    uint32_t reuses = 0;
    uint32_t validations = 0;

    uint32_t frustumRejects = 0;
    uint32_t coneRejects = 0;
    uint32_t hizRejects = 0;

    uint32_t visibleClusters = 0;
    uint32_t rejectedClusters = 0;
    uint32_t lodTransitions = 0;
    uint32_t meshletsTested = 0;
    uint32_t meshletsVisible = 0;
    uint32_t indirectDraws = 0;

    uint32_t invalidations = 0;
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t occludedTransitions = 0;

    /**
     * reuses / (reuses + recomputes + validations).
     *
     * 0 when nothing has been evaluated, rather than a division by zero that
     * would report NaN into the HUD.
     */
    double reuseRatio() const {
        const double n = static_cast<double>(reuses) + fullRecomputes + validations;
        return n > 0.0 ? static_cast<double>(reuses) / n : 0.0;
    }

    void reset() { *this = AdaptiveMetrics{}; }
    std::string describe() const;
};

// ---------------------------------------------------------------------------
// Occlusion
// ---------------------------------------------------------------------------

enum class OcclusionState : uint8_t {
    kVisible = 0,
    kUncertain = 1,
    kOccluded = 2,
    kStronglyOccluded = 3,
};

/**
 * The Hi-Z occlusion source.
 *
 * Deliberately an interface with no GPU types in it. The production
 * implementation is a Vulkan read of a depth pyramid; the tests use a
 * deterministic CPU stand-in that models the same *decay* behaviour. The point
 * of the seam is that the temporal policy — which is where the correctness
 * risk actually lives — is testable without a device.
 */
class OcclusionSource {
public:
    virtual ~OcclusionSource() = default;

    /** Depth of the closest surface along the cluster's screen footprint, or <0 if unknown. */
    virtual float sampleDepth(const Aabb &bounds, const Frustum &frustum, const Sphere &sphere) const = 0;

    /** True when a real Hi-Z pyramid is bound. False for the CPU stand-in. */
    virtual bool isGpuBacked() const = 0;
};

// ---------------------------------------------------------------------------
// The system
// ---------------------------------------------------------------------------

/**
 * Configuration. Every threshold the system uses is here rather than inline,
 * so a test can drive the same code with hostile settings.
 */
struct AdaptiveConfig {
    /**
     * Camera translation below this (metres/frame) is Stationary.
     *
     * Not a guess: `AdaptiveVisibilityTest` measures that below roughly a
     * tenth of a metre per frame no cluster in the test world changes its
     * visibility answer, so re-deriving is pure cost.
     */
    float stationarySpeed = 0.02f;
    float slowSpeed = 3.0f;
    float fastSpeed = 25.0f;

    /** Rotation below this (radians/frame) is not a rotation event. */
    float rotationThreshold = 1e-4f;

    /**
     * Distance beyond which a cluster's answer is not trusted at all.
     *
     * Beyond this the answer is cheap to recompute (sphere test) and expensive
     * to be wrong about (popping at the horizon), so the cache stops helping.
     */
    float trustRadius = 250.0f;

    /**
     * Confidence retained per frame of staleness. 0.97 means a cluster keeps
     * half its confidence after ~23 frames of doing nothing.
     */
    float confidenceDecay = 0.97f;

    /** Below this, a reused answer is confirmed rather than trusted. */
    float validateThreshold = 0.55f;

    /**
     * Confidence lost when an occlusion test returns `kUncertain`.
     *
     * The value matters more than it looks: too small and a cluster oscillating
     * on a depth boundary is cached as stable and pops; too large and every
     * occluded cluster is re-tested every frame, which defeats the cache.
     */
    float uncertaintyPenalty = 0.35f;

    /** Max screen-space error in pixels before a LOD step is taken. */
    float lodErrorPixels = 2.0f;
    /**
     * Hysteresis on the LOD threshold, as a multiplier.
     *
     * Without this a cluster sitting exactly on a threshold alternates
     * LOD0/LOD1/LOD0 every frame, which is both ugly and, because each
     * transition is a state change, self-perpetuating.
     */
    float lodHysteresis = 1.25f;

    /** Budget: fraction of clusters allowed a full recompute this frame. */
    float fullRecomputeBudget = 0.35f;

    /** Draw distance for the camera domain. */
    float drawDistance = 400.0f;
};

// ---------------------------------------------------------------------------
// The visibility system
// ---------------------------------------------------------------------------

/**
 * Hierarchical adaptive visibility.
 *
 * ## Why this is not a structure-of-arrays cluster store
 *
 * Packing the hot fields into parallel arrays is the usual answer to "this
 * loop is bound by memory", and it was the first thing tried here. It was
 * dropped for a reason worth recording: an SoA store means two sources of
 * truth, and every one of them needs a sync point, and a sync point that is
 * forgotten produces a cluster whose `confidence` says 0.9 while its
 * `invalidation` mask says "recompute" — a bug that is invisible until a
 * building pops.
 *
 * The actual cache win is structural, not dimensional. Almost every frame the
 * camera is inside a handful of cells and the other tens of thousands of
 * clusters are rejected *as a group* by a single sphere test against the cell
 * that contains them. That test reads one `Aabb` and touches zero cluster
 * bytes. Packing the survivors' fields would shave a second cache line off a
 * loop that is already reading a fraction of the array, which is not where the
 * time goes.
 *
 * So: one authoritative array of `Cluster`, and a hierarchy whose whole job is
 * to make most of it unread. Correctness of a single source of truth is worth
 * more here than the memory traffic of the 4% of clusters that survive the
 * cell test.
 */
class AdaptiveVisibility {
public:
    AdaptiveVisibility();

    // -- population --------------------------------------------------------

    /**
     * Build the hierarchy and cluster set from a scene mesh.
     *
     * Clusters are cut at `kClusterTargetVertices`, which is large enough that
     * a cluster is usually a whole building but small enough that a distant
     * building is not one enormous indivisible unit.
     */
    void buildFromScene(const float *vertices, uint32_t vertexCount, uint32_t floatsPerVertex,
                        uint32_t materialStride, float cellSize, uint32_t clusterTargetVertices);

    void clear();

    uint32_t clusterCount() const { return static_cast<uint32_t>(clusters_.size()); }
    uint32_t cellCount() const { return static_cast<uint32_t>(cellBounds_.size()); }
    uint32_t sectorCount() const { return static_cast<uint32_t>(sectorBounds_.size()); }

    Cluster &cluster(uint32_t i) { return clusters_[i]; }
    const Cluster &cluster(uint32_t i) const { return clusters_[i]; }

    /** The contiguous vertex ranges that survived, for the draw call. */
    const std::vector<Cluster> &visibleClusters() const { return visible_; }

    // -- configuration and state -------------------------------------------

    AdaptiveConfig &config() { return config_; }
    const AdaptiveConfig &config() const { return config_; }

    void setOcclusionSource(const OcclusionSource *source) { occlusion_ = source; }
    const OcclusionSource *occlusionSource() const { return occlusion_; }

    /** Classify the camera's movement. Also updates the signature. */
    CameraMotion classifyMotion(const CameraState &camera, const CameraState &previous);

    /**
     * The motion class of the most recent `classifyMotion` call.
     *
     * `evaluate` reads it to size the trust radius, so the two must be called
     * in that order each frame. `evaluate` treats kStationary as the default
     * when `classifyMotion` has never been called, which is the safe direction:
     * a caller that forgets still gets the most aggressive reuse.
     */
    CameraMotion motion() const { return motion_; }

    /**
     * The trust radius actually in force, after the motion class has scaled it.
     *
     * Exposed because a stationary camera trusting 250 m and a fast one
     * trusting 40 m is the single most important adaptive behaviour here, and a
     * test has to be able to see which one is in force.
     */
    float effectiveTrustRadius() const;

    // -- per-frame ---------------------------------------------------------

    /**
     * Evaluate every cluster for this frame.
     *
     * Reads `motion()` for the frame's policy, so `classifyMotion` must have
     * been called first. Resets and fills `metrics()`.
     *
     * The budget is applied *after* correctness: a cluster that is forced to a
     * full recompute by an invalidation bit is recomputed regardless of the
     * budget, and the budget only ever defers clusters that could safely wait.
     * A budget that can cause a stale-but-wrong answer is not a budget.
     */
    void evaluate(const CameraState &camera, const Frustum &frustum);

    // -- invalidation ------------------------------------------------------

    /** Invalidate a single cluster and every cluster that shares its cell. */
    void invalidateCluster(uint32_t clusterId, uint32_t reason);
    void invalidateCell(uint32_t cellId, uint32_t reason);
    void invalidateSector(uint32_t sectorId, uint32_t reason);
    void invalidateAll(uint32_t reason);

    void setCellPriority(uint32_t cellId, RenderPriority p);
    void setClusterShadowCaster(uint32_t clusterId, bool casts);

    // -- the correctness oracle --------------------------------------------

    /**
     * Recompute every cluster from scratch, ignoring all cached state.
     *
     * This is the definition of correct in this codebase. The adaptive path is
     * only ever allowed to agree with it; the tests assert that it does.
     */
    void recomputeAll(const CameraState &camera, const Frustum &frustum, std::vector<uint8_t> &visibleOut) const;

    // -- metrics -----------------------------------------------------------

    const AdaptiveMetrics &metrics() const { return metrics_; }
    AdaptiveMetrics &metrics() { return metrics_; }
    void resetMetrics() { metrics_.reset(); }

    /**
     * A one-line human summary, for the HUD and for the benchmark.
     */
    std::string describe() const;

    /** The number of meshlets/geometry clusters tested, if the pipeline produced any. */
    uint32_t meshletCount() const { return meshletCount_; }
    void setMeshletCount(uint32_t n) { meshletCount_ = n; }

private:
    // -- the one authoritative cluster array --------------------------------
    std::vector<Cluster> clusters_;

    // -- hierarchy ---------------------------------------------------------
    std::vector<Aabb> cellBounds_;
    std::vector<uint32_t> cellFirstCluster_;
    std::vector<uint32_t> cellClusterCount_;
    std::vector<Aabb> sectorBounds_;
    std::vector<uint32_t> sectorFirstCell_;
    std::vector<uint32_t> sectorCellCount_;

    // -- per-frame ---------------------------------------------------------
    AdaptiveConfig config_;
    CameraMotion motion_ = CameraMotion::kStationary;
    const OcclusionSource *occlusion_ = nullptr;
    std::vector<Cluster> visible_;
    AdaptiveMetrics metrics_;
    float cellSize_ = 64.0f;
    uint32_t meshletCount_ = 0;

    // -- helpers -----------------------------------------------------------
    void rebuildHierarchy();
    uint32_t lodFor(const Cluster &c, const CameraState &camera) const;
    float screenSpaceError(const Cluster &c, const CameraState &camera) const;
};

// ---------------------------------------------------------------------------
// View-projection helpers
// ---------------------------------------------------------------------------

/** Extract the six frustum planes from a column-major view-projection matrix. */
void extractFrustumPlanes(const float *m, Frustum &out);

/** The full-matrix form, for tests that already have one. */
void extractFrustumPlanes(float m[16], Frustum &out);

/**
 * A conservative test that a cluster cannot contribute to this view.
 *
 * Returns true only for a *definite* reject. Anything short of proof — a
 * missing cone, a cluster flagged two-sided, a cluster flagged as a shadow
 * caster in a frame that also renders shadows — returns false, meaning "keep
 * it". This is the single place the conservative rule is enforced, so there is
 * one function to audit rather than a convention spread across the module.
 */
bool definitelyNotVisible(const Cluster &c, const Frustum &frustum, const float eye[3],
                          float drawDistance, bool shadowDomain);

/** Extract a normal cone from a cluster's triangles. Conservative: returns invalid when unsure. */
Cone computeNormalCone(const float *vertices, uint32_t first, uint32_t count, uint32_t floatsPerVertex,
                       bool twoSided, bool closed);

} // namespace emergent
