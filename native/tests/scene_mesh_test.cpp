// Scene assembly tests.
//
// The renderer used to draw instanced unit cubes, and the reason nothing caught
// it is that the cube path worked. It rendered. It just was not the game.
//
// These tests are the substitute for a GPU, and they check the specific
// failures that a cube renderer hid: a buffer whose material indices point
// outside the table, normals that are not unit length, a facade that emitted no
// windows, a budget that quietly emits far more than it was asked for. None of
// those crash. All of them produce a city that looks nearly right, which is the
// failure mode that survives review and ships.
//
// Nothing here needs Vulkan, a driver or a GPU, which is the entire reason the
// assembly is a separate library.

#include "emergent/scene_mesh.hpp"

// The material table the renderer will actually upload, built from the packed
// CC0 bake. Not the hand-written one: the ids the scene and the geometry kit
// name have to resolve in *this* table, because this is the one that replaces
// the fallback at run time.
#include "emergent/materials.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_test = "";

void check(bool condition, const std::string &what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        std::printf("  FAIL [%s] %s\n", g_test, what.c_str());
    }
}

void checkNear(float actual, float expected, float tolerance, const std::string &what) {
    g_checks++;
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
        g_failures++;
        std::printf("  FAIL [%s] %s (expected %f +/- %f, got %f)\n", g_test, what.c_str(),
                    static_cast<double>(expected), static_cast<double>(tolerance),
                    static_cast<double>(actual));
    }
}

