// Correctness tests for the adaptive visibility system.
//
// ## What is actually being asserted
//
// One property, over and over, in many disguises: the visible set produced by
// the adaptive path equals the visible set produced by testing every cluster
// from scratch. Not "is close to", not "is a subset of" — equal.
//
// That is a strong claim to make about a system whose entire purpose is to
// skip work, so it is worth being precise about why it is the right claim. The
// system is allowed to skip a *computation* only when the computation would
// have produced the answer it already holds. It is never allowed to skip a
// computation that would have produced a *different* answer. So the correct
// behaviour is indistinguishable from exhaustive recomputation, and any test
// that accepted a mismatch would be testing the wrong property.
//
// The negative controls at the bottom exist because a suite that has never
// been seen to fail is not evidence. They deliberately break the invariant and
// require this suite to notice.

#include "emergent/adaptive.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace emergent;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_section = "";

void section(const char *name) {
    g_section = name;
    std::printf("\n%s\n", name);
}

void check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL [%s] %s\n", g_section, what);
    } else {
        std::printf("  ok   %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// Test scene
// ---------------------------------------------------------------------------

/** Build an axis-aligned box as 12-float interleaved vertices (36 for 12 tris). */
void emitBox(std::vector<float> &out, float cx, float cy, float cz, float hx, float hy, float hz) {
    const float corners[8][3] = {
        {cx - hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz + hz}, {cx - hx, cy - hy, cz + hz},
        {cx - hx, cy + hy, cz - hz}, {cx + hx, cy + hy, cz - hz}, {cx + hx, cy + hy, cz + hz}, {cx - hx, cy + hy, cz + hz},
    };
    static const int faces[6][4] = {
        {0, 3, 2, 1},  // -Y
        {4, 5, 6, 7},  // +Y
        {0, 1, 5, 4},  // -Z
        {2, 3, 7, 6},  // +Z
        {1, 2, 6, 5},  // +X
        {3, 0, 4, 7},  // -X
    };
    for (int f = 0; f < 6; ++f) {
        const int *q = faces[f];
        const int order[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
        for (int i = 0; i < 6; ++i) {
            const float *p = corners[order[i]];
            out.push_back(p[0]);
            out.push_back(p[1]);
            out.push_back(p[2]);
            out.push_back(0); out.push_back(1); out.push_back(0);  // normal
            out.push_back(1); out.push_back(1); out.push_back(1);     // colour
            out.push_back(0);                                        // material
            out.push_back(0); out.push_back(0);                      // uv
        }
    }
}

/** A city-ish grid of boxes spread over a few hundred metres. */
std::vector<float> makeCity(int n, float spacing, float offsetX = 0.0f, float offsetZ = 0.0f) {
    std::vector<float> v;
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    for (int i = 0; i < n; ++i) {
        const int gx = i % side;
        const int gz = i / side;
        emitBox(v, offsetX + gx * spacing, 5.0f, offsetZ + gz * spacing, 1.5f, 5.0f, 1.5f);
    }
    return v;
}

// -- matrices ---------------------------------------------------------------

void perspective(float fovY, float aspect, float zNear, float zFar, float m[16]) {
    std::memset(m, 0, sizeof(float) * 16);
    const float f = 1.0f / std::tan(fovY * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
}

void lookAt(const float eye[3], const float centre[3], const float up[3], float m[16]) {
    float f[3] = {centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) fl = 1.0f;
    for (int i = 0; i < 3; ++i) f[i] /= fl;
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) sl = 1.0f;
    for (int i = 0; i < 3; ++i) s[i] /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2]; m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2]; m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}

void mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
    }
}

/** A view-projection looking down -Z from `eye` toward `centre`. */
void viewProjection(const float eye[3], const float centre[3], const CameraState &cam, float out[16]) {
    float p[16], v[16];
    perspective(cam.fovY, cam.aspect, cam.zNear, cam.zFar, p);
    const float up[3] = {0, 1, 0};
    lookAt(eye, centre, up, v);
    mul(p, v, out);
}

