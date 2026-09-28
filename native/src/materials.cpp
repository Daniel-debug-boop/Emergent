// The material table. See `materials.hpp` for the design.

#include "emergent/materials.hpp"

// The generated bake table. Present only after `npm run assets:textures`; the
// fallback below is what makes that a build-output dependency rather than a
// source dependency.
#if __has_include("materials.gen.hpp")
#include "materials.gen.hpp"
#define EMERGENT_HAVE_BAKED_MATERIALS 1
#endif

#include <cstdio>
#include <cstdlib>

namespace emergent {

int32_t MaterialTable::index(const std::string &id) const {
    auto it = byId.find(id);
    if (it == byId.end()) {
        // A typo here renders as the default solid, which looks almost right —
        // so it is an error rather than a silent fallback. That is the whole
        // reason this is a function and not an operator[].
        std::fprintf(stderr, "material: unknown id '%s'\n", id.c_str());
        std::fprintf(stderr, "  known: ");
        for (const auto &kv : byId) std::fprintf(stderr, "%s ", kv.first.c_str());
        std::fprintf(stderr, "\n");
        std::abort();
    }
    return it->second;
}

void MaterialTable::finalize() {
    a.assign(static_cast<size_t>(kMaxMaterials) * 4, 0.0f);
    b.assign(static_cast<size_t>(kMaxMaterials) * 4, 0.0f);
    for (size_t i = 0; i < materials.size(); i++) {
        const Material &m = materials[i];
        a[i * 4 + 0] = static_cast<float>(m.layer);
        a[i * 4 + 1] = m.invTileScale;
        a[i * 4 + 2] = m.roughness;
        a[i * 4 + 3] = m.metallic;
        b[i * 4 + 0] = static_cast<float>(static_cast<int32_t>(m.mapMode));
        b[i * 4 + 1] = m.normalStrength;
        b[i * 4 + 2] = m.emissive;
        b[i * 4 + 3] = m.textured ? 1.0f : 0.0f;
    }
    for (const auto &m : materials) byId[m.id] = static_cast<int32_t>(&m - materials.data());
}

const std::vector<SolidMaterialSpec> &solidMaterialSpecs() {
    // The same set, in the same order, as `SOLID_MATERIALS` in materials.mjs.
    // The order is load-bearing: it determines the table indices, and a table
    // index baked into a vertex buffer has to mean the same thing in both
    // implementations or the same mesh renders as a different material.
    static const std::vector<SolidMaterialSpec> specs = {
        {"glass", 0.06f, 0.0f, 0.55f, 0.68f, 0.75f, 0.0f,
         "Window and windscreen glass. Dark by default because a window is a hole, not a light."},
        {"glass_lit", 0.10f, 0.0f, 0.95f, 0.82f, 0.55f, 1.6f,
         "Interior light behind glass, driven up at night."},
        {"paint_metal", 0.28f, 0.0f, 0.5f, 0.5f, 0.52f, 0.0f,
         "Painted steel: poles, shutters, railings, containers. Paint is a dielectric."},
        {"metal_bare", 0.34f, 1.0f, 0.62f, 0.64f, 0.67f, 0.0f,
         "Galvanised or unpainted steel. The one place the metal path is driven by a constant."},
        {"metal_dark", 0.45f, 0.9f, 0.22f, 0.23f, 0.25f, 0.0f,
         "Dark structural steel: frames, brackets, exhausts."},
        {"rubber", 0.85f, 0.0f, 0.09f, 0.09f, 0.10f, 0.0f,
         "Tyres, seals, mats."},
        {"road_paint", 0.55f, 0.0f, 0.88f, 0.86f, 0.78f, 0.0f,
         "Lane and crossing markings. Worn, not white."},
        {"road_paint_yellow", 0.55f, 0.0f, 0.82f, 0.68f, 0.18f, 0.0f,
         "Bus stop boxes, loading bays, temporary works."},
        {"foliage", 0.62f, 0.0f, 0.42f, 0.58f, 0.26f, 0.0f,
         "Leaves and canopy. Never specular, which is why it is its own material."},
        {"foliage_dry", 0.70f, 0.0f, 0.56f, 0.52f, 0.30f, 0.0f, "Dead and autumn vegetation."},
        {"bark", 0.88f, 0.0f, 0.30f, 0.24f, 0.18f, 0.0f, "Trunks and branches."},
        {"skin", 0.55f, 0.0f, 0.72f, 0.55f, 0.44f, 0.0f, "Character skin. Slightly waxy, never shiny."},
        {"fabric", 0.82f, 0.0f, 0.5f, 0.5f, 0.5f, 0.0f, "Clothing. Fully rough, which is the whole point."},
        {"lamp", 0.15f, 0.0f, 1.0f, 0.94f, 0.80f, 3.0f, "Lit lamp glass and bulb."},
        {"sign", 0.35f, 0.0f, 0.85f, 0.30f, 0.22f, 0.9f, "Illuminated signage."},
        {"plastic", 0.40f, 0.0f, 0.6f, 0.6f, 0.6f, 0.0f, "Crates, bins, casings."},
        {"tarpaulin", 0.75f, 0.0f, 0.35f, 0.40f, 0.35f, 0.0f, "Sheeting, covers, awnings."},
    };
    return specs;
}

namespace {

/**
 * Which plane a textured material is projected onto.
 *
 * Derived from the role the asset database recorded rather than guessed per
 * surface, so a road tiles as ground even if something later places it on a
 * slope-facing quad.
 */
MapMode mapModeForRole(const std::string &role) {
    static const char *const groundish[] = {"road", "pavement", "street", "shoulder", "ground",
                                            "terrain", "beach", "hardstanding", "plaza"};
    for (const char *g : groundish) {
        if (role.find(g) != std::string::npos) return MapMode::Ground;
    }
    return MapMode::WallX;
}

}  // namespace

MaterialTable buildMaterialTable(const MaterialArrays &arrays,
                                 const std::vector<Material> &textureMaterials) {
    MaterialTable table;
    table.layers = arrays.layers;
    table.size = arrays.size;

    for (const Material &m : textureMaterials) {
        table.materials.push_back(m);
    }
    for (const SolidMaterialSpec &s : solidMaterialSpecs()) {
        Material m;
        m.id = s.id;
        m.textured = false;
        m.layer = -1;
        m.invTileScale = 0.0f;
        m.roughness = s.roughness;
        m.metallic = s.metallic;
        m.mapMode = MapMode::Solid;
        m.normalStrength = 0.0f;
        m.emissive = s.emissive;
        table.materials.push_back(m);
    }

    if (static_cast<int32_t>(table.materials.size()) > kMaxMaterials) {
        // The shader's uniform array is sized from kMaxMaterials at compile
        // time, so overflowing it truncates silently and every material past
        // the limit renders as row zero. It has to be a hard stop.
        std::fprintf(stderr, "material table overflow: %zu > %d\n", table.materials.size(), kMaxMaterials);
        std::abort();
    }
    table.finalize();
    return table;
}

MapMode mapModeFor(const std::string &role) { return mapModeForRole(role); }


MaterialTable defaultMaterialTable() {
    MaterialArrays arrays;
    arrays.layers = 26;
    arrays.size = 512;

    // The same 26 ids the bake produces, in the same order, so the untextured
    // fallback and the real table agree on what a material is *called*.
    //
    // They have to. The geometry kit and the scene assembler resolve ids through
    // `MaterialTable::index`, which aborts the process on an unknown one — so a
    // fallback that invented convenient names ("terrain", "pavement") would let a
    // clean checkout build and then crash the moment the real table arrived,
    // with the abort pointing at the material system rather than at the table
    // that changed underneath it.
    const char *texturedIds[] = {
        "road_asphalt", "road_asphalt_worn", "pavement_concrete", "pavement_pavers",
        "street_cobble", "ground_gravel",    "wall_brick",       "wall_brick_dark",
        "wall_plaster",  "wall_plaster_dirty", "wall_concrete",  "wall_painted_concrete",
        "wall_stone",    "wall_siding",      "wall_metal_shutter", "wall_corrugated",
        "metal_rust",    "metal_plate",      "terrain_grass",    "terrain_dirt",
        "terrain_mud",    "terrain_rock",    "terrain_sand",     "floor_concrete",
        "floor_wood",     "floor_tile",
    };
    std::vector<Material> textured;
    int32_t layer = 0;
    for (const char *id : texturedIds) {
        Material m;
        m.id = id;
        m.textured = true;
        m.layer = layer++;
        m.invTileScale = 3.0f;
        m.roughness = 0.8f;
        m.metallic = 0.0f;
        // The map mode is what makes a road tile as ground and a facade as a
        // wall, so the fallback has it right even though its texture is flat.
        const std::string name(id);
        m.mapMode = (name == "road_asphalt" || name == "terrain" || name == "pavement")
                        ? MapMode::Ground
                        : MapMode::WallX;
        textured.push_back(m);
    }
    return buildMaterialTable(arrays, textured);
}

MaterialTable buildBakedMaterialTable() {
#ifdef EMERGENT_HAVE_BAKED_MATERIALS
    // The 26 CC0 materials, in layer order, followed by the solids. Textured
    // entries first so a material index and a texture layer index are both small
    // and the shader's dynamic index into a 64-entry table stays predictable.
    MaterialArrays arrays;
    arrays.layers = kBakedLayers;
    arrays.size = kBakedSize;

    std::vector<Material> textured;
    textured.reserve(kBakedMaterialCount);
    for (int i = 0; i < kBakedMaterialCount; ++i) {
        const BakedMaterial &b = kBakedMaterials[i];
        Material m;
        m.id = b.id;
        m.textured = true;
        m.layer = b.layer;
        m.invTileScale = b.invTileScale;
        m.roughness = b.roughness;
        m.metallic = b.metallic;
        m.mapMode = b.mapMode;
        m.normalStrength = b.normalStrength;
        textured.push_back(m);
    }
    return buildMaterialTable(arrays, textured);
#else
    // The generated header has not been built. The solids alone still work, and
    // the scene still draws and lights correctly, with every surface untextured.
    // Failing here instead would mean `cmake --build` cannot run before an asset
    // pipeline, which is a worse trade than a grey world that says it is grey.
    return defaultMaterialTable();
#endif
}

MaterialTable &sharedMaterialTable() {
    static MaterialTable table = buildBakedMaterialTable();
    return table;
}

}  // namespace emergent