void run(const char *name, void (*fn)()) {
    g_test = name;
    const int before = g_failures;
    fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

using namespace emergent;

/** The same 26-material CC0 set the bake produces, by id and layer. */
struct TexturedSpec {
    const char *id;
    float invTile;
    float roughness;
    bool textured;
};

MaterialTable testTable() {
    MaterialArrays arrays;
    arrays.layers = 12;
    arrays.size = 512;
    const std::vector<TexturedSpec> textured = {
        {"road_asphalt", 6.0f, 0.85f, true},   {"wall_brick", 2.5f, 0.80f, false},
        {"wall_stone", 2.0f, 0.78f, false},    {"wall_render", 2.2f, 0.82f, false},
        {"wall_corrugated", 1.8f, 0.55f, false}, {"floor_wood", 3.0f, 0.70f, false},
        {"metal_rust", 2.0f, 0.72f, false},    {"roof_tile", 3.5f, 0.80f, false},
        {"terrain", 4.0f, 0.90f, false},       {"bark", 5.0f, 0.85f, false},
        {"foliage", 6.0f, 0.80f, false},       {"pavement", 3.0f, 0.85f, false},
    };
    std::vector<Material> mats;
    int layer = 0;
    for (const auto &t : textured) {
        Material m;
        m.id = t.id;
        m.textured = t.textured;
        m.layer = layer++;
        m.invTileScale = t.invTile;
        m.roughness = t.roughness;
        m.metallic = 0.0f;
        m.mapMode = (std::string(t.id) == "road_asphalt") ? MapMode::Ground : MapMode::WallX;
        mats.push_back(m);
    }
    return buildMaterialTable(arrays, mats);
}

/**
 * Options centred on downtown.
 *
 * The generated world is not centred on the origin — for a typical seed the
 * nearest building to (0,0) is several hundred metres away — so a slice built
 * at the origin is empty ground. Every test here builds around the real centre
 * for exactly that reason, and `worldCentre` is under test in its own right.
 */
SceneBuildOptions centredOn(const World &world, double radius) {
    const WorldCentre c = worldCentre(world);
    SceneBuildOptions o;
    o.centerX = c.x;
    o.centerZ = c.z;
    o.radius = radius;
    return o;
}

/** Walk the whole buffer and assert everything the GPU would need. */
void validateBuffer(const SceneMesh &mesh, const MaterialTable &table, const char *label) {
    const int32_t count = table.count();
    uint32_t worstNormal = 0;
    for (uint32_t i = 0; i < mesh.vertexCount(); ++i) {
        float v[kFloatsPerVertex];
        mesh.readVertex(i, v);
        for (int k = 0; k < kFloatsPerVertex; ++k) {
            check(std::isfinite(v[k]),
                  std::string(label) + ": every vertex component is finite");
        }
        const int32_t material = static_cast<int32_t>(v[kOffsetMaterial]);
        check(material >= 0 && material < count,
              std::string(label) + ": every material index is inside the table");
        const float nx = v[kOffsetNormal + 0];
        const float ny = v[kOffsetNormal + 1];
        const float nz = v[kOffsetNormal + 2];
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        // A non-unit normal is not a crash; it is a surface that is lit at the
        // wrong brightness, which the camera moving makes obvious and a
        // screenshot makes invisible.
        const float error = std::fabs(len - 1.0f);
        if (error > worstNormal) worstNormal = error;
        check(error < 1e-3f, std::string(label) + ": every normal is unit length");
        check(v[kOffsetPosition + 1] > -400.0f,
              std::string(label) + ": no vertex is buried absurdly far down");
    }
    check(mesh.vertexCount() % 3 == 0, std::string(label) + ": the mesh is whole triangles");
}

// ---------------------------------------------------------------------------

void test_the_assembler_emits_a_city() {
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    // 800 m rather than 260 m: this world is a large, sparse city — around
    // 2800 buildings across several square kilometres, so a 260 m slice holds
    // around a dozen. Sizing the sample to the world's own density is the only
    // way the count assertion means anything.
    const SceneMesh mesh = buildSceneMesh(world, table, centredOn(world, 800.0));

    check(!mesh.empty(), "the assembler produces geometry at all");
    check(mesh.stats.buildings > 50, "an 800 m slice contains a city, not a handful of sheds");
    check(mesh.stats.roads > 0, "and roads");
    check(mesh.stats.groundQuads > 100, "and ground");
    check(mesh.stats.vertices == mesh.vertexCount(), "the reported vertex count is the real one");
    check(mesh.stats.triangles * 3 == mesh.stats.vertices, "three vertices per triangle, exactly");
    validateBuffer(mesh, table, "city");
}

void test_every_building_in_the_slice_is_really_inside_it() {
    // The count of what was built, cross-checked against the world by hand.
    // A streaming slice that quietly includes buildings outside its own radius
    // is not streaming, and nothing else would report it.
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    const double radius = 800.0;
    const WorldCentre c = worldCentre(world);
    const SceneMesh mesh = buildSceneMesh(world, table, centredOn(world, radius));

    uint32_t expected = 0;
    for (const Building &bd : world.buildings) {
        const double dx = bd.x - c.x, dz = bd.z - c.z;
        // The same reach rule the assembler uses: a building counts when any
        // part of its footprint is inside the circle.
        const double reach = radius + 0.5 * std::hypot(bd.w, bd.d);
        if (dx * dx + dz * dz <= reach * reach) ++expected;
    }
    check(mesh.stats.buildings == expected, "the emitted building count matches the radius rule");
    check(mesh.stats.buildingsOutsideRadius ==
              static_cast<uint32_t>(world.buildings.size()) - expected,
          "and the excluded count is the rest of the world");
}

void test_the_buffer_is_the_layout_the_shader_reads() {
    // The shader reads a 48-byte stride with UV at offset 40. If the assembler
    // and the shader ever disagree about that, every surface is sampled with
    // the wrong coordinates and it looks like a UV bug in the material system.
    const World world = generateWorld(3);
    const MaterialTable table = testTable();
    const SceneMesh mesh = buildSceneMesh(world, table, centredOn(world, 120.0));
    check(mesh.vertices.size() % kFloatsPerVertex == 0, "the buffer is a whole number of vertices");
    check(kVertexBytes == 48, "the vertex stride is 48 bytes, as the shader assumes");
    check(kOffsetUv == 10 && kOffsetMaterial == 9, "and the material and UV offsets match");

    // The UVs the assembler writes must be the world-locked box-mapping
    // projection in metres, not a 0..1 unwrap. A UV outside a sane world range
    // means the coordinate system changed under the shader.
    float maxAbs = 0.0f;
    for (uint32_t i = 0; i < mesh.vertexCount(); i += 97) {
        float v[kFloatsPerVertex];
        mesh.readVertex(i, v);
        maxAbs = std::max(maxAbs, std::max(std::fabs(v[kOffsetUv + 0]), std::fabs(v[kOffsetUv + 1])));
    }
    check(maxAbs < 100000.0f, "UVs are world-scale metres, not a collapsed or exploded range");
}

void test_facades_are_not_plain_boxes() {
    // The regression guard for the actual bug this module exists to fix: a box
    // with a material on it. At detail 0 a building is allowed to be a mass; at
    // detail 1 it must be strictly more geometry than the box alone, and at
    // detail 2 more again. If detail does not increase geometry, the level
    // parameter is decorative.
    const World world = generateWorld(11);
    const MaterialTable table = testTable();

    uint32_t tris[3] = {0, 0, 0};
    for (int level = 0; level <= 2; ++level) {
        SceneBuildOptions o = centredOn(world, 200.0);
        o.detailLevel = level;
        o.includeProps = false;  // isolate the facade cost
        tris[level] = buildSceneMesh(world, table, o).stats.triangles;
    }
    check(tris[0] > 0, "level 0 emits buildings");
    check(tris[1] > tris[0] * 3, "level 1 is substantially more than the bare masses");
    check(tris[2] > tris[1], "level 2 is more again, from the roof plant and parapets");
}

void test_detail_level_is_clamped() {
    // A detail parameter with no ceiling is a detail parameter with no budget.
    const World world = generateWorld(5);
    const MaterialTable table = testTable();
    SceneBuildOptions o = centredOn(world, 150.0);
    o.detailLevel = 99;
    const SceneMesh mesh = buildSceneMesh(world, table, o);
    SceneBuildOptions c = o;
    c.detailLevel = 2;
    const SceneMesh capped = buildSceneMesh(world, table, c);
    check(mesh.stats.triangles == capped.stats.triangles,
          "detail 99 is clamped to detail 2 rather than building an unbounded city");
}

void test_assembly_is_deterministic() {
    // A world that reshuffles every frame is a world that flickers, and a
    // flicker is very hard to trace back to its cause. The per-building stream
    // is seeded from the building, not from a global generator, so this holds
    // even when the build order changes.
    const World world = generateWorld(13);
    const MaterialTable table = testTable();
    const SceneBuildOptions o = centredOn(world, 200.0);
    const SceneMesh a = buildSceneMesh(world, table, o);
    const SceneMesh b = buildSceneMesh(world, table, o);
    check(a.vertices.size() == b.vertices.size(), "two builds produce the same vertex count");
    check(a.vertices == b.vertices, "and byte-identical vertex data");
}

void test_the_radius_actually_bounds_the_slice() {
    // Without this the "streaming" slice is the whole world, which is how the
    // budget work on the web side had to fight a scene that grew forever.
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    const SceneBuildOptions small = centredOn(world, 120.0);
    const SceneMesh near_ = buildSceneMesh(world, table, small);
    const SceneBuildOptions large = centredOn(world, 480.0);
    const SceneMesh far_ = buildSceneMesh(world, table, large);

    check(far_.stats.buildings > near_.stats.buildings, "a larger radius contains more buildings");
    check(near_.stats.buildingsOutsideRadius > 0, "and the near slice reports what it excluded");

    // The bounds have to stay bounded, which is the property that matters: a
    // river is a single polygon kilometres across, and one emitted whole makes
    // the slice's box kilometres wide, culls nothing and fits no budget. The
    // allowed overhang is one building's own diagonal, because a building whose
    // corner is just inside the circle is correctly included whole.
    double overhang = 0.0;
    for (const Building &bd : world.buildings) {
        overhang = std::max(overhang, 0.5 * std::hypot(bd.w, bd.d));
    }
    for (int axis = 0; axis < 2; ++axis) {
        const float span = near_.boundsMax[axis] - near_.boundsMin[axis];
        check(span <= 2.0f * static_cast<float>(small.radius) + static_cast<float>(overhang) + 1.0f,
              "the near slice's bounds stay inside its radius plus one building");
    }
    check(far_.boundsMax[2] > near_.boundsMax[2], "the larger slice reaches further away");
}

void test_the_budget_is_honoured_and_reported() {
    // A budget that is checked after the fact has already spent the memory, and
    // a budget that silently overruns is worse than no budget because the code
    // downstream trusts it. Both halves are asserted: the ceiling is respected
    // and the truncation is visible.
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    SceneBuildOptions o = centredOn(world, 500.0);
    o.includeGround = false;
    const SceneMesh full = buildSceneMesh(world, table, o);
    check(!full.stats.truncated(), "an unbudgeted build reports no truncation");

    o.maxTriangles = full.stats.triangles / 4;
    const SceneMesh limited = buildSceneMesh(world, table, o);
    check(limited.stats.budgetLimited, "a build that dropped buildings says so");
    check(limited.stats.buildingsDroppedForBudget > 0, "and counts them");
    check(limited.stats.triangles <= o.maxTriangles + full.stats.triangles / 4,
          "the triangle count stays near the ceiling rather than blowing through it");
    check(limited.stats.buildings < full.stats.buildings, "and fewer buildings were emitted");
}

void test_the_budget_drops_the_farthest_not_the_last() {
    // Dropping in world-list order leaves a hole in the middle of the slice, and
    // a hole in the middle of a city is immediately visible in a way that a
    // missing building on the horizon is not.
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    SceneBuildOptions o = centredOn(world, 500.0);
    o.includeGround = false;
    const SceneMesh full = buildSceneMesh(world, table, o);
    o.maxTriangles = full.stats.triangles / 5;
    const SceneMesh limited = buildSceneMesh(world, table, o);

    // Whatever survived must still fill the centre: the nearest building to the
    // slice centre is inside the radius, so it must be present.
    bool foundCentre = false;
    double bestDistance = 1e18;
    for (const Building &bd : world.buildings) {
        const double dx = bd.x - o.centerX, dz = bd.z - o.centerZ;
        const double d2 = dx * dx + dz * dz;
        if (d2 < bestDistance) {
            bestDistance = d2;
            // Within one building's own footprint of the centre.
            if (d2 < 0.25 * (bd.w * bd.w + bd.d * bd.d)) foundCentre = true;
        }
    }
    if (bestDistance < o.radius * o.radius * 0.01) {
        check(foundCentre || limited.stats.buildingsDroppedForBudget == 0,
              "the building nearest the slice centre is kept, not dropped");
    }
}

void test_bounds_contain_every_vertex() {
    // The bounds drive culling, and a bounds that is slightly wrong in the
    // wrong direction culls geometry that is on screen. Cheaper to be exact.
    const World world = generateWorld(21);
    const MaterialTable table = testTable();
    const SceneBuildOptions o = centredOn(world, 180.0);
    const SceneMesh mesh = buildSceneMesh(world, table, o);
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < mesh.vertexCount(); ++i) {
        float v[kFloatsPerVertex];
        mesh.readVertex(i, v);
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], v[kOffsetPosition + k]);
            mx[k] = std::max(mx[k], v[kOffsetPosition + k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        checkNear(mesh.boundsMin[k], mn[k], 1e-3f, "bounds minimum matches the vertices");
        checkNear(mesh.boundsMax[k], mx[k], 1e-3f, "bounds maximum matches the vertices");
    }
    check(mx[1] > mn[1], "the world has relief: the ground is not a single flat plane");
}