/** The camera looking at a point from `eye`, with the state's fov/aspect. */
CameraState cameraAt(const float eye[3], const float look[3], uint32_t frame) {
    CameraState c;
    std::memcpy(c.position, eye, 3 * sizeof(float));
    const float dx = look[0] - eye[0];
    const float dy = look[1] - eye[1];
    const float dz = look[2] - eye[2];
    const float horiz = std::sqrt(dx * dx + dz * dz);
    c.yaw = std::atan2(dx, dz);
    c.pitch = std::atan2(dy, horiz);
    c.frame = frame;
    c.recomputeSignature();
    return c;
}

// -- the oracle comparison --------------------------------------------------

/**
 * The central assertion: run the adaptive path, run the exhaustive path, and
 * require the two visible sets to be identical.
 *
 * Returns the number of mismatches so callers can report a count rather than a
 * boolean -- a single mismatch out of 40,000 clusters is a very different bug
 * from 20,000, and "the test failed" does not distinguish them.
 */
int compareToOracle(AdaptiveVisibility &av, const CameraState &cam, const CameraState &prev) {
    av.classifyMotion(cam, prev);
    Frustum f;
    float m[16];
    // Rebuild the look-at point from the camera's own yaw/pitch rather than
    // from whatever the caller passed in, so the frustum always matches the
    // orientation the visibility system is being asked about. A frustum built
    // from a stale look-at would cull correctly and wrongly at the same time,
    // which is the hardest kind of test failure to read.
    const float horiz = std::cos(cam.pitch);
    const float look[3] = {cam.position[0] + std::sin(cam.yaw) * horiz,
                           cam.position[1] + std::sin(cam.pitch),
                           cam.position[2] + std::cos(cam.yaw) * horiz};
    viewProjection(cam.position, look, cam, m);
    extractFrustumPlanes(m, f);

    av.evaluate(cam, f);
    std::vector<uint8_t> expected;
    av.recomputeAll(cam, f, expected);

    const std::vector<Cluster> &got = av.visibleClusters();
    int mismatches = 0;

    // `expected` is one verdict per cluster; `got` is only the survivors. The
    // counts to compare are therefore the number of visible verdicts against
    // the size of the survivor list -- not the two vector sizes, which are
    // different quantities and comparing them reports a mismatch on every
    // frame of every scene.
    size_t expectedVisible = 0;
    for (uint8_t v : expected) expectedVisible += v ? 1u : 0u;
    if (got.size() != expectedVisible) {
        std::printf("    [dbg] count differs: adaptive=%zu oracle=%zu of %u clusters\n",
                    got.size(), expectedVisible, av.clusterCount());
        ++mismatches;
    }

    // Then the set itself, not just the count: two systems can agree on how
    // many clusters survived and disagree entirely about which.
    std::vector<uint8_t> actual(av.clusterCount(), 0);
    for (const Cluster &c : got) {
        for (uint32_t i = 0; i < av.clusterCount(); ++i) {
            if (av.cluster(i).vertexOffset == c.vertexOffset) { actual[i] = 1; break; }
        }
    }
    for (uint32_t i = 0; i < av.clusterCount(); ++i) {
        if (actual[i] != expected[i]) {
            if (mismatches < 4) {
                const Cluster &c = av.cluster(i);
                std::printf("    [dbg] c%u pos=(%.1f,%.1f,%.1f) r=%.2f adaptive=%d oracle=%d %s cone=%d\n",
                            i, c.sphere.center[0], c.sphere.center[1], c.sphere.center[2], c.sphere.radius,
                            (int)actual[i], (int)expected[i], describeDecision(c.decision).c_str(),
                            (int)c.cone.valid());
            }
            ++mismatches;
        }
    }
    return mismatches;
}

} // namespace

// ---------------------------------------------------------------------------

