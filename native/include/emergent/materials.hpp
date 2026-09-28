// The EMERGENT material system, in C++.
//
// A port of `materials.mjs`, and the same design: one table, two kinds of
// entry, one uniform layout, one shader. Every vertex the renderer draws
// carries a material index, and the shader resolves that index into a texture
// layer, a tiling scale, a roughness and a metalness. There is no second code
// path and no per-surface shader.
//
// The two entry kinds are the same as in the JavaScript:
//
//   Textured  from the bake. 26 curated CC0 PBR materials from Poly Haven,
//             resampled and packed into three 2D array textures.
//   Solid     everything a photograph has no business covering: window glass,
//             painted steel, rubber, foliage, road markings, lamp glass. A PBR
//             shader shades these correctly with a constant, and a painted car
//             body is a dielectric with a roughness, not an image.
//
// ## Texture coordinates
//
// Static world geometry is axis-aligned, so a surface's coordinate is a
// projection of its world position onto the plane its normal points out of —
// box mapping. It has no seams, needs no unwrapping, and is world-locked, so a
// brick wall's bricks stay put while the player walks past instead of
// shimmering.
//
// It is wrong for anything that moves, so moving geometry uses solid materials.
// And it is wrong for imported meshes, which carry authored UVs and are not
// aligned to any world axis; those use `MapMode::Uv` and the vertex's own
// coordinate, with a derivative-based tangent frame.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace emergent {

/** How the fragment shader derives a texture coordinate. */
enum class MapMode : int32_t {
    /** Project onto world XZ. Ground, roads, roofs. */
    Ground = 0,
    /** Project onto world ZY. Walls whose normal points along X. */
    WallX = 1,
    /** Project onto world XY. Walls whose normal points along Z. */
    WallZ = 2,
    /** No texture. Vertex colour only. */
    Solid = 3,
    /** Use the vertex's own UV. Imported meshes only. */
    Uv = 4,
};

/** The uniform array size. Baked into the shader so the two cannot disagree. */
inline constexpr int kMaxMaterials = 64;

struct Material {
    std::string id;
    bool textured = false;
    int32_t layer = -1;
    float invTileScale = 0.0f;
    float roughness = 0.7f;
    float metallic = 0.0f;
    MapMode mapMode = MapMode::Solid;
    float normalStrength = 0.0f;
    float emissive = 0.0f;
};

/** The three texture arrays the scene shader binds. */
struct MaterialArrays {
    int32_t layers = 0;
    int32_t size = 512;
    std::string albedo[8];
    std::string normal[8];
    std::string arm[8];
    /** Source resolution, which is 8k. The runtime resamples. */
    int32_t sourceResolution = 8192;
};

/** The merged table, ready to upload as uniform arrays. */
struct MaterialTable {
    std::vector<Material> materials;
    std::unordered_map<std::string, int32_t> byId;

    /** Four vec4s per material; see the layout in the shader. */
    std::vector<float> a;  // layer, invTileScale, roughness, metallic
    std::vector<float> b;  // mapMode, normalStrength, emissive, textured

    int32_t layers = 0;
    int32_t size = 512;

    int32_t count() const { return static_cast<int32_t>(materials.size()); }

    /** Look up a material, failing loudly. A silent default is a grey surface. */
    int32_t index(const std::string &id) const;
    void finalize();
};

/**
 * An untextured PBR surface.
 *
 * The roughness and metallic values are the physically right ones, not taste:
 * a painted metal car body is metallic 0 because the paint is an oxide, glass
 * is metallic 0 at roughness 0.05, and bare galvanised steel is metallic 1.
 */
struct SolidMaterialSpec {
    const char *id;
    float roughness;
    float metallic;
    float r, g, b;
    float emissive;
    const char *note;
};

/** The solid set, in table order. */
const std::vector<SolidMaterialSpec> &solidMaterialSpecs();

/**
 * Build the merged table from the bake descriptor and the solid set.
 *
 * @param arrays The baked texture arrays, which supply the layer count.
 * @param textureMaterials Ids of the baked materials, with their layers.
 */
MaterialTable buildMaterialTable(const MaterialArrays &arrays,
                                 const std::vector<Material> &textureMaterials);

/**
 * The table the renderer starts with, before any bake is loaded.
 *
 * The honest fallback, not the asset: the solid set with real roughness and
 * metalness, plus one texture layer per id the scene and the geometry kit name,
 * so the shader's array indexing is always in range. Every textured material
 * resolves to a layer holding a neutral placeholder, so a wall is correctly lit
 * and correctly fogged but flat grey until the CC0 bake is uploaded over it.
 *
 * It exists so that loading the bake is a *replacement* rather than a
 * precondition. A renderer that draws nothing until the asset pipeline is wired
 * up is a renderer that looks broken, and the breakage is easy to mistake for a
 * shader bug.
 */
MaterialTable defaultMaterialTable();

/**
 * The one table the whole process agrees on.
 *
 * A function-local static rather than a parameter, because the alternative is
 * two tables: the renderer uploads one and the scene assembler names materials
 * against the other, and nothing reports the mismatch until a wall is the wrong
 * grey. `setSceneMaterials` replaces the contents, and the next slice is
 * assembled against the replacement.
 */
MaterialTable &sharedMaterialTable();

/**
 * The table built from the packed CC0 bake, or the untextured fallback.
 *
 * Reads `materials.gen.hpp`, which `npm run assets:textures` generates from the
 * bake descriptor. Returns the fallback when that header has not been built, so
 * a clean checkout configures and compiles.
 */
MaterialTable buildBakedMaterialTable();

/**
 * The vertex layout every renderer buffer uses.
 *
 *   position(3) normal(3) colour(3) material(1) uv(2) = 12 floats, 48 bytes.
 *
 * The UV pair exists because imported models need authored coordinates and box
 * mapping cannot supply them. It is the last two floats rather than a fifth
 * attribute so the stride stays one contiguous read.
 */
inline constexpr int kFloatsPerVertex = 12;
inline constexpr int kVertexBytes = kFloatsPerVertex * 4;

inline constexpr int kOffsetPosition = 0;
inline constexpr int kOffsetNormal = 3;
inline constexpr int kOffsetColour = 6;
inline constexpr int kOffsetMaterial = 9;
inline constexpr int kOffsetUv = 10;

}  // namespace emergent
