#version 450

// EMERGENT scene fragment shader.
//
// The native counterpart of the browser's `sceneFS`, and the same shading
// model: a GGX + Smith microfacet BRDF resolving the material index through the
// uniform table, sampling albedo, normal and packed AO/roughness/metalness from
// three 2D array textures, with a hemispherical sky/ground ambient and an
// environment term through a roughness-aware Fresnel.
//
// The model is not decoration. A Lambert term is what made the first version of
// this game look like untextured boxes, and a Lambert term on top of real
// textures still looks like untextured boxes, just with noise.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColour;
layout(location = 2) in float vViewDistance;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in vec2 vUv;
layout(location = 5) flat in int vMaterial;
layout(location = 6) in float vUseUv;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 view;
    vec3 cameraPosition;
    float time;
    float water;
    vec4 materialA[EMERGENT_MAX_MATERIALS];
    vec4 materialB[EMERGENT_MAX_MATERIALS];
    int materialCount;
    int pad0;
    int pad1;
    int pad2;
} pc;

// The three material arrays. Units 0, 1 and 2, bound once per frame — a unit
// assignment is a property of the pipeline, and setting it every frame is a way
// for a rebind elsewhere to quietly point the shader at the wrong array.
layout(set = 0, binding = 0) uniform sampler2DArray albedoArray;
layout(set = 0, binding = 1) uniform sampler2DArray normalArray;
layout(set = 0, binding = 2) uniform sampler2DArray armArray;

layout(set = 1, binding = 0) uniform SceneUniforms {
    vec3 sunDirection;
    vec3 sunColor;
    vec3 skyColor;
    vec3 groundColor;
    vec3 fogColor;
    float fogDensity;
    float night;
    float materialsReady;
} scene;

layout(location = 0) out vec4 outColour;

const float PI = 3.14159265359;

float distributionGGX(float NoH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float geometrySmith(float NoV, float NoL, float roughness) {
    float k = roughness * 0.5;
    return (NoV / (NoV * (1.0 - k) + k)) * (NoL / (NoL * (1.0 - k) + k));
}

vec3 fresnel(float u, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - u, 0.0, 1.0), 5.0);
}

void main() {
    if (vMaterial < 0 || vMaterial >= pc.materialCount) {
        // A material index the table does not cover would index out of bounds
        // on a uniform array, and Vulkan's behaviour for an out-of-bounds
        // dynamic index on a non-robust buffer access is undefined. Rejecting
        // here turns that into a magenta surface, which is diagnosable.
        outColour = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    vec4 A = pc.materialA[vMaterial];
    vec4 B = pc.materialB[vMaterial];

    // Face the normal at the eye. Without this, a box corner built from two
    // opposing faces shades both of them identically and every solid reads flat.
    vec3 Ng = normalize(vNormal);
    vec3 V = normalize(pc.cameraPosition - vWorldPos);
    vec3 N = dot(Ng, V) < 0.0 ? -Ng : Ng;
    vec3 L = normalize(scene.sunDirection);

    vec3 albedo = vColour;
    float roughness = A.z;
    float metallic = A.w;
    float ao = 1.0;
    float emissive = B.z;

    if (B.w > 0.5 && scene.materialsReady > 0.5) {
        int layer = int(A.x);
        vec3 uvw = vec3(vUv, float(layer));
        vec4 alb = texture(albedoArray, uvw);
        vec4 arm = texture(armArray, uvw);
        vec3 nt = texture(normalArray, uvw).xyz * 2.0 - 1.0;

        vec3 T, Bt;
        if (vUseUv > 0.5) {
            // Screen-space derivatives of position and UV give the tangent frame
            // for an arbitrary mesh without storing tangents, which is the
            // standard technique and costs two derivatives instead of four
            // floats a vertex. Degenerate when a triangle is edge-on or has a
            // mirrored UV island, so it falls back to an arbitrary perpendicular
            // rather than a NaN.
            vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
            vec2 du1 = dFdx(vUv), du2 = dFdy(vUv);
            float det = du1.x * du2.y - du1.y * du2.x;
            vec3 Tt = abs(det) > 1e-12 ? (dp1 * du2.y - dp2 * du1.y) / det : vec3(0.0);
            T = normalize(Tt - N * dot(N, Tt));
            if (dot(T, T) < 0.5) {
                T = normalize(cross(abs(N.y) > 0.7 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), N));
            }
        } else {
            // A box-projected surface's tangent is one world axis, derived from
            // the same plane the coordinate came from. No tangents in the vertex
            // format.
            T = normalize(cross(abs(N.y) > 0.7 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), N));
        }
        Bt = cross(N, T);
        nt.xy *= B.y;
        albedo *= alb.rgb;
        ao = arm.r;
        roughness = clamp(arm.g, 0.04, 1.0);
        metallic = arm.b;
        N = normalize(mat3(T, Bt, N) * nt);
    }

    // Dielectric reflectance is about 4%; a conductor takes its reflectance
    // from its own colour. Getting this wrong is the most common way a PBR
    // scene ends up looking like plastic.
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseColour = albedo * (1.0 - metallic);
    float NoV = max(dot(N, V), 1e-4);

    vec3 direct = vec3(0.0);
    float NoL = dot(N, L);
    if (NoL > 0.0) {
        vec3 H = normalize(L + V);
        float NoH = max(dot(N, H), 0.0);
        float VoH = max(dot(V, H), 0.0);
        float a = roughness * roughness;
        vec3 spec = fresnel(VoH, f0) *
                    (distributionGGX(NoH, roughness) * geometrySmith(NoV, NoL, roughness) /
                     max(4.0 * NoV * NoL, 1e-5));
        direct = (diffuseColour / PI + spec) * scene.sunColor * NoL;
    }

    // Hemispherical ambient: a two-colour sky/ground irradiance. Not an IBL,
    // but it is what keeps a shaded wall off flat black and gives a rough
    // surface something to reflect, and it costs two uniforms.
    vec3 ambientIrradiance = mix(scene.groundColor, scene.skyColor, N.y * 0.5 + 0.5);
    vec3 R = reflect(-V, N);
    vec3 env = mix(scene.groundColor, scene.skyColor, R.y * 0.5 + 0.5);
    vec3 ambient = diffuseColour * ambientIrradiance * ao +
                   env * fresnel(NoV, f0) * (1.0 - roughness * 0.8) * ao * mix(0.30, 1.0, metallic);

    vec3 colour = direct + ambient + albedo * emissive * (0.25 + scene.night * 0.95);
    if (pc.water > 0.5) colour = mix(colour, vec3(0.08, 0.25, 0.35), 0.42);

    // Exponential-squared fog, matched to the browser's
    // `1 - exp(-d*d*density)`.
    float fog = 1.0 - exp(-vViewDistance * vViewDistance * scene.fogDensity);
    colour = mix(colour, scene.fogColor, fog);

    outColour = vec4(colour, 1.0);
}