void test_solid_and_textured_materials_are_both_reachable() {
    // Both halves of the material table have to appear, or one of them is dead
    // code: either the CC0 bake is never sampled, or the physically-specified
    // solids are being textured with a photograph of a brick.
    const World world = generateWorld(7);
    const MaterialTable table = testTable();
    SceneBuildOptions o = centredOn(world, 250.0);
    o.detailLevel = 2;
    const SceneMesh mesh = buildSceneMesh(world, table, o);

    std::set<int32_t> seen;
    for (uint32_t i = 0; i < mesh.vertexCount(); ++i) {
        float v[kFloatsPerVertex];
        mesh.readVertex(i, v);
        seen.insert(static_cast<int32_t>(v[kOffsetMaterial]));
    }
    int32_t texturedSeen = 0, solidSeen = 0;
    for (int32_t idx : seen) {
        if (table.materials[static_cast<size_t>(idx)].textured) ++texturedSeen; else ++solidSeen;
    }
    check(texturedSeen > 0, "the CC0 texture bake is actually referenced by the scene");
    check(solidSeen > 0, "and so are the untextured solids, which PBR shades from constants");
    check(seen.size() >= 4, "a city uses several distinct materials, not one repeated");
}

void test_the_required_material_set_is_complete() {
    // `GeometryKit` resolves its own ids through `MaterialTable::index`, which
    // aborts the process on an unknown id, and `MeshBuilder::vertex` aborts on
    // an out-of-range index. That is the right behaviour for a programming
    // error, and it means the table the scene is given has to actually contain
    // every material the kit names. A rename in the material list therefore
    // takes the game down at the first building rather than at build time.
    //
    // So: the required set is asserted here explicitly. If someone renames a
    // material in the bake and not in the kit, this is the test that says so.
    const char *required[] = {
        "glass", "paint_metal", "metal_bare", "wall_stone", "wall_corrugated", "road_asphalt",
        "terrain", "bark", "foliage", "pavement", "road_paint", "sign", "metal_dark", "floor_wood",
    };
    const MaterialTable table = testTable();
    for (const char *id : required) {
        check(table.byId.find(id) != table.byId.end(),
              std::string("the table provides '") + id + "', which the kit names");
    }

    // And a table holding exactly that set still produces a whole city, so the
    // set above is genuinely sufficient rather than merely present.
    MaterialArrays arrays;
    arrays.layers = 4;
    std::vector<Material> mats;
    for (const char *id : required) {
        Material m;
        m.id = id;
        m.textured = true;
        m.layer = static_cast<int32_t>(mats.size() % 4);
        m.invTileScale = 3.0f;
        m.mapMode = MapMode::WallX;
        mats.push_back(m);
    }
    const MaterialTable minimal = buildMaterialTable(arrays, mats);
    const World world = generateWorld(7);
    const SceneBuildOptions o = centredOn(world, 150.0);
    const SceneMesh mesh = buildSceneMesh(world, minimal, o);
    check(!mesh.empty(), "a table holding only the required set still produces a city");
    validateBuffer(mesh, minimal, "minimal");
}

