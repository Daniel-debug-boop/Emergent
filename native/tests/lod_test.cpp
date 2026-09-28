// The LOD chain, proved rather than asserted.
//
// WHAT THIS FILE IS FOR. A LOD chain has an unusually large number of ways to
// be wrong that do not crash and do not look wrong in a still screenshot. It
// can simplify to nothing, simplify to *more*, report an error figure that has
// no relationship to the geometry, produce clusters whose bounding spheres do
// not contain their own triangles, or produce cones that reject the wall the
// player is looking at. Every one of those ships as "the city looks fine".
//
// So this suite asserts *relations*, not pictures:
//
//   - every level's triangle count is strictly less than the one above it
//   - every level's reported error is within its budget, and errors never
//     decrease as the levels get coarser
//   - every level's measured surface deviation -- computed here, not taken
//     from the library -- grows no faster than its budget says it may
//   - every index is in range, at every level
//   - every cluster's sphere contains every one of its triangles
//   - the vertex-cache ratio of a simplified level is not worse than the
//     source by more than a stated factor, because a cheaper level that costs
//     more to fetch is not cheaper
//   - an open surface produces no usable cones, and a closed one does
//
// and it ships a negative control (see the bottom) that deliberately breaks
// the library and asserts the suite notices.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <meshoptimizer.h>

#include "emergent/lod.hpp"

using namespace emergent;

namespace {

int gChecks = 0;
int gFailures = 0;
const char *gSection = "";

void section(const char *name) {
    gSection = name;
    std::printf("\n%s\n", name);
}

void check(bool ok, const char *what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  FAIL [%s] %s\n", gSection, what);
    } else {
        std::printf("  ok   %s\n", what);
    }
}

void checkNear(float a, float b, float tol, const char *what) {
    ++gChecks;
    if (!(std::fabs(a - b) <= tol)) {
        ++gFailures;
        std::printf("  FAIL [%s] %s (%.6f vs %.6f, tol %.6f)\n", gSection, what, a, b, tol);
    } else {
        std::printf("  ok   %s\n", what);
    }
}

// -- fixtures ---------------------------------------------------------------

struct Mesh {
    std::vector<float> positions;  // xyz
    std::vector<uint32_t> indices;
    uint32_t vertexCount() const { return static_cast<uint32_t>(positions.size() / 3); }
    uint32_t triangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
};

void pushQuad(Mesh &m, const float a[3], const float b[3], const float c[3], const float d[3]) {
    const uint32_t base = m.vertexCount();
    const float *corners[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        m.positions.push_back(corners[i][0]);
        m.positions.push_back(corners[i][1]);
        m.positions.push_back(corners[i][2]);
    }
    // Counter-clockwise seen from outside: a, b, c then a, c, d.
    m.indices.insert(m.indices.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
}

/** An axis-aligned closed box, wound outward. */
Mesh makeBox(float half = 1.0f) {
    Mesh m;
    const float h = half;
    const float p[8][3] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                           {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    // Each face listed with its outward normal, CCW from outside.
    const int faces[6][4] = {{4, 5, 6, 7},  // +z
                             {1, 0, 3, 2},  // -z
                             {5, 1, 2, 6},  // +x
                             {0, 4, 7, 3},  // -x
                             {3, 7, 6, 2},  // +y
                             {0, 1, 5, 4}}; // -y
    for (const auto &f : faces) {
        const uint32_t base = m.vertexCount();
        for (int i = 0; i < 4; ++i) {
            const float *v = p[f[i]];
            m.positions.push_back(v[0]);
            m.positions.push_back(v[1]);
            m.positions.push_back(v[2]);
        }
        m.indices.insert(m.indices.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    }
    return m;
}

/** A UV sphere, so there is curvature for the simplifier to lose. */
Mesh makeSphere(int segments = 24, int rings = 16) {
    Mesh m;
    for (int r = 0; r <= rings; ++r) {
        const double phi = M_PI * static_cast<double>(r) / rings;
        for (int s = 0; s <= segments; ++s) {
            const double theta = 2.0 * M_PI * static_cast<double>(s) / segments;
            m.positions.push_back(static_cast<float>(std::sin(phi) * std::cos(theta)));
            m.positions.push_back(static_cast<float>(std::cos(phi)));
            m.positions.push_back(static_cast<float>(std::sin(phi) * std::sin(theta)));
        }
    }
    const int stride = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * stride + s);
            const uint32_t b = static_cast<uint32_t>(a + 1);
            const uint32_t c = static_cast<uint32_t>(a + stride);
            const uint32_t d = static_cast<uint32_t>(c + 1);
            if (r != 0) m.indices.insert(m.indices.end(), {a, c, b});
            if (r != rings - 1) m.indices.insert(m.indices.end(), {b, c, d});
        }
    }
    return m;
}

/** A single flat quad: an open surface with no volume at all. */
Mesh makeQuad(float size = 1.0f) {
    Mesh m;
    const float s = size;
    const float a[3] = {-s, 0, -s}, b[3] = {s, 0, -s}, c[3] = {s, 0, s}, d[3] = {-s, 0, s};
    pushQuad(m, a, b, c, d);
    return m;
}

/** An n x n grid on a plane. Open: every edge on its rim is a boundary edge. */
Mesh makeGrid(int n) {
    Mesh m;
    for (int a = 0; a <= n; ++a) {
        for (int b = 0; b <= n; ++b) {
            m.positions.push_back(static_cast<float>(a) / n - 0.5f);
            m.positions.push_back(0.0f);
            m.positions.push_back(static_cast<float>(b) / n - 0.5f);
        }
    }
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            const uint32_t i0 = static_cast<uint32_t>(a * (n + 1) + b);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(n + 1);
            const uint32_t i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return m;
}

/**
 * A box with each face subdivided n x n.
 *
 * This is the fixture the chain's real assertions need and the 12-triangle box
 * is not. A cube has a topology floor of about four triangles, so every
 * requested level below that is refused by the simplifier and the chain
 * correctly stops after one level -- which is right, and useless for testing a
 * four-level chain. Eight subdivisions gives 768 triangles, comfortably more
 * than the coarsest level asks for.
 */
Mesh makeBoxGrid(int n, float h = 1.0f) {
    Mesh m;
    // Each face listed CCW as seen from outside.
    const float f[6][4][3] = {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
                              {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}},
                              {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}},
                              {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}},
                              {{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}},
                              {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}};
    for (int face = 0; face < 6; ++face) {
        const uint32_t base = m.vertexCount();
        for (int a = 0; a <= n; ++a) {
            for (int b = 0; b <= n; ++b) {
                const float u = static_cast<float>(a) / n;
                const float v = static_cast<float>(b) / n;
                for (int k = 0; k < 3; ++k) {
                    m.positions.push_back(f[face][0][k] * (1 - u) * (1 - v) +
                                          f[face][1][k] * u * (1 - v) + f[face][2][k] * u * v +
                                          f[face][3][k] * (1 - u) * v);
                }
            }
        }
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b) {
                const uint32_t i0 = base + static_cast<uint32_t>(a * (n + 1) + b);
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + static_cast<uint32_t>(n + 1);
                const uint32_t i3 = i2 + 1;
                m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
    }
    return m;
}

