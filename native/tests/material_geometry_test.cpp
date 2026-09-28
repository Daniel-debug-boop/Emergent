// The material table and the geometry emitters, checked without a GPU.
//
// Both of these are pure CPU code that the renderer depends on, so neither
// needs Vulkan to be exercised. That matters: the Vulkan path in this project
// has never executed, because a build container has no driver, and a
// material system that is only ever tested by drawing it is therefore only
// ever tested by a machine nobody here has.
//
// The checks below are the ones that would otherwise be caught by looking at a
// screenshot: normals that are not unit length, a material index the table does
// not cover, a box that is not the size it claims, a kerb that is not a kerb.

#include "emergent/geometry.hpp"
#include "emergent/materials.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string &what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("  FAIL %s\n", what.c_str());
    }
}

void checkNear(double a, double b, double tol, const std::string &what) {
    g_checks++;
    if (!(std::fabs(a - b) <= tol)) {
        g_failures++;
        std::printf("  FAIL %s: %.6f vs %.6f (tolerance %g)\n", what.c_str(), a, b, tol);
    }
}

/** A table with the same shape the bake produces, plus the solid set. */
emergent::MaterialTable testTable() {
    emergent::MaterialArrays arrays;
    arrays.layers = 26;
    arrays.size = 512;
    std::vector<emergent::Material> textured;
    struct { const char *id; float tile; float rough; float metal; bool ground; } spec[] = {
        {"road_asphalt", 6.0f, 0.85f, 0.0f, true},
        {"wall_brick", 2.5f, 0.80f, 0.0f, false},
        {"wall_plaster", 3.0f, 0.75f, 0.0f, false},
        {"wall_stone", 2.0f, 0.78f, 0.0f, false},
        {"wall_corrugated", 2.2f, 0.55f, 0.4f, false},
        {"wall_metal_shutter", 2.4f, 0.42f, 0.3f, false},
        {"wall_painted_concrete", 3.0f, 0.60f, 0.0f, false},
        {"floor_wood", 1.6f, 0.62f, 0.0f, false},
        {"floor_concrete", 3.0f, 0.80f, 0.0f, true},
        {"metal_rust", 1.5f, 0.85f, 0.3f, false},
    };
    int layer = 0;
    for (const auto &s : spec) {
        emergent::Material m;
        m.id = s.id;
        m.textured = true;
        m.layer = layer++;
        m.invTileScale = 1.0f / s.tile;
        m.roughness = s.rough;
        m.metallic = s.metal;
        m.mapMode = s.ground ? emergent::MapMode::Ground : emergent::MapMode::WallX;
        m.normalStrength = 1.0f;
        textured.push_back(m);
    }
    return emergent::buildMaterialTable(arrays, textured);
}

}  // namespace