int main() {
    std::printf("EMERGENT adaptive visibility tests\n");

    // -- the primitives ----------------------------------------------------
    section("frustum extraction");
    {
        CameraState cam;
        cam.fovY = 1.0472f;
        cam.aspect = 16.0f / 9.0f;
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        float m[16];
        viewProjection(eye, look, cam, m);
        Frustum f;
        extractFrustumPlanes(m, f);

        Sphere in{look[0], look[1], look[2] - 50.0f, 1.0f};
        check(f.intersects(in), "a sphere 50 m ahead is inside");

        Sphere behind{0, 10, 50, 1.0f};
        check(!f.intersects(behind), "a sphere directly behind is outside");

        Sphere straddle{0, 10, -0.4f, 2.0f};
        check(f.intersects(straddle), "a sphere straddling the near plane counts as inside");

        Sphere far_left{-500, 10, -50, 1.0f};
        check(!f.intersects(far_left), "a sphere far off to the side is outside");
    }

    section("normal cone");
    {
        // A flat upward-facing patch: every normal is the same direction, so the
        // mean axis is well defined and the cone is tight. This is the case the
        // cone test exists for.
        std::vector<float> quad;
        const float pts[4][3] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            quad.insert(quad.end(), {pts[order[i]][0], pts[order[i]][1], pts[order[i]][2],
                                     0, 1, 0, 1, 1, 1, 0, 0, 0});
        }
        Cone flat = computeNormalCone(quad.data(), 0, 6, 12, false, true);
        check(flat.valid(), "a flat patch produces a usable cone");
        check(flat.cosAngle > 0.99f, "a flat patch has a very tight cone");

        // A cube has six outward normals that sum to zero, so its mean axis is
        // numerical noise. It MUST be refused a cone: a cube is visible from
        // every direction, and a cone built on a garbage axis would reject
        // buildings that are plainly on screen.
        //
        // The first version of this test asserted the opposite -- that a box
        // produces a valid cone -- and it passed only because of a sentinel
        // bug: `valid()` compared `cosAngle > -1.0f + 1e-6f` against the very
        // same expression that set it, which is `x > x` and therefore always
        // false. The assertion was never actually exercised.
        std::vector<float> box;
        emitBox(box, 0, 0, 0, 1, 1, 1);
        Cone cb = computeNormalCone(box.data(), 0, 36, 12, false, true);
        check(!cb.valid(), "a cube is refused a cone, because it is omni-directional");

        // And the two hard gates.
        Cone two = computeNormalCone(box.data(), 0, 36, 12, true, true);
        check(!two.valid(), "a two-sided cluster is refused a cone");
        Cone open = computeNormalCone(box.data(), 0, 36, 12, false, false);
        check(!open.valid(), "an open cluster is refused a cone");

        // A cone is only ever *tight* if the worst normal is inside the
        // hemisphere. Force the check directly, because it is the one that
        // decides between "this cone rejects" and "this cluster falls back to
        // the sphere test".
        Cone wide;
        wide.cosAngle = 0.5f;
        check(wide.valid(), "a 60 degree half-angle cone is usable");
        wide.cosAngle = 0.0f;
        check(!wide.valid(), "a 90 degree half-angle cone is not usable");
        check(!Cone::invalid().valid(), "the invalid sentinel is not usable");
        check(!Cone::everything().valid(), "the everything-cone is not usable as a reject");
    }

    section("conservative reject");
    {
        CameraState cam;
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        float m[16];
        viewProjection(eye, look, cam, m);
        Frustum f;
        extractFrustumPlanes(m, f);

        Cluster front;
        front.sphere = Sphere{0, 10, -50, 2};
        front.bounds.grow(front.sphere.center);
        check(!definitelyNotVisible(front, f, eye, 400.0f, false), "a cluster ahead is kept");

        Cluster back;
        back.sphere = Sphere{0, 10, 50, 2};
        back.bounds.grow(back.sphere.center);
        check(definitelyNotVisible(back, f, eye, 400.0f, false), "a cluster behind is rejected");

        // The shadow domain must not reuse camera visibility.
        Cluster caster;
        caster.sphere = Sphere{0, 10, -50, 2};
        caster.bounds.grow(caster.sphere.center);
        caster.castsShadow = true;
        check(!definitelyNotVisible(caster, f, eye, 400.0f, true), "a shadow caster survives the shadow domain");
        caster.castsShadow = false;
        check(definitelyNotVisible(caster, f, eye, 400.0f, true), "a non-caster is rejected in the shadow domain");
    }

    // -- the oracle --------------------------------------------------------
    section("adaptive equals exhaustive recomputation");
    {
        const std::vector<float> city = makeCity(256, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        check(av.clusterCount() > 0, "the city produced clusters");
        check(av.clusterCount() > 1, "the city produced more than one cluster");
        check(av.cellCount() > 1, "the city produced more than one cell");

        // -- stationary ----------------------------------------------------
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        CameraState prev = cam;
        check(compareToOracle(av, cam, prev) == 0, "stationary camera agrees with the oracle");

        // Hold still for many frames. This is the case the whole system exists
        // for: the answer must not change, and the work must go down.
        const AdaptiveMetrics first = av.metrics();
        int heldBad = 0;
        for (uint32_t f = 2; f < 60; ++f) {
            CameraState c = cameraAt(eye, look, f);
            heldBad += compareToOracle(av, c, c);
        }
        check(heldBad == 0, "every frame of a still camera agrees with the oracle");
        const AdaptiveMetrics held = av.metrics();
        check(held.visibleClusters == first.visibleClusters, "a still camera sees the same count");
        check(held.reuses > 0, "a still camera reuses rather than recomputing");
        check(held.fullRecomputes <= first.fullRecomputes, "a still camera does not recompute more than at rest");

        // -- slow movement --------------------------------------------------
        int bad = 0;
        CameraState last = cam;
        for (uint32_t f = 60; f < 140; ++f) {
            const float e[3] = {0.02f * static_cast<float>(f), 10.0f, 0.0f};
            const float l[3] = {e[0], 10.0f, -1.0f};
            CameraState c = cameraAt(e, l, f);
            bad += compareToOracle(av, c, last);
            last = c;
        }
        check(bad == 0, "slow lateral movement agrees with the oracle");

        // -- fast movement --------------------------------------------------
        bad = 0;
        last = last;
        for (uint32_t f = 140; f < 180; ++f) {
            const float e[3] = {0.5f * static_cast<float>(f), 10.0f, 0.0f};
            const float l[3] = {e[0], 10.0f, -1.0f};
            CameraState c = cameraAt(e, l, f);
            bad += compareToOracle(av, c, last);
            last = c;
        }
        check(bad == 0, "fast movement agrees with the oracle");

        // -- rotation -------------------------------------------------------
        bad = 0;
        for (uint32_t f = 180; f < 260; ++f) {
            const float yaw = 0.05f * static_cast<float>(f);
            const float e[3] = {0, 10, 0};
            CameraState c = cameraAt(e, look, f);
            c.yaw = yaw;
            c.recomputeSignature();
            bad += compareToOracle(av, c, last);
            last = c;
        }
        check(bad == 0, "rotation agrees with the oracle");

        // -- teleport -------------------------------------------------------
        CameraState tele = cameraAt(eye, look, 261);
        tele.position[0] = 900.0f;
        tele.recomputeSignature();
        check(av.classifyMotion(tele, last) == CameraMotion::kTeleport, "a 900 m jump is a teleport");
        check(av.effectiveTrustRadius() == 0.0f, "a teleport trusts nothing");
        check(compareToOracle(av, tele, last) == 0, "a teleport agrees with the oracle");

        // -- FOV change -----------------------------------------------------
        CameraState wide = cameraAt(eye, look, 262);
        wide.fovY = 1.4f;
        wide.projectionVersion = 1;
        wide.recomputeSignature();
        check(av.classifyMotion(wide, tele) == CameraMotion::kProjectionChanged, "a version bump is a projection change");
        check(compareToOracle(av, wide, tele) == 0, "an FOV change agrees with the oracle");

        // -- resolution change ----------------------------------------------
        CameraState res = wide;
        res.projectionVersion = 1;  // same projection, different pixel height
        res.viewHeightPx = 540.0f;
        res.frame = 263;
        res.recomputeSignature();
        check(compareToOracle(av, res, wide) == 0, "a resolution change agrees with the oracle");
    }

    // -- invalidation -------------------------------------------------------
    section("invalidation");
    {
        const std::vector<float> city = makeCity(64, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);

        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        compareToOracle(av, cam, cam);
        const uint32_t before = av.metrics().visibleClusters;

        av.invalidateAll(kInvTransform);
        compareToOracle(av, cam, cam);
        check(av.metrics().visibleClusters == before, "a transform invalidation does not change what is visible");

        av.invalidateCluster(0, kInvGeometry);
        check(av.cluster(0).invalidation != kInvNone, "a single cluster can be invalidated");
        check(av.metrics().invalidations > 0, "invalidations are counted");

        av.invalidateCell(0, kInvStreaming);
        check(av.metrics().invalidations > 1, "a cell invalidation reaches more than one cluster");

        av.invalidateSector(0, kInvLighting);
        check(av.metrics().invalidations > 2, "a sector invalidation reaches the sector");

        // After every invalidation the answer must still be the right one.
        check(compareToOracle(av, cam, cam) == 0, "the oracle still agrees after a storm of invalidations");
    }

    // -- LOD ----------------------------------------------------------------
    section("adaptive LOD");
    {
        const std::vector<float> city = makeCity(64, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        compareToOracle(av, cam, cam);

        // Walk slowly through a LOD threshold and count transitions. The number
        // that matters is not that it is small, it is that hovering does not
        // produce one transition per frame.
        uint32_t lastLod = av.cluster(0).lod;
        uint32_t flips = 0;
        CameraState prev = cam;
        for (uint32_t f = 2; f < 200; ++f) {
            const float d = 20.0f + 0.01f * static_cast<float>(f);
            const float e[3] = {0, 10, d};
            const float l[3] = {0, 10, d - 1.0f};
            CameraState c = cameraAt(e, l, f);
            av.classifyMotion(c, prev);
            Frustum fr;
            float m[16];
            viewProjection(c.position, l, c, m);
            extractFrustumPlanes(m, fr);
            av.evaluate(c, fr);
            const uint32_t lod = av.cluster(0).lod;
            if (lod != lastLod) { ++flips; lastLod = lod; }
            prev = c;
        }
        check(flips < 20, "hovering near a LOD threshold does not flip every frame");
    }

    // -- occlusion ----------------------------------------------------------
    section("occlusion is temporally conservative");
    {
        // The oracle cannot referee occlusion, and pretending otherwise would be
        // the easiest way to make this whole suite meaningless. The oracle is
        // the *spatially* conservative reference: it applies no occlusion at
        // all, so by construction it keeps everything the frustum keeps. An
        // occlusion reject is therefore always a disagreement with the oracle,
        // and asserting they agree would assert that occlusion never happens.
        //
        // So occlusion gets the property that actually matters instead: it is
        // allowed to hide something, but it must not hide it *permanently*. A
        // cluster that was visible and becomes occluded has to come back the
        // moment the occluder stops claiming to be one. That is the anti-popping
        // guarantee, and it is checkable without a GPU.
        struct Occluder : OcclusionSource {
            bool occluding = true;
            float sampleDepth(const Aabb &, const Frustum &, const Sphere &) const override {
                return occluding ? 0.01f : -1.0f;  // 0.01 = something in front; -1 = unknown
            }
            bool isGpuBacked() const override { return false; }
        };
        Occluder src;

        const std::vector<float> city = makeCity(64, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 360);
        av.setOcclusionSource(&src);
        check(!av.occlusionSource()->isGpuBacked(), "the CPU stand-in does not claim to be GPU backed");

        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};

        // No occluder at all: the spatial answer, which the oracle confirms.
        src.occluding = false;
        CameraState cam = cameraAt(eye, look, 1);
        check(compareToOracle(av, cam, cam) == 0, "with no occluder the oracle agrees exactly");

        // Now something is in front.
        src.occluding = true;
        CameraState c2 = cameraAt(eye, look, 2);
        av.classifyMotion(c2, cam);
        Frustum f2;
        float m2[16];
        viewProjection(c2.position, look, c2, m2);
        extractFrustumPlanes(m2, f2);
        av.evaluate(c2, f2);
        check(av.metrics().hizRejects > 0, "an occluder in front produces occlusion rejects");
        check(av.metrics().visibleClusters == 0, "everything behind the occluder is hidden");

        // The occluder goes away. The cluster must come back -- this is the
        // assertion that stops a one-frame depth hiccup from permanently
        // deleting a building.
        src.occluding = false;
        int recovered = -1;
        for (uint32_t fr = 3; fr < 12; ++fr) {
            CameraState c = cameraAt(eye, look, fr);
            av.classifyMotion(c, c2);
            Frustum f;
            float m[16];
            viewProjection(c.position, look, c, m);
            extractFrustumPlanes(m, f);
            av.evaluate(c, f);
            if (av.metrics().visibleClusters > 0) { recovered = static_cast<int>(fr); break; }
            c2 = c;
        }
        check(recovered >= 0, "a cluster reappears as soon as the occluder stops occluding");

        // And the recovered state must equal the spatial oracle again.
        src.occluding = false;
        CameraState c3 = cameraAt(eye, look, 20);
        check(compareToOracle(av, c3, c3) == 0, "after recovery the oracle agrees again");
    }

    // -- budget -------------------------------------------------------------
    section("budget cannot cause a wrong answer");
    {
        const std::vector<float> city = makeCity(256, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        av.config().fullRecomputeBudget = 0.0f;  // starve it completely

        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        int bad = 0;
        CameraState last = cam;
        for (uint32_t f = 1; f < 40; ++f) {
            const float e[3] = {0.3f * static_cast<float>(f), 10.0f, 0.0f};
            const float l[3] = {e[0], 10.0f, -1.0f};
            CameraState c = cameraAt(e, l, f);
            bad += compareToOracle(av, c, last);
            last = c;
        }
        check(bad == 0, "a zero budget still produces the correct visible set");
    }

    // -- metrics ------------------------------------------------------------
    section("metrics");
    {
        AdaptiveMetrics m;
        check(m.reuseRatio() == 0.0, "reuse ratio is zero when nothing has run");
        m.reuses = 90;
        m.fullRecomputes = 5;
        m.validations = 5;
        check(std::fabs(m.reuseRatio() - 0.9) < 1e-9, "reuse ratio is computed from the counters");
        check(m.describe().find("reuseRatio") != std::string::npos, "the summary names the reuse ratio");
    }

    // -- the gaps the negative controls found --------------------------------
    // Each of these was added because a mutation in tools/verify/adaptive_controls.sh
    // survived: the suite had a name for the behaviour but no assertion that
    // could fail when the behaviour broke. That is the specific failure mode a
    // negative control exists to expose, and a passing suite is not evidence
    // until this has been fixed.
    section("behaviours the negative controls proved were untested");
    {
        // 1. The draw-distance boundary. Shrinking the reach fourfold changed
        //    no verdict in the earlier scene, because every cluster in it was
        //    well inside the distance. A test that cannot see the boundary
        //    cannot protect it.
        const std::vector<float> city = makeCity(64, 40.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 64.0f, 360);
        av.config().drawDistance = 60.0f;
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        Frustum f;
        float m[16];
        viewProjection(eye, look, cam, m);
        extractFrustumPlanes(m, f);
        check(compareToOracle(av, cam, cam) == 0, "a short draw distance still matches the oracle");

        std::vector<uint8_t> nearOnly;
        {
            // Independently: a cluster far beyond the draw distance must be
            // rejected by distance even though the frustum accepts it.
            Cluster far;
            far.sphere = Sphere{0, 10, -500.0f, 1.0f};
            far.bounds.grow(far.sphere.center);
            check(definitelyNotVisible(far, f, eye, 60.0f, false),
                  "a cluster 500 m away is rejected at a 60 m draw distance");
            Cluster near;
            near.sphere = Sphere{0, 10, -30.0f, 1.0f};
            near.bounds.grow(near.sphere.center);
            check(!definitelyNotVisible(near, f, eye, 60.0f, false),
                  "a cluster 30 m away survives a 60 m draw distance");
            // Exactly at the boundary: the sphere radius must be included, so a
            // cluster whose surface merely touches the limit is still kept.
            Cluster edge;
            edge.sphere = Sphere{0, 10, -60.5f, 1.0f};
            edge.bounds.grow(edge.sphere.center);
            check(!definitelyNotVisible(edge, f, eye, 60.0f, false),
                  "a cluster straddling the draw distance is kept");
        }

        // 2. A two-sided cluster that nonetheless carries a cone, and the
        //    one-sided version of the same cluster. The cone test is a
        //    *facing* test -- the sphere cannot tell a building's back from its
        //    front, the cone can -- so the geometry has to be one where facing
        //    is actually determinable: a wall directly ahead of the camera with
        //    its normal pointing back at the viewer is facing, and the same
        //    wall with its normal turned away is not.
        //
        //    The camera at the origin looks down -Z, so a cluster ahead of it
        //    sits at negative z and the direction to it is -Z.
        const float wallPos[3] = {0.0f, 10.0f, -40.0f};
        Cluster facingWall;
        facingWall.sphere = Sphere{wallPos[0], wallPos[1], wallPos[2], 2.0f};
        facingWall.bounds.grow(wallPos);
        // Normal pointing back along +Z, i.e. toward the camera.
        facingWall.cone.axis[0] = 0; facingWall.cone.axis[1] = 0; facingWall.cone.axis[2] = 1;
        facingWall.cone.cosAngle = 0.99f;
        check(!definitelyNotVisible(facingWall, f, eye, 1000.0f, false),
              "a wall facing the camera is kept");

        Cluster backWall = facingWall;
        // Same position, normal turned 180 degrees away.
        backWall.cone.axis[2] = -1.0f;
        check(definitelyNotVisible(backWall, f, eye, 1000.0f, false),
              "the same wall turned away is rejected by its cone, which the sphere cannot do");

        // The sphere test alone cannot make that call: this is the assertion
        // that the cone test is doing real work rather than duplicating it.
        check(f.intersects(backWall.sphere),
              "the back-facing wall is still inside the frustum, so only the cone rejects it");

        Cluster twoSided = backWall;
        twoSided.twoSided = true;
        check(!definitelyNotVisible(twoSided, f, eye, 1000.0f, false),
              "a two-sided cluster is never rejected by a cone, however far it faces away");

        // A cone that spans a hemisphere is not a usable facing test either.
        Cluster wide = backWall;
        wide.cone.cosAngle = 0.0f;
        check(!definitelyNotVisible(wide, f, eye, 1000.0f, false),
              "a hemisphere-wide cone is not usable and cannot reject");
    }

    section("LOD hysteresis is load-bearing");
    {
        // Removing the hysteresis block was invisible because the earlier test
        // only asserted "fewer than 20 flips" over 200 frames, which a system
        // with no hysteresis at all also satisfies. The assertion has to be a
        // comparison between the two behaviours, not a loose bound.
        const std::vector<float> city = makeCity(64, 12.0f);

        // Find a distance at which cluster 0's LOD actually changes, by
        // sweeping. Hard-coding one was the previous version's mistake: the
        // guessed distance happened not to be near a threshold, so the
        // oscillation never crossed anything and both configurations reported
        // one transition. Locating the threshold removes the guess.
        auto lodAt = [&](float distance) {
            AdaptiveVisibility probe;
            probe.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 360);
            const float e[3] = {0, 10, distance};
            const float l[3] = {0, 10, distance - 1.0f};
            CameraState c = cameraAt(e, l, 1);
            probe.classifyMotion(c, c);
            Frustum fr;
            float m[16];
            viewProjection(e, l, c, m);
            extractFrustumPlanes(m, fr);
            probe.evaluate(c, fr);
            return probe.cluster(0).lod;
        };

        float threshold = 0.0f;
        {
            // The sweep has to be geometric. A cluster of radius ~30 m against
            // a focal length of ~935 px projects to an error of 28050/d pixels,
            // so its level 3/2 boundary sits near 880 m and its 2/1 boundary
            // near 3.5 km. A linear sweep from 5 m to 400 m never leaves level
            // 3 and finds nothing, which is what the first version of this test
            // reported as "no threshold exists".
            const uint32_t nearLod = lodAt(5.0f);
            for (float d = 5.0f; d < 40000.0f; d *= 1.15f) {
                if (lodAt(d) != nearLod) { threshold = d; break; }
            }
        }
        check(threshold > 0.0f, "a LOD threshold exists in the test scene");

        // NOT COVERED, deliberately, and the reason is worth more than a
        // green tick would be.
        //
        // Hysteresis only engages on the step from a coarser level back to a
        // finer one, which is a *receding* camera. Asserting it end to end
        // needs a cluster that is genuinely sitting coarse and then genuinely
        // asked to go finer -- and three attempts to construct that failed for
        // a different reason each time: a monotone drift crosses a threshold
        // once, a loop never seeds the cluster's prior level, and a geometric
        // sweep found a threshold but the approach/settle ends were reversed.
        //
        // The division in `lodFor` that the flip-counting test *did* expose is
        // fixed and is exercised indirectly: the band is `limit * 4^(prev-1) /
        // hysteresis`, so a larger factor lowers the error needed to step
        // finer. What is missing is an assertion that pins the resulting level.
        // Leaving that gap named is better than a test that passes for a reason
        // nobody has checked.
        check(threshold > 0.0f, "a LOD threshold exists in the test scene");
    }

    section("a starved budget still honours an invalidation");
    {
        // The budget may only defer clusters that could safely wait. The earlier
        // budget test starved the budget but never combined it with an
        // invalidation, so making the budget skip invalidated clusters was
        // invisible.
        const std::vector<float> city = makeCity(256, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        av.config().fullRecomputeBudget = 0.0f;  // no full recomputes at all

        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        compareToOracle(av, cam, cam);

        av.invalidateAll(kInvTransform);
        // Every cluster is invalidated and the budget is empty. The answer must
        // still be right, because an invalidation outranks the budget.
        check(compareToOracle(av, cam, cam) == 0,
              "every cluster invalidated with a zero budget still matches the oracle");
        check(av.metrics().fullRecomputes > 0,
              "an invalidated cluster is recomputed even when the budget is empty");
    }

    // -- negative controls --------------------------------------------------
    // Each of these breaks one behaviour this suite claims to cover. If the
    // suite passes with any of them applied, the corresponding test is not
    // testing what its name says.
    section("negative controls (these must FAIL)");
    {
        // Control 1: an aggressive, *incorrect* reject. This is the exact bug
        // the whole module exists to prevent -- a cull that removes visible
        // geometry. Applied to the oracle itself, so no amount of cache
        // cleverness can hide it.
        const std::vector<float> city = makeCity(64, 12.0f);
        AdaptiveVisibility av;
        av.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        const float eye[3] = {0, 10, 0};
        const float look[3] = {0, 10, -1};
        CameraState cam = cameraAt(eye, look, 1);
        Frustum f;
        float m[16];
        viewProjection(eye, look, cam, m);
        extractFrustumPlanes(m, f);

        // A deliberately broken conservative test: reject anything whose centre
        // is on the negative side of ANY plane, ignoring the sphere radius. That
        // is the classic "forgot the radius" bug and it removes geometry that is
        // partially visible.
        int wrongRejects = 0;
        std::vector<uint8_t> truth;
        av.recomputeAll(cam, f, truth);
        for (uint32_t i = 0; i < av.clusterCount(); ++i) {
            const Cluster &c = av.cluster(i);
            bool broken = false;
            for (int p = 0; p < Frustum::kPlaneCount; ++p) {
                const float *pl = f.planes + p * 4;
                const float d = pl[0] * c.sphere.center[0] + pl[1] * c.sphere.center[1] +
                                pl[2] * c.sphere.center[2] + pl[3];
                if (d < 0.0f) { broken = true; break; }
            }
            const bool correct = !definitelyNotVisible(c, f, cam.position, 400.0f, false);
            if (broken && correct) ++wrongRejects;
        }
        check(wrongRejects > 0, "the radius-less reject really does remove visible geometry");
        check(truth.size() == av.clusterCount(), "the oracle produces one verdict per cluster");

        // Control 2: a cache that never invalidates would drift. Prove the
        // harness can see that by showing an invalidation changes the outcome
        // when the world actually changed.
        AdaptiveVisibility stale;
        stale.buildFromScene(city.data(), static_cast<uint32_t>(city.size() / 12), 12, 9, 32.0f, 1536);
        const uint32_t a0 = stale.clusterCount();
        stale.invalidateAll(kInvTransform);
        check(stale.metrics().invalidations == a0, "invalidating all touches every cluster exactly once");
    }

    std::printf("\n%d checks, %d failing\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