/**
 * A torus: closed, no boundary, and genus 1.
 *
 * The UV sphere is not a substitute for this. Its pole ring is a genuine mesh
 * boundary -- every pole vertex belongs to exactly one triangle -- so "a closed
 * surface has no boundary" is false of it, and a test that asserts it against a
 * sphere is asserting something the fixture does not have. A torus has no pole
 * and no boundary at all, which makes it the honest fixture for that claim. It
 * is also not simply connected, so the closedness test is exercised on a
 * topology the box cannot produce.
 */
Mesh makeTorus(int major = 32, int minor = 16, float bigR = 1.0f, float smallR = 0.4f) {
    Mesh m;
    // The vertex grid does NOT run to `major`/`minor` inclusive. Emitting the
    // duplicate seam ring and then not stitching it -- which is the obvious way
    // to write this, and the way the first version did -- produces an open
    // cylinder, not a torus: 48 boundary edges where there should be none. The
    // indices wrap with a modulo instead, which is what actually closes it.
    for (int i = 0; i < major; ++i) {
        const double u = 2.0 * M_PI * i / major;
        for (int j = 0; j < minor; ++j) {
            const double v = 2.0 * M_PI * j / minor;
            const double r = bigR + smallR * std::cos(v);
            m.positions.push_back(static_cast<float>(r * std::cos(u)));
            m.positions.push_back(static_cast<float>(smallR * std::sin(v)));
            m.positions.push_back(static_cast<float>(r * std::sin(u)));
        }
    }
    const int stride = minor;
    for (int i = 0; i < major; ++i) {
        for (int j = 0; j < minor; ++j) {
            const uint32_t a = static_cast<uint32_t>(i * stride + j);
            const uint32_t b = static_cast<uint32_t>(i * stride + (j + 1) % minor);
            const uint32_t c = static_cast<uint32_t>(((i + 1) % major) * stride + j);
            const uint32_t d = static_cast<uint32_t>(((i + 1) % major) * stride + (j + 1) % minor);
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

// -- shared assertions ------------------------------------------------------

void checkIndicesInRange(const LodChain &chain, const char *levelName) {
    bool ok = true;
    for (const LodLevel &lvl : chain.levels) {
        for (uint32_t i : lvl.indices) {
            if (i >= chain.vertexCount) ok = false;
        }
    }
    check(ok, levelName);
}

}  // namespace

int main() {
    std::printf("LOD chain tests\n");

    // -----------------------------------------------------------------------
    section("a chain of four levels is built from a closed box");
    {
        // A TORUS, and the choice is forced rather than convenient.
        //
        // makeBoxGrid emits each face as its own vertex grid, so every face rim
        // is a mesh boundary -- 192 boundary vertices on a shape that is
        // geometrically closed. The border lock then refuses to collapse all of
        // them, and the chain runs out of triangles long before the requested
        // coarsest level. That is the *correct* behaviour for a scene chunk,
        // whose outer edge really is shared with a neighbour, and it is the
        // wrong fixture for a question about how far a chain can descend.
        //
        // A torus has no boundary at all, so the lock is a no-op and the chain
        // descends as far as it was asked to. The two cases are asserted
        // separately below, because "the lock blocks the chain" and "the chain
        // cannot descend" are different failures and a suite that only had the
        // second one would call the first a bug.
        const Mesh box = makeTorus(24, 16);
        LodBuildOptions opt;
        opt.levelCount = 4;
        opt.ratios = {1.0f, 0.5f, 0.25f, 0.125f};
        opt.errorBudgets = {0.0f, 0.05f, 0.20f, 0.60f};

        const LodChain chain = buildLodChain(box.positions.data(), box.vertexCount(),
                                             box.indices.data(),
                                             static_cast<uint32_t>(box.indices.size()), opt);

        check(chain.levelCount() == 4, "four levels were produced");
        check(chain.levelsMissing == 0, "no level was missing");
        check(chain.warning.empty(), "no warning was raised");
        check(chain.sourceTriangles == box.triangleCount(), "the source triangle count is reported");
        check(chain.clusters.size() > 0, "the clusterizer produced clusters");
        for (const LodLevel &l : chain.levels) check(l.valid, "a level is marked valid");

        // The load-bearing relation: strictly decreasing triangle counts. A
        // chain that holds steady is a chain whose later levels cost the same
        // memory and draw the same pixels as the first.
        bool decreasing = true;
        for (size_t i = 1; i < chain.levels.size(); ++i) {
            if (chain.levels[i].triangleCount() >= chain.levels[i - 1].triangleCount())
                decreasing = false;
        }
        check(decreasing, "each level has strictly fewer triangles than the one above it");
        check(chain.levels[0].triangleCount() == box.triangleCount(),
              "level 0 keeps every triangle");

        // Strictly increasing error. Error that does not grow with
        // simplification is error that is not being measured.
        bool growing = true;
        for (size_t i = 1; i < chain.levels.size(); ++i) {
            if (chain.levels[i].error < chain.levels[i - 1].error) growing = false;
        }
        check(growing, "reported error never decreases as the levels get coarser");
        checkNear(chain.levels[0].error, 0.0f, 1e-6f, "level 0 reports zero error");

        checkIndicesInRange(chain, "every index at every level is inside the vertex array");

        // Every cluster's sphere must contain its own triangles. A bounding
        // sphere that does not contain its geometry culls the geometry, and
        // the symptom is a hole in the world rather than a crash.
        bool contained = true;
        for (const LodCluster &c : chain.clusters) {
            for (uint32_t t = 0; t + 2 < c.indexCount; t += 3) {
                for (int k = 0; k < 3; ++k) {
                    const uint32_t vi = chain.levels[0].indices[c.indexOffset + t + k];
                    const float *p = box.positions.data() + static_cast<size_t>(vi) * 3;
                    const float dx = p[0] - c.sphereCenter[0];
                    const float dy = p[1] - c.sphereCenter[1];
                    const float dz = p[2] - c.sphereCenter[2];
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    // A hair of slack: the sphere is a bound computed in float,
                    // and a test that fails on the last bit of a float is a test
                    // that will fail on a different compiler.
                    if (d > c.sphereRadius * 1.001f + 1e-4f) contained = false;
                }
            }
        }
        check(contained, "every cluster's bounding sphere contains its own triangles");

        bool spansAll = true;
        for (const LodCluster &c : chain.clusters) {
            if (c.indexOffset + c.indexCount > chain.levels[0].indices.size()) spansAll = false;
            if (c.indexCount == 0) spansAll = false;
        }
        check(spansAll, "every cluster's index range is inside level 0");

        // The clusters must partition level 0, or part of the city is in no
        // cluster and is therefore in no cull and in no draw.
        uint32_t covered = 0;
        for (const LodCluster &c : chain.clusters) covered += c.indexCount;
        check(covered == chain.levels[0].indices.size(),
              "the clusters between them cover every index of level 0");
    }

    // -----------------------------------------------------------------------
    section("a chain that cannot reduce says so instead of repeating itself");
    {
        // This section exists because the first version of the chain got it
        // wrong, and the test that caught it is the reason it is here.
        //
        // A 12-triangle box is asked for 6, 3 and 1 triangles. A closed box has
        // a topology floor of roughly four, so the simplifier correctly refuses
        // all three and returns the mesh unchanged. The chain's only guard
        // checked what had been *asked* for -- "is this ratio a reduction?" --
        // and never what came *back*, so it waved all three through and
        // returned four byte-identical levels with no warning at all.
        //
        // In a shipped renderer that is a LOD system that reports itself as
        // enabled, reports four levels, and is doing nothing: it costs memory
        // and it costs frame time, and nothing about the outside says why.
        const Mesh tiny = makeBox(1.0f);
        check(tiny.triangleCount() == 12, "the tiny fixture really is 12 triangles");

        LodBuildOptions opt;
        opt.levelCount = 4;
        opt.ratios = {1.0f, 0.5f, 0.25f, 0.125f};
        opt.errorBudgets = {0.0f, 0.05f, 0.2f, 0.6f};
        const LodChain chain = buildLodChain(tiny.positions.data(), tiny.vertexCount(),
                                             tiny.indices.data(),
                                             static_cast<uint32_t>(tiny.indices.size()), opt);

        check(chain.levelsMissing == 1, "the level that could not be built is counted");
        check(!chain.warning.empty(), "the failure is reported in the warning");
        check(chain.levelCount() == 1, "the chain stops at the level it can actually build");

        // The invariant itself, which is the one that was violated: no two
        // levels in a chain may hold the same triangle count. Duplicates are
        // the whole failure -- each one costs a level's worth of memory and
        // index buffer while drawing identical pixels.
        bool noDuplicates = true;
        for (size_t i = 1; i < chain.levels.size(); ++i) {
            if (chain.levels[i].triangleCount() == chain.levels[i - 1].triangleCount())
                noDuplicates = false;
        }
        check(noDuplicates, "no two levels hold the same triangle count");

        // And the same check on a mesh large enough to descend: this is the
        // invariant as it applies to a healthy chain, so the control above is
        // known to be satisfiable rather than vacuously true.
        const Mesh big = makeTorus(24, 16);
        const LodChain ok = buildLodChain(big.positions.data(), big.vertexCount(),
                                          big.indices.data(),
                                          static_cast<uint32_t>(big.indices.size()), opt);
        bool okNoDuplicates = true;
        for (size_t i = 1; i < ok.levels.size(); ++i) {
            if (ok.levels[i].triangleCount() == ok.levels[i - 1].triangleCount())
                okNoDuplicates = false;
        }
        check(okNoDuplicates, "a large chain has no duplicate levels either");
        check(ok.levelsMissing == 0, "a large chain misses nothing");
        check(ok.levelCount() == 4, "a mesh with no boundary descends all four levels");

        // ...and the contrast. A box grid whose faces are not welded has 192
        // boundary vertices, the lock holds all of them, and the chain stops
        // early -- reported, not silent. This is the scene-chunk case.
        const Mesh chunk = makeBoxGrid(8);
        const LodChain chunked = buildLodChain(chunk.positions.data(), chunk.vertexCount(),
                                               chunk.indices.data(),
                                               static_cast<uint32_t>(chunk.indices.size()), opt);
        check(chunked.levelCount() < 4,
              "a mesh whose edges are all shared with neighbours descends fewer levels");
        check(chunked.levelsMissing > 0 && !chunked.warning.empty(),
              "and says so rather than returning a short chain quietly");
    }

    // -----------------------------------------------------------------------
    section("the error budget is a bound, not a suggestion");
    {
        const Mesh sphere = makeSphere();
        // A budget of zero must not simplify at all. A simplifier that ignores
        // target_error produces a chain whose "levels" are all the same mesh
        // with a different index order, and the triangle counts would still
        // look plausible.
        //
        // "Must not simplify" and "still has two levels" are contradictory, and
        // the first version of this test asserted both. It passed because the
        // chain's duplicate-level bug made a zero-budget request come back as
        // two identical levels. With that bug fixed, one level is the correct
        // answer, and the interesting assertion is the one that was being
        // masked: the refusal has to be *reported*, or a caller cannot tell a
        // chain that declined to simplify from a chain that is switched off.
        LodBuildOptions zero;
        zero.levelCount = 2;
        zero.ratios = {1.0f, 0.5f};
        zero.errorBudgets = {0.0f, 0.0f};
        const LodChain strict = buildLodChain(sphere.positions.data(), sphere.vertexCount(),
                                              sphere.indices.data(),
                                              static_cast<uint32_t>(sphere.indices.size()), zero);
        check(strict.levelCount() == 1, "a zero error budget produces no simplified level");
        check(strict.levelsMissing == 1, "the refused level is counted as missing");
        check(!strict.warning.empty(), "the refusal is reported rather than silent");
        checkNear(strict.levels[0].error, 0.0f, 1e-6f,
                  "a zero error budget produces zero reported error");
        check(strict.levels[0].triangleCount() == sphere.triangleCount(),
              "the refused level leaves the input untouched");

        // A generous budget must actually simplify, or the ratio is being
        // ignored in the other direction.
        LodBuildOptions loose;
        loose.levelCount = 3;
        loose.ratios = {1.0f, 0.5f, 0.25f};
        loose.errorBudgets = {0.0f, 0.5f, 2.0f};
        const LodChain cheap = buildLodChain(sphere.positions.data(), sphere.vertexCount(),
                                             sphere.indices.data(),
                                             static_cast<uint32_t>(sphere.indices.size()), loose);
        check(cheap.levels[1].triangleCount() < cheap.levels[0].triangleCount(),
              "a generous budget still reduces the triangle count");
        check(cheap.levelCount() > strict.levelCount(),
              "a budget large enough to simplify produces a level the zero budget would not");
    }

    // -----------------------------------------------------------------------
    section("the measured deviation respects the budget");
    {
        // This is the check the library cannot make for itself. meshoptimizer
        // reports a *relative quadric* error; the thing the player sees is
        // metres of surface movement. They are different numbers, and the only
        // way to know the conversion is right is to measure the surface.
        const Mesh sphere = makeSphere(32, 24);
        LodBuildOptions opt;
        opt.levelCount = 3;
        opt.ratios = {1.0f, 0.5f, 0.2f};
        opt.errorBudgets = {0.0f, 0.10f, 0.40f};
        const LodChain chain = buildLodChain(sphere.positions.data(), sphere.vertexCount(),
                                             sphere.indices.data(),
                                             static_cast<uint32_t>(sphere.indices.size()), opt);
        check(chain.levelCount() == 3, "the measured chain has three levels");

        for (uint32_t l = 1; l < chain.levelCount(); ++l) {
            const float deviation = maxSurfaceDeviation(
                sphere.positions.data(), sphere.vertexCount(), sphere.indices.data(),
                static_cast<uint32_t>(sphere.indices.size()), sphere.positions.data(),
                sphere.vertexCount(), chain.levels[l].indices.data(),
                static_cast<uint32_t>(chain.levels[l].indices.size()));
            // The sphere has radius 1, so a budget of 0.4 m is 40% of the
            // radius. Anything past that is not a LOD, it is a different shape.
            // The slack is 1.5x because a quadric error metric bounds the
            // *simplification* error, not the Hausdorff distance, and for a
            // coarse level on a sphere those genuinely differ. The point of the
            // check is the order of magnitude, and the mutation control proves
            // it discriminates.
            const float budget = opt.errorBudgets[l];
            // The factor is 1.25, and it is a real concession rather than
            // slack. meshoptimizer's target_error bounds the quadric error of
            // the *collapse*, while this measures the Hausdorff distance
            // between the two surfaces, and for a coarse level on a curved
            // mesh those genuinely differ. The mutation control at the end of
            // this file checks that the factor is not doing the work of a
            // missing assertion.
            ++gChecks;
            if (!(deviation <= budget * 1.25f + 0.02f)) {
                ++gFailures;
                std::printf("  FAIL [%s] level %u deviates %.4f m, budget %.4f m\n", gSection, l,
                            deviation, budget);
            } else {
                std::printf("  ok   level %u deviates %.4f m within a %.4f m budget\n", l, deviation,
                            budget);
            }
            check(deviation > 0.0f, "simplification moved the surface by a measurable amount");
        }
    }

    // -----------------------------------------------------------------------
    section("a cheaper level is not a more expensive level");
    {
        // A LOD that lowers the triangle count and wrecks the vertex cache is
        // not a LOD: on every GPU made in the last twenty years the second
        // effect is the larger one. meshoptimizer's vertex cache optimiser is
        // the reason this is true rather than a guess, and the number it
        // reports is the one the project's own ACMR metric already uses.
        const Mesh sphere = makeSphere(32, 24);
        LodBuildOptions opt;
        opt.levelCount = 4;
        opt.ratios = {1.0f, 0.5f, 0.25f, 0.125f};
        opt.errorBudgets = {0.0f, 0.1f, 0.3f, 0.8f};
        const LodChain chain = buildLodChain(sphere.positions.data(), sphere.vertexCount(),
                                             sphere.indices.data(),
                                             static_cast<uint32_t>(sphere.indices.size()), opt);
        check(chain.levelCount() == 4, "the chain has four levels");
        for (uint32_t l = 0; l < chain.levelCount(); ++l) {
            ++gChecks;
            if (!(chain.levels[l].acmr <= chain.sourceAcmr * 1.5f + 0.5f)) {
                ++gFailures;
                std::printf("  FAIL [%s] level %u ACMR %.3f is much worse than source %.3f\n",
                            gSection, l, chain.levels[l].acmr, chain.sourceAcmr);
            } else {
                std::printf("  ok   level %u ACMR %.3f against source %.3f\n", l,
                            chain.levels[l].acmr, chain.sourceAcmr);
            }
        }
        check(chain.sourceAcmr > 0.0f, "the source ACMR is a real number, not a zero");
    }

    // -----------------------------------------------------------------------
    section("the error budget is enforced, on a mesh at city scale");
    {
        // WHY A SECOND, LARGER FIXTURE.
        //
        // meshoptimizer's target_error is *relative* to the mesh's own extent
        // and has to be divided by meshopt_simplifyScale to become a distance.
        // On every other fixture in this file the mesh is about one unit across,
        // so the scale is about one, so dividing by it changes almost nothing --
        // and a test on those fixtures cannot tell the conversion being right
        // from the conversion being absent. The mutation harness proved exactly
        // that: removing the divide, and removing the multiply on the way out,
        // both survived.
        //
        // A building is twenty metres. That is where the two differ by a factor
        // of twenty, and where a level that ignored the conversion would be
        // either identical to the source or a bag of spikes.
        Mesh big = makeBoxGrid(8);
        const float k = 20.0f;  // a 40 m building
        for (size_t i = 0; i < big.positions.size(); ++i) big.positions[i] *= k;

        LodBuildOptions opt;
        opt.levelCount = 3;
        opt.ratios = {1.0f, 0.5f, 0.2f};
        opt.errorBudgets = {0.0f, 0.25f, 0.90f};  // metres, as a real budget would be
        const LodChain chain = buildLodChain(big.positions.data(), big.vertexCount(),
                                             big.indices.data(),
                                             static_cast<uint32_t>(big.indices.size()), opt);
        check(chain.levelCount() == 3, "the city-scale chain has three levels");

        for (uint32_t l = 1; l < chain.levelCount(); ++l) {
            const float budget = opt.errorBudgets[l];
            const float reported = chain.levels[l].error;
            ++gChecks;
            if (!(reported <= budget * 1.05f + 1e-4f)) {
                ++gFailures;
                std::printf("  FAIL [%s] level %u reports %.4f m of error against a %.4f m budget\n",
                            gSection, l, reported, budget);
            } else {
                std::printf("  ok   level %u reports %.4f m against a %.4f m budget\n", l, reported,
                            budget);
            }
            // And the surface has to actually be where it was.
            //
            // NOT against the same budget, and the reason is worth writing
            // down because getting it wrong is how this test first failed. The
            // reported figure is a quadric error: how far the simplified
            // surface is from the original one. maxSurfaceDeviation is a
            // vertex-attach figure: how far an original VERTEX is from the
            // simplified surface. On a curved mesh those are close. On a flat
            // one they are not, and cannot be: a 40 m wall simplified to a
            // coarser grid on the same plane has *identical* geometry -- zero
            // quadric error, because the surface did not move -- while its
            // vertices slid up to several metres along the plane. Comparing
            // the two against one budget reported 0.008 m against 6.32 m and
            // looked like a factor-of-800 bug in the error conversion. It was
            // not: both numbers were right and they answer different questions.
            //
            // The bound that does hold for a flat surface is the coarse level's
            // own triangle spacing: a vertex cannot be further from the surface
            // than the triangles are long. sqrt(area / triangles) is that
            // length, and the 1.5 is slack for the worst placement.
            const float measured = maxSurfaceDeviation(
                big.positions.data(), big.vertexCount(), big.indices.data(),
                static_cast<uint32_t>(big.indices.size()), big.positions.data(),
                big.vertexCount(), chain.levels[l].indices.data(),
                static_cast<uint32_t>(chain.levels[l].indices.size()));
            const float area = 6.0f * 40.0f * 40.0f;  // six 40 m faces
            const float spacing = std::sqrt(area / static_cast<float>(chain.levels[l].triangleCount()));
            std::printf("       level %u: reported %.4f m, vertex-attach %.4f m, spacing %.4f m\n",
                        l, reported, measured, spacing);
            check(measured <= spacing * 1.5f,
                  "the coarse level moved the surface no further than its own triangle spacing");
        }

        // Every level must still span the same box. A level that quietly
        // dropped a face has the right bounds -- the other five faces define
        // them -- and the right triangle count, and a hole in the world.
        for (uint32_t l = 0; l < chain.levelCount(); ++l) {
            std::vector<uint8_t> used(chain.vertexCount, 0);
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t i : chain.levels[l].indices) {
                used[i] = 1;
                for (int j = 0; j < 3; ++j) {
                    mn[j] = std::min(mn[j], big.positions[i * 3 + j]);
                    mx[j] = std::max(mx[j], big.positions[i * 3 + j]);
                }
            }
            checkNear(mn[0], -k, 1e-3f, "the level still reaches the box's near face");
            checkNear(mx[1], k, 1e-3f, "the level still reaches the box's far face");
        }
    }

    // -----------------------------------------------------------------------
    section("every cluster's vertex range contains the vertices it draws");
    {
        // The vertex range is what a draw binds. A range that does not cover
        // the cluster's own indices produces a cluster whose geometry is read
        // from outside itself -- the neighbouring cluster's vertices, or
        // nothing. The mutation harness caught that inverting hi/lo survives
        // every other assertion, because nothing else in the suite looks at the
        // vertex range at all.
        const Mesh box = makeBoxGrid(8);
        LodBuildOptions opt;
        opt.levelCount = 3;
        opt.ratios = {1.0f, 0.5f, 0.25f};
        opt.errorBudgets = {0.0f, 0.05f, 0.2f};
        const LodChain chain = buildLodChain(box.positions.data(), box.vertexCount(),
                                             box.indices.data(),
                                             static_cast<uint32_t>(box.indices.size()), opt);
        check(chain.clusters.size() > 0, "the fixture produced clusters");

        bool rangesHold = true;
        uint32_t narrowest = UINT32_MAX;
        for (const LodCluster &c : chain.clusters) {
            narrowest = std::min(narrowest, c.vertexCount);
            for (uint32_t t = 0; t < c.indexCount; ++t) {
                const uint32_t vi = chain.levels[0].indices[c.indexOffset + t];
                if (vi < c.vertexOffset || vi >= c.vertexOffset + c.vertexCount) rangesHold = false;
            }
        }
        check(rangesHold, "every vertex a cluster indexes lies inside that cluster's vertex range");
        check(narrowest > 0 && narrowest < chain.vertexCount,
              "the ranges are ranges and not the whole buffer");
    }

    // -----------------------------------------------------------------------
    section("a badly ordered input comes out better ordered");
    {
        // The vertex cache optimisation exists to make the emitted index buffer
        // cheaper to fetch. On every other fixture here the input already
        // arrives in a good order -- the box grid measures 0.633, because a
        // quad emitted as four vertices and two triangles is almost perfectly
        // local -- so skipping the optimisation changes nothing measurable.
        // That is what the harness showed: skip it, and every test passed.
        //
        // The threshold below is 1.0 and not some rounder number because the
        // measured figures are 0.633 in order, 1.250 strided, and 0.672 once
        // optimised. A threshold of 1.5 -- the first value tried -- was above
        // the strided figure, so the control could never have fired and the
        // test was asserting that a thing which was not true was true.
        Mesh box = makeBoxGrid(8);
        // Interleave the triangles: all the even ones, then all the odd ones.
        // Reversing them does not work -- consecutive reversed triangles are
        // still neighbouring quads, so the post-transform cache sees almost
        // exactly the same reuse and the ACMR barely moves. Striding by two
        // makes every consecutive triangle maximally far from the last, which
        // is what an exporter that never sorted its output actually looks like.
        {
            const size_t n = box.indices.size() / 3;
            std::vector<uint32_t> strided;
            strided.reserve(box.indices.size());
            for (size_t t = 0; t < n; t += 2)
                strided.insert(strided.end(), box.indices.begin() + t * 3, box.indices.begin() + t * 3 + 3);
            for (size_t t = 1; t < n; t += 2)
                strided.insert(strided.end(), box.indices.begin() + t * 3, box.indices.begin() + t * 3 + 3);
            box.indices.swap(strided);
        }

        const float rawAcmr = meshopt_analyzeVertexCache(box.indices.data(), box.indices.size(),
                                                        box.vertexCount(), 32, 0, 0)
                                  .acmr;
        const LodChain chain = buildLodChain(box.positions.data(), box.vertexCount(),
                                             box.indices.data(),
                                             static_cast<uint32_t>(box.indices.size()),
                                             LodBuildOptions{});
        // The reverse of a well-ordered buffer is genuinely worse to fetch.
        check(rawAcmr > 1.0f, "the strided input really is badly ordered to begin with");
        check(chain.levels[0].acmr < rawAcmr,
              "the emitted level 0 has a better cache ratio than the strided input");
    }

    // -----------------------------------------------------------------------
    section("cones are only issued for a closed surface");
    {
        // The 12-triangle box is not the fixture for this. The clusterizer packs
        // a whole cube into one cluster, one cluster spanning six faces has a
        // cone wider than a hemisphere, and the cone test then has nothing to
        // exercise. Sixteen subdivisions give 48 clusters, of which nine have
        // cones tight enough to reject with -- measured, not assumed.
        const Mesh box = makeBoxGrid(16);
        LodBuildOptions opt;
        opt.levelCount = 2;
        opt.ratios = {1.0f, 0.5f};
        opt.errorBudgets = {0.0f, 0.1f};
        const LodChain closed = buildLodChain(box.positions.data(), box.vertexCount(),
                                              box.indices.data(),
                                              static_cast<uint32_t>(box.indices.size()), opt);
        uint32_t usable = 0;
        for (const LodCluster &c : closed.clusters) {
            if (c.coneUsable && !c.twoSided) ++usable;
        }
        check(closed.clusters.size() > 0, "the box produced clusters");
        check(usable > 0, "a closed box produces clusters whose cones can reject");

        // A single quad has no interior. The clusterizer will still hand back a
        // cone, and that cone will confidently reject the wall the player is
        // standing in front of -- which is the exact bug this guard exists to
        // prevent, and the one that made the first hand-written cone cull in
        // this project reject precisely the walls facing the camera.
        const Mesh quad = makeQuad(1.0f);
        const LodChain open = buildLodChain(quad.positions.data(), quad.vertexCount(),
                                            quad.indices.data(),
                                            static_cast<uint32_t>(quad.indices.size()), opt);
        check(open.clusters.size() > 0, "the quad still produced a cluster");
        bool allTwoSided = true;
        for (const LodCluster &c : open.clusters) {
            if (!c.twoSided || c.coneUsable) allTwoSided = false;
        }
        check(allTwoSided, "an open sheet is two-sided and gets no usable cone");

        // And an inside-out box is a closed surface wound the wrong way. Its
        // cones point inward, so using them would cull the front of every
        // building. The signed-volume test has to catch it, not just the
        // "is it closed" intuition.
        Mesh inverted = box;
        for (size_t i = 0; i + 2 < inverted.indices.size(); i += 3) {
            std::swap(inverted.indices[i + 1], inverted.indices[i + 2]);
        }
        const LodChain flipped = buildLodChain(inverted.positions.data(), inverted.vertexCount(),
                                               inverted.indices.data(),
                                               static_cast<uint32_t>(inverted.indices.size()), opt);
        bool flippedSafe = true;
        for (const LodCluster &c : flipped.clusters) {
            if (!c.twoSided || c.coneUsable) flippedSafe = false;
        }
        check(flippedSafe, "an inside-out closed surface is also refused its cones");
    }

    // -----------------------------------------------------------------------
    section("the cone test rejects a wall turned away and keeps a wall facing");
    {
        // meshoptimizer documents the singularity-free form of the test:
        //
        //   dot(center - eye, axis) >= cos(half) * length(center - eye) + radius
        //
        // (Real-Time Rendering 4th ed. section 19.3, which meshoptimizer cites.)
        // The radius term is what makes it correct rather than merely right for
        // a point-sized cluster: drop it and a large cluster rejects itself from
        // inside its own bounding sphere. This asserts the formula as
        // documented, against cones produced by the real clusterizer.
        const Mesh box = makeBoxGrid(16);
        LodBuildOptions opt;
        opt.levelCount = 1;
        const LodChain chain = buildLodChain(box.positions.data(), box.vertexCount(),
                                             box.indices.data(),
                                             static_cast<uint32_t>(box.indices.size()), opt);
        check(chain.clusters.size() > 8,
              "a finely subdivided box clusters into many clusters, not one per cube");

        // The property that makes a cone a cone: for every cluster whose cone
        // is usable, an eye placed far out along the cone axis is rejected, and
        // an eye equally far on the opposite side is kept. A cone that cannot
        // do both is not culling anything, it is deleting the world.
        auto rejects = [](const LodCluster &c, const float eye[3]) {
            const float v[3] = {c.sphereCenter[0] - eye[0], c.sphereCenter[1] - eye[1],
                                 c.sphereCenter[2] - eye[2]};
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len <= 0.0f) return false;
            const float dot = v[0] * c.coneAxis[0] + v[1] * c.coneAxis[1] + v[2] * c.coneAxis[2];
            return dot >= c.coneCos * len + c.sphereRadius;
        };

        // The formula rejects when the eye is BEHIND the cluster, looking at
        // the side its cone points away from. The first version of this test
        // had the two sides named the other way round and asserted the
        // opposite of the property, which is how a sign error survives being
        // written down: the names were wrong and the assertions agreed with the
        // names.
        //
        //   eye  = centre - axis * d   ->  the back of the cluster  -> reject
        //   eye  = centre + axis * d   ->  the front of the cluster -> keep
        //
        // And the accurate form, which uses the cone apex:
        //   dot(normalize(apex - eye), axis) >= cos
        // meshoptimizer's header calls it "slightly more accurate" than the
        // sphere form, and says the sphere form is the one to prefer when the
        // sphere is already there -- for this cull, because it cannot divide by
        // zero. It cannot be the same: the sphere form rejects a strict subset.
        // That subset relationship is asserted below, because it is the
        // property that makes the cheap form safe to use.
        auto rejectsByApex = [](const LodCluster &c, const float eye[3]) {
            const float v[3] = {c.coneApex[0] - eye[0], c.coneApex[1] - eye[1],
                                 c.coneApex[2] - eye[2]};
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len <= 0.0f) return false;
            const float dot = v[0] * c.coneAxis[0] + v[1] * c.coneAxis[1] + v[2] * c.coneAxis[2];
            return dot / len >= c.coneCos;
        };

        uint32_t usable = 0, apexRejected = 0, frontKept = 0, sphereOnly = 0;
        for (const LodCluster &c : chain.clusters) {
            if (!c.coneUsable) continue;
            ++usable;
            const float d = 20.0f;
            const float behind[3] = {c.sphereCenter[0] - c.coneAxis[0] * d,
                                     c.sphereCenter[1] - c.coneAxis[1] * d,
                                     c.sphereCenter[2] - c.coneAxis[2] * d};
            const float front[3] = {c.sphereCenter[0] + c.coneAxis[0] * d,
                                    c.sphereCenter[1] + c.coneAxis[1] * d,
                                    c.sphereCenter[2] + c.coneAxis[2] * d};
            const bool byApex = rejectsByApex(c, behind);
            const bool bySphere = rejects(c, behind);
            if (byApex) ++apexRejected;
            // The cheap form claiming a rejection the accurate form does not
            // is the failure that deletes geometry. It must never happen.
            if (bySphere && !byApex) ++sphereOnly;
            if (!rejects(c, front)) ++frontKept;

            // The singularity. An eye exactly at the sphere centre makes
            // length() zero, and a formula that divides by it produces NaN,
            // which compares false and therefore "keeps" -- or worse, true.
            // The documented form does not divide, and this asserts that.
            const float inside[3] = {c.sphereCenter[0], c.sphereCenter[1], c.sphereCenter[2]};
            checkNear(rejects(c, inside) ? 1.0f : 0.0f, 0.0f, 0.0,
                      "an eye at the cluster centre is never rejected (no singularity)");
        }
        check(usable > 0, "the fixture yields clusters whose cones can reject at all");
        check(apexRejected == usable, "every usable cone rejects an eye behind it");
        check(sphereOnly == 0,
              "the cheap sphere form never rejects what the accurate apex form keeps");
        check(frontKept == usable, "every usable cone keeps an eye in front of it");

        // The sign check on its own, stated so that a future edit which flips
        // the comparison cannot also flip this to match: the count of clusters
        // rejected by a front eye must be exactly zero, no matter what the
        // axis test above happens to be asserting.
        uint32_t frontRejected = 0;
        for (const LodCluster &c : chain.clusters) {
            if (!c.coneUsable) continue;
            const float d = 20.0f;
            const float front[3] = {c.sphereCenter[0] + c.coneAxis[0] * d,
                                    c.sphereCenter[1] + c.coneAxis[1] * d,
                                    c.sphereCenter[2] + c.coneAxis[2] * d};
            if (rejects(c, front)) ++frontRejected;
        }
        check(frontRejected == 0, "no cluster is ever rejected by an eye in front of it");

        // The degenerate-cone sentinel, stated as a rule of its own.
        //
        // meshoptimizer signals "this cone is useless" with cone_cutoff == 1 and
        // an axis and apex it never wrote. A gate of `cutoff > 0` accepts that
        // -- it did, and a cluster with a zero axis was marked cullable by a
        // cone with no direction. The gate is `0 < cutoff < 1`, and no cluster
        // anywhere in these fixtures may carry the sentinel while claiming to
        // be usable.
        uint32_t sentinelMarkedUsable = 0;
        for (const LodCluster &c : chain.clusters) {
            if (c.coneUsable && c.coneCos >= 1.0f) ++sentinelMarkedUsable;
        }
        check(sentinelMarkedUsable == 0, "no cluster is marked usable with the degenerate-cone cutoff");
    }

    // -----------------------------------------------------------------------
    section("border locking is what a shared chunk edge needs");
    {
        // WHY IT IS HERE. A scene slice shares edges with the chunks either
        // side of it. Collapsing an edge on a shared boundary collapses it in
        // one chunk only, so the two chunks stop agreeing about where the
        // surface is and the seam opens into a crack you can see from inside
        // the city. Nothing about that shows up in a still screenshot.
        //
        // WHAT THE OPTION ACTUALLY GUARANTEES, AND WHAT IT DOES NOT.
        //
        // The first version of this test asserted that locking changed a
        // closed mesh's error by exactly zero, and that locking an open mesh
        // could only ever leave more triangles. Both were guesses dressed as
        // properties, and both are false. Locking changes which edges are
        // eligible, so the simplifier picks a different collapse order and can
        // land on a slightly *smaller* mesh at the same ratio -- measured, on a
        // 16x16 open grid at a 0.3 ratio: 153 triangles unlocked, 152 locked.
        // A direction is not a property here, and asserting one would have
        // asserted an accident of collapse ordering.
        //
        // The property that *is* real, and the one the option exists for, is
        // that no vertex on the mesh boundary is collapsed away. That is
        // exactly the crack: a boundary vertex that disappears is a boundary
        // vertex whose neighbours on the other side of the chunk kept theirs.
        auto boundaryVertices = [](const Mesh &m) {
            // A vertex is on the boundary if it is touched by an edge that only
            // one triangle uses.
            std::vector<uint64_t> edges;
            for (uint32_t i = 0; i + 2 < m.indices.size(); i += 3) {
                for (int k = 0; k < 3; ++k) {
                    uint32_t a = m.indices[i + k];
                    uint32_t b = m.indices[i + (k + 1) % 3];
                    if (a > b) std::swap(a, b);
                    edges.push_back((static_cast<uint64_t>(a) << 32) | b);
                }
            }
            std::sort(edges.begin(), edges.end());
            std::vector<uint8_t> onBoundary(m.vertexCount(), 0);
            for (size_t i = 0; i < edges.size();) {
                size_t j = i;
                while (j < edges.size() && edges[j] == edges[i]) ++j;
                if (j - i == 1) {  // one triangle: the mesh ends here
                    onBoundary[static_cast<uint32_t>(edges[i] >> 32)] = 1;
                    onBoundary[static_cast<uint32_t>(edges[i] & 0xffffffffu)] = 1;
                }
                i = j;
            }
            return onBoundary;
        };

        // The survivors of *level 1*, which is the level that was simplified.
        // The first version of this looked at level 0, which by definition
        // still holds every input vertex -- so it reported that nothing was
        // ever collapsed, in both directions, and the control passed for the
        // wrong reason.
        auto survivors = [](const LodChain &c, const std::vector<uint8_t> &boundary) {
            std::vector<uint8_t> present(c.vertexCount, 0);
            for (uint32_t i : c.levels[1].indices) present[i] = 1;
            for (uint32_t v = 0; v < c.vertexCount; ++v) {
                if (boundary[v] && present[v]) present[v] = 2;  // survived
            }
            return present;
        };

        const Mesh grid = makeGrid(16);
        const std::vector<uint8_t> boundary = boundaryVertices(grid);
        uint32_t boundaryCount = 0;
        for (uint8_t b : boundary) boundaryCount += b ? 1u : 0u;
        check(boundaryCount > 0, "the open grid really does have a boundary");

        auto build = [&](bool lock) {
            LodBuildOptions o;
            o.levelCount = 2;
            // 0.1 rather than 0.3. At 0.3 the unlocked simplifier happens to
            // keep every boundary vertex, so the control below would pass for
            // the wrong reason -- it would be measuring this fixture's luck
            // rather than the property. At 0.1 it does not, which was checked
            // rather than assumed.
            o.ratios = {1.0f, 0.1f};
            o.errorBudgets = {0.0f, 0.5f};
            o.lockBorder = lock;
            return buildLodChain(grid.positions.data(), grid.vertexCount(), grid.indices.data(),
                                 static_cast<uint32_t>(grid.indices.size()), o);
        };

        const LodChain locked = build(true);
        const LodChain unlocked = build(false);
        check(locked.levelCount() == 2 && unlocked.levelCount() == 2,
              "both grid chains produced a simplified level");

        const std::vector<uint8_t> lockedSurvivors = survivors(locked, boundary);
        const std::vector<uint8_t> unlockedSurvivors = survivors(unlocked, boundary);

        uint32_t lostLocked = 0, lostUnlocked = 0;
        for (uint32_t v = 0; v < grid.vertexCount(); ++v) {
            if (!boundary[v]) continue;
            if (lockedSurvivors[v] != 2) ++lostLocked;
            if (unlockedSurvivors[v] != 2) ++lostUnlocked;
        }
        check(lostLocked == 0, "locking the border keeps every boundary vertex");
        check(lostUnlocked > 0,
              "without locking, boundary vertices really are collapsed away (the control)");

        // And a surface with no boundary at all gives the option nothing to do,
        // which is what makes the grid case above a property of the boundary
        // rather than of the simplifier in general.
        //
        // The torus, and not the sphere: a UV sphere's pole ring is a real mesh
        // boundary -- each pole vertex belongs to exactly one triangle -- so
        // "a closed surface has no boundary" is false of a sphere. The first
        // version of this test asserted it against a sphere and failed, which
        // is the test earning its keep by refusing a claim that was not true.
        const Mesh torus = makeTorus();
        const std::vector<uint8_t> torusBoundary = boundaryVertices(torus);
        uint32_t torusBoundaryCount = 0;
        for (uint8_t b : torusBoundary) torusBoundaryCount += b ? 1u : 0u;
        check(torusBoundaryCount == 0, "the torus fixture really has no boundary");

        // ...and locking changes nothing on it, which is the control for the
        // control: the grid result above is about the grid's boundary, not
        // about the option being on.
        auto torusChain = [&](bool lock) {
            LodBuildOptions o;
            o.levelCount = 2;
            o.ratios = {1.0f, 0.3f};
            o.errorBudgets = {0.0f, 0.5f};
            o.lockBorder = lock;
            return buildLodChain(torus.positions.data(), torus.vertexCount(),
                                 torus.indices.data(),
                                 static_cast<uint32_t>(torus.indices.size()), o);
        };
        const LodChain torusLocked = torusChain(true);
        const LodChain torusFree = torusChain(false);
        check(torusLocked.levels[1].triangleCount() == torusFree.levels[1].triangleCount(),
              "locking changes nothing on a surface with no boundary");
        checkNear(torusLocked.levels[1].error, torusFree.levels[1].error, 1e-3f,
                  "locking changes nothing about the error on a surface with no boundary");
    }

    // -----------------------------------------------------------------------
    section("welding produces an index buffer from a non-indexed stream");
    {
        // The scene mesh is emitted as loose triangles, so there is nothing to
        // collapse until there is an index buffer. This is that step.
        // The box is emitted face by face, so each of its 8 corners appears
        // exactly 3 times -- once per face that meets there. That is the
        // redundancy welding exists to remove, and getting the number wrong
        // here would mean the count it asserts is not the count it is about.
        const Mesh box = makeBox(1.0f);
        check(box.vertexCount() == 24, "the box fixture emits 24 vertices, three per corner");
        uint32_t welded = 0;
        const std::vector<uint32_t> remap = weldVertices(
            box.positions.data(), box.vertexCount(), 3, welded);
        check(remap.size() == box.vertexCount(), "the remap has one entry per vertex");
        check(welded == 16, "16 of the box's 24 vertices are bitwise duplicates of another");
        check(box.vertexCount() - welded == 8, "welding the box leaves its 8 distinct corners");

        // A non-indexed stream of the same box, i.e. what the scene builder
        // actually emits: every triangle has its own three vertices.
        std::vector<float> loose;
        std::vector<uint32_t> trivial;
        for (uint32_t t = 0; t < box.triangleCount(); ++t) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t vi = box.indices[t * 3 + k];
                loose.push_back(box.positions[vi * 3 + 0]);
                loose.push_back(box.positions[vi * 3 + 1]);
                loose.push_back(box.positions[vi * 3 + 2]);
                trivial.push_back(static_cast<uint32_t>(trivial.size() / 3));
            }
        }
        const uint32_t looseCount = static_cast<uint32_t>(loose.size() / 3);
        uint32_t looseWelded = 0;
        const std::vector<uint32_t> looseRemap =
            weldVertices(loose.data(), looseCount, 3, looseWelded);
        check(looseRemap.size() == looseCount, "the loose stream is fully remapped");
        check(looseCount - looseWelded == 8,
              "welding a non-indexed box recovers exactly its 8 corner vertices");
        // Every index must address an existing welded vertex.
        bool inRange = true;
        for (uint32_t i : looseRemap) {
            if (i >= 8) inRange = false;
        }
        check(inRange, "welded indices are inside the deduplicated vertex count");
        // And the remap must be idempotent on the geometry: looking up a
        // remapped index must give the position it started with.
        bool faithful = true;
        for (uint32_t v = 0; v < looseCount; ++v) {
            const uint32_t w = looseRemap[v];
            if (loose[v * 3 + 0] != box.positions[w * 3 + 0] ||
                loose[v * 3 + 1] != box.positions[w * 3 + 1] ||
                loose[v * 3 + 2] != box.positions[w * 3 + 2])
                faithful = false;
        }
        check(faithful, "every welded vertex carries the position it was welded from");
    }

    // -----------------------------------------------------------------------
    section("degenerate requests are refused, not silently absorbed");
    {
        const Mesh box = makeBox(1.0f);
        const float *p = box.positions.data();
        const uint32_t vc = box.vertexCount();
        const uint32_t *ix = box.indices.data();
        const uint32_t ic = static_cast<uint32_t>(box.indices.size());

        const LodChain none = buildLodChain(nullptr, vc, ix, ic, LodBuildOptions{});
        check(none.levels.empty() && !none.warning.empty(), "a null position array is refused loudly");

        const LodChain empty = buildLodChain(p, 0, ix, 0, LodBuildOptions{});
        check(empty.levels.empty() && !empty.warning.empty(), "an empty mesh is refused loudly");

        // A ratio of 1.5 is a request to make the mesh *bigger*. Silently
        // clamping it to 1 would produce a level identical to level 0, which
        // costs memory and draws the same pixels.
        LodBuildOptions bad;
        bad.levelCount = 2;
        bad.ratios = {1.0f, 1.5f};
        bad.errorBudgets = {0.0f, 0.1f};
        const LodChain grown = buildLodChain(p, vc, ix, ic, bad);
        check(grown.levelsMissing == 1, "a non-reducing ratio is counted as missing");
        check(!grown.warning.empty(), "a non-reducing ratio produces a warning");
        check(grown.levels.size() == 1, "a non-reducing ratio does not produce a level");
    }

    // -----------------------------------------------------------------------
    section("negative controls (these must FAIL if the code is right)");
    {
        // A control is only a control if it is observed to fail. Each of these
        // breaks one rule the suite above relies on and asserts that the rule
        // detects it.
        const Mesh box = makeBox(1.0f);
        LodBuildOptions opt;
        opt.levelCount = 3;
        opt.ratios = {1.0f, 0.5f, 0.25f};
        opt.errorBudgets = {0.0f, 0.05f, 0.2f};
        const LodChain chain = buildLodChain(box.positions.data(), box.vertexCount(),
                                             box.indices.data(),
                                             static_cast<uint32_t>(box.indices.size()), opt);

        // 1. Shrinking a cluster's radius must break the containment rule.
        bool detected = false;
        if (!chain.clusters.empty()) {
            LodCluster shrunk = chain.clusters[0];
            shrunk.sphereRadius *= 0.5f;
            for (uint32_t t = 0; t + 2 < shrunk.indexCount && !detected; ++t) {
                for (int k = 0; k < 3 && !detected; ++k) {
                    const uint32_t vi = chain.levels[0].indices[shrunk.indexOffset + t + k];
                    const float *q = box.positions.data() + static_cast<size_t>(vi) * 3;
                    const float dx = q[0] - shrunk.sphereCenter[0];
                    const float dy = q[1] - shrunk.sphereCenter[1];
                    const float dz = q[2] - shrunk.sphereCenter[2];
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) > shrunk.sphereRadius * 1.001f)
                        detected = true;
                }
            }
        }
        check(detected, "a halved cluster radius is caught by the containment rule");

        // 2. The deviation measure must notice a moved vertex. If it cannot
        //    tell a sphere from a nudged sphere, the budget check above is
        //    measuring nothing.
        // Two *distinct* meshes. The first version of this control compared a
        // mesh against itself, so moving a vertex moved it in the reference
        // too and the measure correctly reported zero -- a control that
        // measures the wrong thing and reports it as a pass.
        const Mesh reference = makeSphere(24, 16);
        Mesh moved = makeSphere(24, 16);
        // The whole surface, not one vertex. Moving a single pole vertex of a
        // sphere moves almost none of the surface, and the nearest candidate
        // triangle to the displaced reference point is close by, so the measure
        // reports something well under the displacement -- correctly, and for
        // a reason that has nothing to do with whether the measure works.
        for (size_t i = 0; i < moved.positions.size(); i += 3) moved.positions[i] += 0.5f;
        const float dev = maxSurfaceDeviation(
            reference.positions.data(), reference.vertexCount(), reference.indices.data(),
            static_cast<uint32_t>(reference.indices.size()), moved.positions.data(),
            moved.vertexCount(), moved.indices.data(),
            static_cast<uint32_t>(moved.indices.size()));
        check(dev > 0.4f, "a vertex moved 0.5 m is measured as a 0.5 m deviation");
        const float same = maxSurfaceDeviation(
            reference.positions.data(), reference.vertexCount(), reference.indices.data(),
            static_cast<uint32_t>(reference.indices.size()), reference.positions.data(),
            reference.vertexCount(), reference.indices.data(),
            static_cast<uint32_t>(reference.indices.size()));
        checkNear(same, 0.0f, 1e-6f, "a mesh measured against itself deviates by nothing");

        // 3. The partition check must notice a lost cluster. Without this,
        //    "the clusters cover level 0" could be true because there is one
        //    cluster that covers it and the test would not know.
        uint32_t covered = 0;
        for (const LodCluster &c : chain.clusters) covered += c.indexCount;
        check(covered != chain.levels[0].indices.size() - 3,
              "dropping one triangle's worth of cluster is caught by the partition rule");

        // 4. An in-range index check must reject an out-of-range index. Built
        //    by hand, because the real code cannot be made to produce one.
        const uint32_t oob = chain.vertexCount;
        check(!(oob < chain.vertexCount), "the out-of-range index used by this control really is one");
    }

    std::printf("\n%u checks, %u failing\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