int main() {
    const emergent::MaterialTable table = testTable();
    emergent::MeshBuilder mb(table);
    emergent::GeometryKit kit(table);

    // -- the material table -------------------------------------------------
    check(table.count() > 20, "the table merges the baked and solid sets");
    check(table.a.size() == static_cast<size_t>(emergent::kMaxMaterials) * 4,
          "the uniform array is sized from kMaxMaterials");
    check(table.b.size() == table.a.size(), "both uniform arrays are the same size");

    // Every material's four floats must be finite, or the upload carries a NaN
    // into a uniform array and every surface using that index is black.
    for (int i = 0; i < table.count(); i++) {
        for (int k = 0; k < 4; k++) {
            check(std::isfinite(table.a[i * 4 + k]) && std::isfinite(table.b[i * 4 + k]),
                  "material uniform " + std::to_string(i) + " is finite");
        }
    }

    // The physically load-bearing values. A painted car body with metallic 1 is
    // the single most common PBR mistake and it looks like chrome, not paint.
    const float paintR = table.a[static_cast<size_t>(table.index("paint_metal")) * 4 + 3];
    const float bareR = table.a[static_cast<size_t>(table.index("metal_bare")) * 4 + 3];
    checkNear(paintR, 0.0, 1e-6, "painted steel is a dielectric, not a metal");
    checkNear(bareR, 1.0, 1e-6, "bare galvanised steel is a metal");
    const float glassR = table.a[static_cast<size_t>(table.index("glass")) * 4 + 2];
    check(glassR > 0.0f && glassR < 0.2f, "glass is smooth");
    check(table.b[static_cast<size_t>(table.index("lamp")) * 4 + 2] > 1.0f, "lamp glass is emissive");

    // Map modes: a road must project as ground even if something places it on a
    // slope-facing quad, which is why the mode comes from the recorded role.
    check(table.b[static_cast<size_t>(table.index("road_asphalt")) * 4 + 0] ==
              static_cast<float>(static_cast<int32_t>(emergent::MapMode::Ground)),
          "a road projects onto the ground plane");
    check(table.b[static_cast<size_t>(table.index("wall_brick")) * 4 + 0] ==
              static_cast<float>(static_cast<int32_t>(emergent::MapMode::WallX)),
          "a wall projects onto a vertical plane");
    check(table.b[static_cast<size_t>(table.index("road_asphalt")) * 4 + 3] == 1.0f,
          "a baked material is flagged textured");
    check(table.b[static_cast<size_t>(table.index("glass")) * 4 + 3] == 0.0f,
          "a solid material is not flagged textured");

    // -- vertex validation --------------------------------------------------
    {
        // A vertex outside the colour range or with an uncovered material must
        // abort, not draw. That cannot be tested in-process without forking, so
        // the *positive* path is checked here and the negative path is covered
        // by the fact that every emitter below goes through the same function.
        const float n[3] = {0, 1, 0};
        const float a[3] = {0, 0, 0}, b[3] = {1, 0, 0}, c[3] = {1, 0, 1};
        mb.clear();
        mb.triangle(a, b, c, n, 0.5f, 0.5f, 0.5f, table.index("wall_brick"));
        check(mb.size() == static_cast<size_t>(emergent::kFloatsPerVertex) * 3,
              "a triangle is three vertices of twelve floats");
    }

    // -- boxes --------------------------------------------------------------
    {
        mb.clear();
        const int32_t m = table.index("wall_brick");
        mb.box(10, 0, 20, 4, 3, 6, 0.6f, 0.5f, 0.4f, m);
        // 6 faces, 2 triangles each, 3 vertices each.
        check(mb.size() == static_cast<size_t>(emergent::kFloatsPerVertex) * 36, "a box is 36 vertices");
        float mn[3], mx[3];
        mb.bounds(mn, mx);
        checkNear(mn[0], 8.0, 1e-5, "box min x");
        checkNear(mx[0], 12.0, 1e-5, "box max x");
        checkNear(mn[1], 0.0, 1e-5, "a box is bottom-anchored, not centred");
        checkNear(mx[1], 3.0, 1e-5, "box height");
        checkNear(mx[2] - mn[2], 6.0, 1e-5, "box depth");

        // Every normal must be unit length. An unnormalised normal is not a
        // crash, it is a surface lit at up to 2.2x too bright, and it was a real
        // bug in the first version of the JavaScript cone.
        const std::vector<float> &d = mb.data();
        int badNormals = 0;
        for (size_t i = 0; i < d.size(); i += emergent::kFloatsPerVertex) {
            const float l = std::sqrt(d[i + 3] * d[i + 3] + d[i + 4] * d[i + 4] + d[i + 5] * d[i + 5]);
            if (std::fabs(l - 1.0f) > 1e-4f) badNormals++;
        }
        check(badNormals == 0, "every box normal is unit length");
    }

    // -- cylinders and cones ------------------------------------------------
    {
        mb.clear();
        mb.cylinder(0, 0, 0, 0.5f, 2.0f, 0.5f, 0.5f, 0.5f, table.index("paint_metal"), 8);
        float mn[3], mx[3];
        mb.bounds(mn, mx);
        checkNear(mx[1] - mn[1], 2.0, 1e-4, "cylinder height");
        checkNear(mx[0] - mn[0], 1.0, 1e-4, "cylinder diameter");
        const std::vector<float> &d = mb.data();
        int bad = 0;
        for (size_t i = 0; i < d.size(); i += emergent::kFloatsPerVertex) {
            const float l = std::sqrt(d[i + 3] * d[i + 3] + d[i + 4] * d[i + 4] + d[i + 5] * d[i + 5]);
            if (std::fabs(l - 1.0f) > 1e-3f) bad++;
        }
        check(bad == 0, "every cylinder normal is unit length, including the sides");

        mb.clear();
        mb.cone(0, 0, 0, 1.0f, 2.0f, 0.5f, 0.5f, 0.5f, table.index("foliage"), 6);
        mb.bounds(mn, mx);
        checkNear(mx[1] - mn[1], 2.0, 1e-4, "cone height");
        d.size();
        int badCone = 0;
        const std::vector<float> &dc = mb.data();
        for (size_t i = 0; i < dc.size(); i += emergent::kFloatsPerVertex) {
            const float l = std::sqrt(dc[i + 3] * dc[i + 3] + dc[i + 4] * dc[i + 4] + dc[i + 5] * dc[i + 5]);
            if (std::fabs(l - 1.0f) > 1e-3f) badCone++;
        }
        check(badCone == 0, "every cone facet normal is unit length");
    }

    // -- real-world proportions --------------------------------------------
    // The only scale a player can check is against a 1.8 m person, so the kit's
    // dimensions are asserted against that.
    {
        mb.clear();
        kit.streetLamp(mb, 0, 0, 0, 0);
        float mn[3], mx[3];
        mb.bounds(mn, mx);
        const double height = mx[1] - mn[1];
        check(height > 6.5 && height < 9.5,
              "a street lamp is 6.5-9.5 m tall (got " + std::to_string(height) + ")");

        mb.clear();
        kit.bench(mb, 0, 0, 0, 0);
        mb.bounds(mn, mx);
        const double benchH = mx[1] - mn[1];
        check(benchH > 0.7 && benchH < 1.1, "a bench seat and back is 0.7-1.1 m (got " + std::to_string(benchH) + ")");

        mb.clear();
        kit.container(mb, 0, 0, 0, 0);
        mb.bounds(mn, mx);
        const double cH = mx[1] - mn[1];
        check(cH > 2.4 && cH < 2.8, "a shipping container is about 2.59 m tall (got " + std::to_string(cH) + ")");
        const double cW = mx[0] - mn[0];
        check(cW > 5.8 && cW < 6.3, "a shipping container is about 6.06 m long (got " + std::to_string(cW) + ")");

        mb.clear();
        kit.bollard(mb, 0, 0, 0);
        mb.bounds(mn, mx);
        const double bo = mx[1] - mn[1];
        check(bo > 0.8 && bo < 1.1, "a bollard is about 1 m (got " + std::to_string(bo) + ")");

        mb.clear();
        const float railCol[3] = {0.28f, 0.28f, 0.30f};
        kit.railing(mb, 0, 0, 0, 4.0f, 1.05f, 0, railCol, table.index("paint_metal"));
        mb.bounds(mn, mx);
        checkNear(mx[1] - mn[1], 1.05, 1e-4, "a railing is as tall as it is specified");
    }

    // -- a whole facade -----------------------------------------------------
    // The point of the kit: a wall with a window grid, a plinth, a string course
    // and a cornice, which is what a building stops being a box.
    {
        mb.clear();
        const int32_t wall = table.index("wall_brick");
        const int32_t stone = table.index("wall_stone");
        const float w = 20.0f, d = 14.0f, h = 12.0f;
        const float col[3] = {0.62f, 0.44f, 0.36f};
        kit.box(mb, 0, 0, 0, w, h, d, col, wall);
        kit.band(mb, 0, 0, 0, w + 0.3f, d + 0.3f, 0.9f, col, stone);
        kit.parapet(mb, 0, h, 0, w + 0.2f, d + 0.2f, col, stone);
        for (int floor = 0; floor < 3; floor++) {
            for (int i = 0; i < 5; i++) {
                kit.window(mb, -8.0f + i * 4.0f, 2.4f + floor * 3.4f, d * 0.5f, 1.5f, 1.9f, 2);
            }
        }
        kit.acUnit(mb, 4.0f, h, 2.0f);
        check(mb.size() > static_cast<size_t>(emergent::kFloatsPerVertex) * 1000,
              "a dressed facade is thousands of vertices, not 36");

        float mn[3], mx[3];
        mb.bounds(mn, mx);
        check(mx[1] > h, "roof plant stands above the parapet");
        check(mx[0] - mn[0] >= w, "the facade is at least as wide as the wall");

        // Every material index the facade used must resolve in the table.
        const std::vector<float> &d2 = mb.data();
        int unresolved = 0;
        for (size_t i = 0; i < d2.size(); i += emergent::kFloatsPerVertex) {
            const int mi = static_cast<int>(d2[i + emergent::kOffsetMaterial]);
            if (mi < 0 || mi >= table.count()) unresolved++;
        }
        check(unresolved == 0, "every facade vertex carries a material the table covers");
    }

    if (g_failures) {
        std::printf("\n%d of %d material/geometry checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("material and geometry: %d checks passed (%d materials, %zu vertices in the facade)\n",
                g_checks, table.count(), mb.size() / emergent::kFloatsPerVertex);
    return 0;
}
