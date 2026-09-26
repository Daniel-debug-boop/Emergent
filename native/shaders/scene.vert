#version 450

// EMERGENT scene shader.
//
// Draws the frame loop's output and nothing else: the scene's boxes at their
// interpolated positions, and the resolved ozz skeleton as joint boxes read
// straight from the pose matrices. It is a debug presentation, not a game
// renderer -- its whole purpose is to make a wrong alpha or a stale joint
// matrix something you can see instead of something you have to infer.
//
// Everything arrives through two storage buffers that are rewritten once per
// frame, so there is no per-object descriptor churn and no dynamic indexing
// into a uniform array.

layout(location = 0) in vec3 inPosition;  // unit cube, -0.5..0.5
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    // The character's world transform. The pose matrices are model-space for
    // the rig only, so without this the skeleton would be drawn at the origin
    // while the body walked away from it.
    mat4 character;
} pc;

// Four vec4s per instance. A power-of-two stride keeps the offset arithmetic
// a shift rather than a multiply, which matters when this is indexed by
// gl_InstanceIndex in the vertex shader for every vertex of every box.
struct Instance {
    vec4 center;   // xyz = world centre, unused for joint instances
    vec4 half;     // xyz = half extents
    vec4 color;    // rgba
    vec4 meta;     // x = joint index, y = 1 when this instance is a joint
};

layout(std430, binding = 0) readonly buffer InstanceBuffer {
    Instance instances[];
} instanceBuffer;

layout(std430, binding = 1) readonly buffer JointBuffer {
    mat4 joints[];
} jointBuffer;

layout(location = 0) out vec4 fragmentColor;
layout(location = 1) out float fragmentDepth;

void main() {
    int index = gl_InstanceIndex;
    Instance inst = instanceBuffer.instances[index];

    vec3 local = inPosition * inst.half.xyz;
    vec3 world;

    if (inst.meta.y > 0.5) {
        // Joints are placed by the pose matrix alone: it already carries the
        // joint's model-space translation, so adding a centre would double it.
        world = (pc.character * jointBuffer.joints[int(inst.meta.x)] * vec4(local, 1.0)).xyz;
    } else {
        world = local + inst.center.xyz;
    }

    vec4 clip = pc.viewProjection * vec4(world, 1.0);
    gl_Position = clip;

    fragmentColor = inst.color * inColor;
    // Perspective-correct depth handed to the fragment stage, so the fog
    // below is linear in view space rather than smeared across a quad.
    fragmentDepth = clip.w;
}
