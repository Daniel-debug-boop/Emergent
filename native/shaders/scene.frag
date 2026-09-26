#version 450

// Flat-shaded debug fragment stage.
//
// No lighting: there are no normals in the vertex format, and inventing them
// for a debug view would mean shipping a normal pipeline that nothing else
// uses. Depth is carried from the vertex stage and turned into fog, which is
// enough to read depth ordering and silhouette without any of that.

layout(location = 0) in vec4 fragmentColor;
layout(location = 1) in float fragmentDepth;

layout(location = 0) out vec4 outColor;

void main() {
    // Matches the clear colour in VulkanRenderBackend, so geometry fades into
    // the background instead of into a different colour than the sky.
    const vec3 fogColor = vec3(0.025, 0.045, 0.09);
    // 18 to 95 units: near enough to keep a walked-to prop crisp, far enough
    // that the far side of the 24-unit prop ring recedes.
    float fog = clamp((fragmentDepth - 18.0) / 77.0, 0.0, 1.0);
    outColor = vec4(mix(fragmentColor.rgb, fogColor, fog), fragmentColor.a);
}