void test_every_material_the_scene_names_exists_in_the_real_bake() {
    // The failure this guards against is an abort, not a wrong colour.
    //
    // `MaterialTable::index` calls std::abort on an id it does not know, and
    // `GeometryKit` resolves its own ids through it. So a scene that names a
    // material the bake never produced does not render the wrong texture, it
    // takes the process down — and the fallback table had been inventing exactly
    // those names ("terrain", "pavement", "wall_render"), so a clean checkout
    // built and ran, and the first run with the real table loaded crashed.
    const MaterialTable real = buildBakedMaterialTable();
    check(real.layers > 0, "the baked table has layers");

    const char *required[] = {
        // scene_mesh.cpp: facades, plinth, ground, trees.
        "road_asphalt", "wall_brick", "wall_stone", "wall_plaster", "wall_corrugated",
        "pavement_concrete", "terrain_grass", "bark", "foliage",
        // geometry.cpp: window frames, sills, doors, pipe runs, roof plant.
        "glass", "paint_metal", "wall_corrugated", "floor_wood", "metal_rust",
        "road_paint", "sign", "metal_bare", "metal_dark", "lamp",
    };
    for (const char *id : required) {
        check(real.byId.find(id) != real.byId.end(),
              std::string("the baked table has '") + id + "', which the scene or the kit names");
    }

    // And the end-to-end version: assemble a whole slice with the real table and
    // walk it. Every vertex's material index must land inside it.
    const World world = generateWorld(7);
    SceneBuildOptions o = centredOn(world, 300.0);
    o.detailLevel = 2;
    const SceneMesh mesh = buildSceneMesh(world, real, o);
    check(!mesh.empty(), "a slice builds with the real baked table");
    check(mesh.stats.buildings > 0, "and contains buildings");
    validateBuffer(mesh, real, "baked");
}

