#version 450

// EMERGENT scene vertex shader.
//
// The native counterpart of the browser's `sceneVS`, and deliberately the
// same program: same vertex layout, same material-table lookup, same box
// mapping with the same MAP_MODE branches, same UV escape for imported meshes.
// Two shaders for one game is how the two halves quietly stop being the same
// game.
//
// Vertex layout, 48 bytes:
//   0  vec3  position
//   12 vec3  normal
//   24 vec3  colour
//   36 float  material index
//   40 vec2  uv

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColour;
layout(location = 3) in float inMaterial;
layout(location = 4) in vec2 inUv;

// 96 bytes. The Vulkan guaranteed minimum for maxPushConstantsSize is 128, and
// the limit is commonly 128 or 256 on real drivers, so this block is sized to
// fit the guarantee rather than the machine it was written on. A `mat4 view`
// used to sit here too, unused by both stages, and on its own it was the
// difference between fitting and not.
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec3 cameraPosition;
    float time;
    float water;
} pc;

// The material table, two vec4s per material, baked in at compile time from
// kMaxMaterials so the uniform array and the table can never disagree about
// their size.
//
// It is a uniform buffer and *not* push constants, which is the whole reason
// this comment exists. At 64 materials the table is 2 KB; the guaranteed
// minimum for maxPushConstantsSize is 128 bytes, and plenty of drivers cap at
// 128 or 256. A material table in push constants does not render slightly
// wrong, it fails pipeline creation — or worse, passes on the one machine whose
// limit happens to be high and fails on every other one.
layout(set = 1, binding = 1) uniform MaterialTable {
    vec4 materialA[EMERGENT_MAX_MATERIALS];
    vec4 materialB[EMERGENT_MAX_MATERIALS];
    int materialCount;
    int pad0;
    int pad1;
    int pad2;
} materials;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColour;
layout(location = 2) out float vViewDistance;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out vec2 vUv;
layout(location = 5) flat out int vMaterial;
layout(location = 6) out float vUseUv;

void main() {
    vec3 p = inPosition;
    if (pc.water > 0.5) {
        p.y += sin(p.x * 0.025 + pc.time * 1.6) * 0.08 + cos(p.z * 0.021 + pc.time) * 0.06;
    }
    gl_Position = pc.viewProjection * vec4(p, 1.0);

    vNormal = inNormal;
    vColour = inColour;
    vWorldPos = p;
    vViewDistance = length(p - pc.cameraPosition);
    // flat, so the material index is not interpolated across a triangle that
    // straddles two materials. Interpolating it produces a layer index between
    // two valid ones, which samples a third material that does not exist.
    vMaterial = int(inMaterial + 0.5);

    // Box mapping. The plane comes from the material's recorded role rather
    // than from the normal, so a road tiles as ground and a facade tiles as a
    // wall even where a chamfer or a pitched roof made the normal ambiguous.
    // World-locked, so a brick wall's bricks stay put while the player walks
    // past.
    //
    // MapMode::Uv is the exception: an imported mesh brings its own coordinates
    // because no world-axis projection puts a wood grain along a tapered leg.
    float mode = materials.materialB[vMaterial].x;
    vUseUv = step(3.5, mode);
    vec2 proj = p.xz;
    if (mode > 0.5 && mode < 1.5) {
        proj = p.zy;
    } else if (mode > 1.5 && mode < 2.5) {
        proj = p.xy;
    }
    vUv = mix(proj * materials.materialA[vMaterial].y, inUv, vUseUv);
}