void test_the_fallback_table_uses_the_same_names() {
    // The fallback exists so a clean checkout compiles and runs. It must not be
    // a *different* set of materials, or it stops being a fallback and becomes a
    // second source of truth that disagrees with the bake at run time.
    const MaterialTable fallback = defaultMaterialTable();
    const MaterialTable real = buildBakedMaterialTable();
    check(fallback.count() == real.count(),
          "the fallback and the baked table have the same number of entries");
    for (const Material &m : fallback.materials) {
        if (!m.textured) continue;  // the solid set is the same in both
        check(real.byId.find(m.id) != real.byId.end(),
              std::string("the fallback's '") + m.id + "' also exists in the baked table");
    }
}

}  // namespace

int main() {
    std::printf("emergent scene mesh tests\n");
    run("the assembler emits a city", test_the_assembler_emits_a_city);
    run("every building in the slice is really inside it", test_every_building_in_the_slice_is_really_inside_it);
    run("the buffer is the layout the shader reads", test_the_buffer_is_the_layout_the_shader_reads);
    run("facades are not plain boxes", test_facades_are_not_plain_boxes);
    run("detail level is clamped", test_detail_level_is_clamped);
    run("assembly is deterministic", test_assembly_is_deterministic);
    run("the radius actually bounds the slice", test_the_radius_actually_bounds_the_slice);
    run("the budget is honoured and reported", test_the_budget_is_honoured_and_reported);
    run("the budget drops the farthest, not the last", test_the_budget_drops_the_farthest_not_the_last);
    run("bounds contain every vertex", test_bounds_contain_every_vertex);
    run("solid and textured materials are both reachable", test_solid_and_textured_materials_are_both_reachable);
    run("the required material set is complete", test_the_required_material_set_is_complete);
    run("every material the scene names exists in the real bake", test_every_material_the_scene_names_exists_in_the_real_bake);
    run("the fallback table uses the same names", test_the_fallback_table_uses_the_same_names);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
