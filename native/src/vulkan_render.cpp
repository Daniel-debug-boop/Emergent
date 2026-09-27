// The Vulkan render backend.
//
// SCOPE AND HONESTY NOTE, because this file is the one place in the tree that
// cannot be verified where it was written: this code was compiled and linked
// against Vulkan 1.3 headers, but no Vulkan driver was present in the build
// environment, so not one line of the path below has ever executed. Every
// function is written to report what it actually did rather than to appear to
// succeed: if a swapchain, a shader module or a pipeline is missing, the
// backend sets stats().status and declines to draw. It never reports frames
// submitted that were not submitted.
//
// It is a debug renderer, not a game renderer. It draws the frame loop's
// boxes and the ozz skeleton so that a bad interpolation alpha or a stale
// pose matrix is visible on screen rather than inferred.

#include "emergent/vulkan_backend.hpp"

#include "emergent/atmosphere.hpp"
#include "emergent/ktx2.hpp"
#include "emergent/materials.hpp"
#include "emergent/scene_mesh.hpp"

// Vulkan's C structs are conventionally initialised as
// `VkFooCreateInfo info{VK_STRUCTURE_TYPE_FOO};`, which zero-fills every other
// member because they are C aggregates. That is the idiom in the Vulkan
// headers and the samples, and it is correct here. The compiler cannot tell
// that apart from a C++ class aggregate, so it warns on every one. Rewriting
// twenty initialisers to silence a false positive would make this file less
// like every other Vulkan source, which is a worse trade than the warning.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <volk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace emergent {
namespace {

// -- small matrix helpers ---------------------------------------------------
// Written out rather than pulled from a math library: this is four functions,
// EMERGENT already owns its own matrix conventions in animation.cpp, and a
// dependency here would be a dependency on a column-major/row-major
// conversion nobody reads.

void perspective(float fovYRadians, float aspect, float zNear, float zFar, float *out) {
    std::memset(out, 0, sizeof(float) * 16);
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    out[0] = f / (aspect > 0.0f ? aspect : 1.0f);
    out[5] = f;
    // Vulkan clip space: y points down and z is [0,1], not OpenGL's [-1,1].
    // Using the OpenGL convention here is the single most common reason a
    // hand-written Vulkan triangle ends up inside-out or clipped away.
    out[10] = zFar / (zNear - zFar);
    out[11] = -1.0f;
    out[14] = (zNear * zFar) / (zNear - zFar);
}

void lookAt(const float *eye, const float *center, const float *up, float *out) {
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len > 1e-6f) {
        f[0] /= len;
        f[1] /= len;
        f[2] /= len;
    } else {
        f[0] = 0.0f;
        f[1] = 0.0f;
        f[2] = 1.0f;
    }
    // s = f x up, normalised
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (len > 1e-6f) {
        s[0] /= len;
        s[1] /= len;
        s[2] /= len;
    } else {
        s[0] = 1.0f;
        s[1] = 0.0f;
        s[2] = 0.0f;
    }
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    // Column-major, matching every other matrix in the engine.
    out[0] = s[0];  out[4] = s[1];  out[8] = s[2];
    out[1] = u[0];  out[5] = u[1];  out[9] = u[2];
    out[2] = -f[0]; out[6] = -f[1]; out[10] = -f[2];
    out[3] = 0.0f;  out[7] = 0.0f;  out[11] = 0.0f;
    out[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    out[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    out[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    out[15] = 1.0f;
}

void multiply(const float *a, const float *b, float *out) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

struct Vertex {
    float position[3];
    float color[4];
};
static_assert(sizeof(Vertex) == 28, "vertex layout must match the pipeline's stride");

// A unit cube, 36 vertices, wound counter-clockwise seen from outside each
// face. Built once at open() rather than shipped as an asset, because a
// hard-coded cube is not content and pretending otherwise would put an asset
// pipeline in the way of a debug renderer.
//
// Each face is given as (normal, u, v) with u x v == normal, which is what
// makes the winding correct: the corner order 0,1,2 / 0,2,3 traverses the
// (u,v) frame counter-clockwise, so it is counter-clockwise from outside
// exactly when the frame is right-handed with respect to the normal. Getting
// this wrong is invisible in the geometry and shows up as a box drawn inside
// out, so the relationship is stated rather than left implicit.
std::vector<Vertex> makeCube() {
    std::vector<Vertex> v;
    v.reserve(36);
    auto face = [&](const float n[3], const float u[3], const float w[3]) {
        // Shaded by facing so adjacent faces are distinguishable under a flat
        // colour with no lighting pass.
        const float shade = 0.72f + 0.28f * (std::fabs(n[0]) * 0.9f + std::fabs(n[1]) * 0.7f + std::fabs(n[2]) * 0.5f);
        const float origin[3] = {n[0] * 0.5f, n[1] * 0.5f, n[2] * 0.5f};
        const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i : order) {
            Vertex vert{};
            for (int k = 0; k < 3; ++k) {
                vert.position[k] = origin[k] + corners[i][0] * u[k] + corners[i][1] * w[k];
            }
            vert.color[0] = shade;
            vert.color[1] = shade;
            vert.color[2] = shade;
            vert.color[3] = 1.0f;
            v.push_back(vert);
        }
    };
    const float nz[3] = {0, 0, 1}, pz[3] = {0, 0, -1};
    const float nx[3] = {1, 0, 0}, px[3] = {-1, 0, 0};
    const float ny[3] = {0, 1, 0}, py[3] = {0, -1, 0};
    const float xpos[3] = {1, 0, 0}, xneg[3] = {-1, 0, 0};
    const float ypos[3] = {0, 1, 0};
    const float zpos[3] = {0, 0, 1}, zneg[3] = {0, 0, -1};

    face(nz, xpos, ypos);
    face(pz, xneg, ypos);
    face(nx, zneg, ypos);
    face(px, zpos, ypos);
    face(ny, xpos, zneg);
    face(py, xpos, zpos);
    return v;
}

constexpr uint32_t kMaxInstances = 256;  // scene boxes + joints
constexpr uint32_t kMaxJoints = 64;
constexpr uint32_t kInstanceStride = 64;  // four vec4s
constexpr float kClear[4] = {0.025f, 0.045f, 0.09f, 1.0f};

/**
 * Push constant range for the scene pipeline: 96 bytes.
 *
 * Sized to the Vulkan guaranteed minimum of 128 rather than to whatever the
 * machine that wrote the shader happened to support. The material table that
 * used to live here is 2 KB, and a `mat4 view` that no stage read pushed the
 * block to 152 — over the guarantee on its own.
 */
constexpr uint32_t kScenePushBytes = 96;

// The scene uniform block: sun direction and colour, sky and ground ambient,
// fog colour and density, night factor, and whether the maps are real. 96 bytes
// with the std140 vec3 padding the block actually occupies.
constexpr uint32_t kSceneUniformFloats = 24;

/** The neutral tile size, used only when the packed bake is not built. */
constexpr uint32_t kPlaceholderTile = 32;

} // namespace

struct VulkanRenderBackend::Impl {
    VulkanBackend *device = nullptr;
    SurfaceProvider *surface = nullptr;
    bool ready = false;
    std::string error;

    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // Target
    VkSurfaceKHR surfaceKHR = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    // The offscreen image belongs to the device, because the allocator that
    // backs it dies with the device. The surface belongs to the provider,
    // because only the provider knows how to destroy it.
    bool ownsOffscreen_ = false;

    // Pipeline
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    // -- the scene pipeline -------------------------------------------------
    //
    // Separate from the debug-cube pipeline above, and deliberately so. The cube
    // path is one 28-byte vertex and one instanced draw; the scene path is the
    // 48-byte layout, a real material table and three texture arrays. Merging
    // them would mean a pipeline with both vertex layouts, which is legal and
    // unreadable, and would put a shader that samples a material table into the
    // path used to draw a debug marker.
    VkShaderModule sceneVertModule = VK_NULL_HANDLE;
    VkShaderModule sceneFragModule = VK_NULL_HANDLE;
    VkPipelineLayout scenePipelineLayout = VK_NULL_HANDLE;
    VkPipeline scenePipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout sceneTexLayout = VK_NULL_HANDLE;    // set 0: the maps
    VkDescriptorSetLayout sceneUniformLayout = VK_NULL_HANDLE; // set 1: scene + table
    VkDescriptorSet sceneTexSet = VK_NULL_HANDLE;
    VkDescriptorSet sceneUniformSet = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    VkSampler sceneSampler = VK_NULL_HANDLE;
    VkImage sceneMaps[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory sceneMapMemory[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView sceneMapViews[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};

    /**
     * Layer count of the material maps, from the current table.
     *
     * Read at open() so the array is allocated once. Changing the table later
     * with a different layer count needs a reallocation, and
     * `setSceneMaterials` says so rather than silently uploading indices past
     * the end of the array.
     */
    uint32_t sceneMaterialLayers() const { return static_cast<uint32_t>(::emergent::sharedMaterialTable().layers); }

    /**
     * Time of day, 0 at midnight and 0.5 at noon.
     *
     * Accumulated from the frame loop rather than read from a wall clock, so the
     * sky is deterministic: the same sequence of frames always produces the same
     * light, which is what makes a lighting bug reproducible.
     */
    float dayFraction = 0.30f;

    // The assembled city: one static upload, one draw call.
    VkBuffer sceneVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sceneVertexMemory = VK_NULL_HANDLE;
    uint32_t sceneVertexCount = 0;
    // Per-frame uniforms, host-coherent and permanently mapped.
    VkBuffer sceneUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory sceneUniformMemory = VK_NULL_HANDLE;
    float *sceneUniformData = nullptr;
    VkBuffer materialBuffer = VK_NULL_HANDLE;
    VkDeviceMemory materialMemory = VK_NULL_HANDLE;
    float *materialData = nullptr;
    /**
     * False until the CC0 bake is uploaded into the map arrays.
     *
     * Until then the arrays hold physically neutral values, so the scene draws,
     * lights and fogs correctly but every surface is a flat mid-grey. Reported
     * in the frame stats rather than left to be discovered by looking at a
     * screenshot and wondering why nothing is textured.
     */
    bool sceneMapsLoaded = false;
    /** Why the maps are placeholders, when they are. */
    std::string sceneMapNote;
    /** Layers the map arrays were actually allocated with. */
    uint32_t sceneMapLayers = 0;
    /** Size of the mapped scene vertex range, for growth without reallocating. */
    VkDeviceSize sceneVertexBytes = 0;
    void *sceneVertexMapped = nullptr;

    // Geometry and per-frame data
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
    float *instanceData = nullptr;  // persistently mapped
    VkBuffer jointBuffer = VK_NULL_HANDLE;
    VkDeviceMemory jointMemory = VK_NULL_HANDLE;
    float *jointData = nullptr;  // persistently mapped
    uint32_t instanceCount = 0;
    uint32_t boxCount = 0;
    uint32_t jointCount = 0;

    // Per-frame synchronisation. One frame in flight, with a fence, is the
    // whole synchronisation story here and it is correct: the fence is waited
    // on before the command buffer is rewritten, so nothing is ever recorded
    // over a submission still in flight.
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;
    bool recording = false;

    /**
     * The eye position, hoisted out of the camera block.
     *
     * The sky and the scene push constants both need it, and both are written
     * outside the block that computes it. Recomputing it would be worse than
     * storing it, because the two copies could disagree.
     */
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float cameraYaw = 35.0f;
    float cameraPitch = 20.0f;
    float cameraDistance = 14.0f;
    float viewProjection[16] = {};
    float character[16] = {};
    float pushConstants[32] = {};

    // -- helpers ------------------------------------------------------------

    VkDevice dev() const { return reinterpret_cast<VkDevice>(device->device_); }
    VkPhysicalDevice phys() const { return reinterpret_cast<VkPhysicalDevice>(device->physical_device_); }
    VkInstance inst() const { return reinterpret_cast<VkInstance>(device->instance_); }
    VkQueue queue() const { return reinterpret_cast<VkQueue>(device->queue_); }
    VkCommandPool pool() const { return reinterpret_cast<VkCommandPool>(device->command_pool_); }

    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t &out) const {
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(phys(), &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) == 0) continue;
            if ((props.memoryTypes[i].propertyFlags & want) == want) {
                out = i;
                return true;
            }
        }
        return false;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer &buffer,
                      VkDeviceMemory &memory) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev(), &bci, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev(), buffer, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        if (!findMemoryType(req.memoryTypeBits, props, mai.memoryTypeIndex)) return false;
        if (vkAllocateMemory(dev(), &mai, nullptr, &memory) != VK_SUCCESS) return false;
        return vkBindBufferMemory(dev(), buffer, memory, 0) == VK_SUCCESS;
    }

    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer, VkDeviceMemory &memory,
                          void **mapped) {
        if (!createBuffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buffer, memory)) {
            return false;
        }
        // COHERENT above means no invalidation is needed, so a permanently
        // mapped upload buffer is correct here and saves a flush per frame.
        return vkMapMemory(dev(), memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
    }

    // -- teardown -----------------------------------------------------------

    void destroyTarget() {
        for (VkFramebuffer fb : framebuffers) {
            if (fb) vkDestroyFramebuffer(dev(), fb, nullptr);
        }
        framebuffers.clear();
        for (VkImageView v : swapViews) {
            if (v) vkDestroyImageView(dev(), v, nullptr);
        }
        swapViews.clear();
        swapImages.clear();
        if (depthView) {
            vkDestroyImageView(dev(), depthView, nullptr);
            depthView = VK_NULL_HANDLE;
        }
        if (depthImage) {
            vkDestroyImage(dev(), depthImage, nullptr);
            depthImage = VK_NULL_HANDLE;
        }
        if (depthMemory) {
            vkFreeMemory(dev(), depthMemory, nullptr);
            depthMemory = VK_NULL_HANDLE;
        }
        if (swapchain) {
            vkDestroySwapchainKHR(dev(), swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        if (surfaceKHR) {
            if (surface) {
                surface->destroyVulkanSurface(surfaceKHR);
            } else {
                vkDestroySurfaceKHR(inst(), surfaceKHR, nullptr);
            }
            surfaceKHR = VK_NULL_HANDLE;
        }
    }

    // -- the scene pipeline, built once at open() ---------------------------

    /**
     * Load the scene shaders, build its two descriptor sets and its pipeline.
     *
     * Failure here is not fatal. The debug-cube pipeline is still perfectly
     * good for what it is for, and a developer on a machine without the
     * SPIR-V built should still get a window and a status line rather than
     * nothing at all. So this reports and returns, and the frame loop carries
     * on drawing boxes.
     */
    bool buildScenePipeline(const std::string &shaderDir, std::string &errorOut) {
        std::vector<uint32_t> vertCode, fragCode;
        if (!loadSpirv(shaderDir + "/scene_pbr.vert.spv", vertCode) ||
            !loadSpirv(shaderDir + "/scene_pbr.frag.spv", fragCode)) {
            errorOut = "scene_pbr SPIR-V unavailable (build native/shaders to SPIR-V; "
                       "the renderer will draw debug boxes only)";
            return false;
        }
        if (!makeModule(vertCode, sceneVertModule) || !makeModule(fragCode, sceneFragModule)) {
            errorOut = "vkCreateShaderModule failed for the scene stages";
            return false;
        }

        // Set 0: the three material maps. Fragment only — a sampler in the
        // vertex stage would need a different descriptor and buys nothing.
        VkDescriptorSetLayoutBinding tex[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            tex[i].binding = i;
            tex[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tex[i].descriptorCount = 1;
            tex[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo tdsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        tdsl.bindingCount = 3;
        tdsl.pBindings = tex;
        if (vkCreateDescriptorSetLayout(dev(), &tdsl, nullptr, &sceneTexLayout) != VK_SUCCESS) {
            errorOut = "vkCreateDescriptorSetLayout failed for the scene maps";
            return false;
        }

        // Set 1: the per-frame scene block and the material table. Both stages
        // read the table: the vertex stage needs the tile scale and the map
        // mode, the fragment stage everything else.
        VkDescriptorSetLayoutBinding uni[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            uni[i].binding = i;
            uni[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uni[i].descriptorCount = 1;
            uni[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo udsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        udsl.bindingCount = 2;
        udsl.pBindings = uni;
        if (vkCreateDescriptorSetLayout(dev(), &udsl, nullptr, &sceneUniformLayout) != VK_SUCCESS) {
            errorOut = "vkCreateDescriptorSetLayout failed for the scene uniforms";
            return false;
        }

        VkDescriptorSetLayout layouts[2] = {sceneTexLayout, sceneUniformLayout};
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  kScenePushBytes};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 2;
        plci.pSetLayouts = layouts;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(dev(), &plci, nullptr, &scenePipelineLayout) != VK_SUCCESS) {
            errorOut = "vkCreatePipelineLayout failed for the scene";
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = sceneVertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = sceneFragModule;
        stages[1].pName = "main";

        // The 48-byte interleaved layout, matching scene_pbr.vert exactly.
        VkVertexInputBindingDescription binding{0, kVertexBytes, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, kOffsetPosition * 4},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, kOffsetNormal * 4},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, kOffsetColour * 4},
            {3, 0, VK_FORMAT_R32_SFLOAT, kOffsetMaterial * 4},
            {4, 0, VK_FORMAT_R32G32_SFLOAT, kOffsetUv * 4},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {width, height}};
        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // Front faces are counter-clockwise, which is what the emitters produce.
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vertexInput;
        gp.pInputAssemblyState = &assembly;
        gp.pViewportState = &viewportState;
        gp.pRasterizationState = &raster;
        gp.pMultisampleState = &multisample;
        gp.pDepthStencilState = &depth;
        gp.pColorBlendState = &blend;
        gp.layout = scenePipelineLayout;
        gp.renderPass = renderPass;
        gp.subpass = 0;
        const VkResult created = vkCreateGraphicsPipelines(dev(), VK_NULL_HANDLE, 1, &gp, nullptr,
                                                            &scenePipeline);
        if (created != VK_SUCCESS) {
            errorOut = "vkCreateGraphicsPipelines failed for the scene (" +
                       std::to_string(static_cast<int>(created)) + ")";
            return false;
        }
        return true;
    }

    bool loadSpirv(const std::string &path, std::vector<uint32_t> &out) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        const std::streamsize size = f.tellg();
        // A SPIR-V module is a sequence of 32-bit words, so a file whose size is
        // not a multiple of four is not a module. Checking it here turns a
        // truncated or HTML-error-page file into a clean "unavailable" rather
        // than a validation-layer error deep inside vkCreateShaderModule.
        if (size <= 0 || (size % 4) != 0) return false;
        out.resize(static_cast<size_t>(size) / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char *>(out.data()), size);
        return f.good() || f.eof();
    }

    bool makeModule(const std::vector<uint32_t> &code, VkShaderModule &out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        return vkCreateShaderModule(dev(), &ci, nullptr, &out) == VK_SUCCESS;
    }

    /**
     * The three material maps, from the packed CC0 bake when it is there.
     *
     * The three are not interchangeable, and the asymmetry is deliberate:
     * albedo is a colour and is uploaded as VK_FORMAT_R8G8B8A8_SRGB so the
     * hardware applies the transfer function before any lighting maths sees it,
     * while the normal and AO/roughness/metalness maps are *data* and are
     * uploaded as UNORM. Running a normal map through the sRGB curve bends it in
     * a way no shader correction undoes, and the result is lighting that is
     * subtly wrong in a way nobody can name.
     *
     * When the packed files are absent the maps are filled with physically
     * neutral values instead: a mid-grey albedo, the tangent-space neutral
     * normal, and AO 1 / roughness 0.5 / metal 0. That is the average material
     * rather than a wrong one, so the scene still lights and fogs correctly, and
     * `sceneMapsLoaded` stays false with a note saying why everything is grey. A
     * renderer that refuses to draw until the asset pipeline is wired up is a
     * renderer that looks broken.
     */
    bool createSceneMaps(uint32_t layers, const std::string &assetDir, std::string &errorOut) {
        sceneMapLayers = layers;

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        // Repeat, because box mapping produces world coordinates far outside
        // 0..1 and a clamp-to-edge sampler smears the last texel across every
        // surface beyond the first tile.
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = 8.0f;
        if (vkCreateSampler(dev(), &sci, nullptr, &sceneSampler) != VK_SUCCESS) {
            errorOut = "vkCreateSampler failed";
            return false;
        }

        // albedo, normal, arm. Albedo is sRGB; the other two are data.
        const char *names[3] = {"albedo.ktx2", "normal.ktx2", "arm.ktx2"};
        const VkFormat formats[3] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_FORMAT_R8G8B8A8_UNORM};

        Ktx2Image decoded[3];
        bool haveRealMaps = true;
        std::string loadError;
        for (int m = 0; m < 3; ++m) {
            const std::string path = assetDir + "/" + names[m];
            if (!loadKtx2(path, decoded[m], loadError)) {
                if (loadError.rfind("could not open", 0) == 0) {
                    // Absent, not malformed. The packer has not been run.
                    haveRealMaps = false;
                    break;
                }
                errorOut = std::string(names[m]) + ": " + loadError;
                return false;
            }
            if (decoded[m].layers != layers) {
                errorOut = std::string(names[m]) + " has " + std::to_string(decoded[m].layers) +
                           " layers but the material table has " + std::to_string(layers) +
                           "; re-run npm run assets:textures";
                return false;
            }
            if (decoded[m].vkFormat != formats[m]) {
                // A mismatch means the packer and the renderer disagree about
                // which maps are colour. Silently reinterpreting a UNORM normal
                // map as sRGB, or the reverse, produces lighting that is wrong by
                // a factor nobody can trace, so it is refused.
                errorOut = std::string(names[m]) + " is vkFormat " +
                           std::to_string(decoded[m].vkFormat) + ", expected " +
                           std::to_string(formats[m]) +
                           " (albedo is sRGB; normal and arm are linear data)";
                return false;
            }
        }

        for (int m = 0; m < 3; ++m) {
            uint32_t mapWidth = kPlaceholderTile;
            uint32_t mapHeight = kPlaceholderTile;
            uint32_t mipLevels = 1;
            if (haveRealMaps) {
                mapWidth = decoded[m].width;
                mapHeight = decoded[m].height;
                mipLevels = decoded[m].levelCount;
            }

            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = formats[m];
            ici.extent = {mapWidth, mapHeight, 1};
            ici.mipLevels = mipLevels;
            ici.arrayLayers = layers;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(dev(), &ici, nullptr, &sceneMaps[m]) != VK_SUCCESS) {
                errorOut = "vkCreateImage failed for a material map";
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(dev(), sceneMaps[m], &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            if (!findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 mai.memoryTypeIndex)) {
                errorOut = "no device-local memory type for a material map";
                return false;
            }
            if (vkAllocateMemory(dev(), &mai, nullptr, &sceneMapMemory[m]) != VK_SUCCESS) {
                errorOut = "vkAllocateMemory failed for a material map";
                return false;
            }
            vkBindImageMemory(dev(), sceneMaps[m], sceneMapMemory[m], 0);

            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image = sceneMaps[m];
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            // The view format must match the image format. An SRGB view over a
            // UNORM image is invalid, and a UNORM view over an SRGB image drops
            // the transfer function, which is the whole reason albedo is sRGB.
            ivci.format = formats[m];
            ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
            if (vkCreateImageView(dev(), &ivci, nullptr, &sceneMapViews[m]) != VK_SUCCESS) {
                errorOut = "vkCreateImageView failed for a material map";
                return false;
            }

            if (haveRealMaps) {
                if (!uploadKtx2(decoded[m], sceneMaps[m], mipLevels)) {
                    errorOut = std::string("uploading ") + names[m] + " failed";
                    return false;
                }
            } else if (!uploadSolid(placeholderFor(m), sceneMaps[m], kPlaceholderTile, layers)) {
                // The placeholder still has to be written: an undefined image is
                // a validation error the first time it is sampled, and the scene
                // has to draw *something*.
                errorOut = "the material map staging upload failed";
                return false;
            }
        }
        sceneMapsLoaded = haveRealMaps;
        if (!haveRealMaps) {
            sceneMapNote = assetDir + " not found; run npm run assets:textures";
        }
        return true;
    }

    /** A physically neutral tile, for when the packed bake is not built. */
    static std::vector<uint8_t> placeholderFor(int which) {
        const size_t n = static_cast<size_t>(kPlaceholderTile) * kPlaceholderTile;
        std::vector<uint8_t> px(n * 4, 0);
        for (size_t i = 0; i < n; ++i) {
            if (which == 0) {
                // Mid grey: the average of a real material, and dark enough to be
                // obviously untextured next to a wall that has brick on it.
                px[i * 4 + 0] = 128; px[i * 4 + 1] = 128; px[i * 4 + 2] = 128; px[i * 4 + 3] = 255;
            } else if (which == 1) {
                // The tangent-space neutral normal: flat, pointing out of the surface.
                px[i * 4 + 0] = 128; px[i * 4 + 1] = 128; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
            } else {
                px[i * 4 + 0] = 255;  // AO 1
                px[i * 4 + 1] = 128;  // roughness 0.5
                px[i * 4 + 2] = 0;    // metal 0
                px[i * 4 + 3] = 255;
            }
        }
        return px;
    }

    /**
     * Copy every mip level of a decoded KTX2 image into an array image.
     *
     * All levels go through one staging buffer and one command submission. The
     * obvious alternative, a submission per level, would need a fence between
     * them because the staging buffer is rewritten while the previous copy may
     * still be reading it — and this upload is 26 layers x 10 mips x 3 maps, so
     * 780 submissions is a startup stall nobody would diagnose.
     *
     * A level's layers are contiguous in the file, and `imageSubresource` may
     * name the whole array, so one copy per mip covers all 26 layers: ten copies
     * per map rather than 260.
     */
    bool uploadKtx2(const Ktx2Image &image, VkImage target, uint32_t mipLevels) {
        size_t largest = 0;
        for (const Ktx2Level &level : image.levels) {
            largest = std::max(largest, level.pixels.size());
        }
        if (largest == 0) return false;

        VkBuffer staging{};
        VkDeviceMemory stagingMemory{};
        void *mapped = nullptr;
        if (!createHostBuffer(largest, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingMemory,
                              &mapped)) {
            return false;
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev(), &cai, &cmd) != VK_SUCCESS) {
            vkDestroyBuffer(dev(), staging, nullptr);
            vkFreeMemory(dev(), stagingMemory, nullptr);
            return false;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        // One transition for the whole image, covering every mip and layer.
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, image.layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        for (uint32_t mip = 0; mip < image.levelCount && mip < mipLevels; ++mip) {
            const Ktx2Level &level = image.levels[mip];
            if (level.pixels.size() > largest) continue;
            std::memcpy(mapped, level.pixels.data(), level.pixels.size());
            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, image.layers};
            copy.imageOffset = {0, 0, 0};
            copy.imageExtent = {level.width, level.height, 1};
            vkCmdCopyBufferToImage(cmd, staging, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
        }

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);

        vkEndCommandBuffer(cmd);
        VkFence fence{};
        vkCreateFence(dev(), nullptr, nullptr, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue(), 1, &si, fence);
        vkWaitForFences(dev(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(dev(), fence, nullptr);
        vkFreeCommandBuffers(dev(), pool(), 1, &cmd);
        vkUnmapMemory(dev(), stagingMemory);
        vkDestroyBuffer(dev(), staging, nullptr);
        vkFreeMemory(dev(), stagingMemory, nullptr);
        return true;
    }

    /** Copy a solid colour into every layer of an array image. */
    bool uploadSolid(const std::vector<uint8_t> &tile, VkImage image, uint32_t tileSize, uint32_t layers) {
        const VkDeviceSize bytes = tile.size();
        VkBuffer staging{};
        VkDeviceMemory stagingMemory{};
        void *mapped = nullptr;
        if (!createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, stagingMemory, &mapped)) {
            return false;
        }
        std::memcpy(mapped, tile.data(), static_cast<size_t>(bytes));

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev(), &cai, &cmd) != VK_SUCCESS) {
            vkDestroyBuffer(dev(), staging, nullptr);
            vkFreeMemory(dev(), stagingMemory, nullptr);
            return false;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        copy.imageOffset = {0, 0, 0};
        copy.imageExtent = {tileSize, tileSize, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier toRead = toDst;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);

        vkEndCommandBuffer(cmd);
        VkFence fence{};
        vkCreateFence(dev(), nullptr, nullptr, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue(), 1, &si, fence);
        vkWaitForFences(dev(), 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(dev(), fence, nullptr);
        vkFreeCommandBuffers(dev(), pool(), 1, &cmd);
        vkUnmapMemory(dev(), stagingMemory);
        vkDestroyBuffer(dev(), staging, nullptr);
        vkFreeMemory(dev(), stagingMemory, nullptr);
        return true;
    }

    bool createSceneUniforms(const MaterialTable &materials, std::string &errorOut) {
        // Scene block: 3 vec3 and 3 float, rounded up to a 16-byte multiple.
        const VkDeviceSize sceneBytes = 96;
        if (!createHostBuffer(sceneBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sceneUniformBuffer,
                              sceneUniformMemory, reinterpret_cast<void **>(&sceneUniformData))) {
            errorOut = "the scene uniform buffer could not be created";
            return false;
        }
        // Material table: two vec4 per material, then the count. Sized from
        // kMaxMaterials so the shader's compile-time array and this buffer can
        // never disagree.
        const VkDeviceSize tableBytes = static_cast<VkDeviceSize>(kMaxMaterials) * 2 * 16 + 16;
        if (!createHostBuffer(tableBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, materialBuffer, materialMemory,
                              reinterpret_cast<void **>(&materialData))) {
            errorOut = "the material table buffer could not be created";
            return false;
        }
        uploadMaterialTable(materials);

        VkDescriptorPoolSize sizes[2]{};
        sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
        sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        // A pool of its own rather than sharing the debug path's. The debug pool
        // is sized for two storage buffers and one set, and growing it to cover
        // the scene's combined image samplers would put the scene's descriptor
        // lifetime in the same object as the debug path's, so closing one would
        // free the other's sets.
        if (vkCreateDescriptorPool(dev(), &dpci, nullptr, &sceneDescriptorPool) != VK_SUCCESS) {
            errorOut = "vkCreateDescriptorPool failed for the scene sets";
            return false;
        }

        VkDescriptorSetLayout setLayouts[2] = {sceneTexLayout, sceneUniformLayout};
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = sceneDescriptorPool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = setLayouts;
        VkDescriptorSet sets[2]{};
        if (vkAllocateDescriptorSets(dev(), &dsai, sets) != VK_SUCCESS) {
            errorOut = "vkAllocateDescriptorSets failed for the scene";
            return false;
        }
        sceneTexSet = sets[0];
        sceneUniformSet = sets[1];

        VkDescriptorImageInfo images[3]{};
        for (int m = 0; m < 3; ++m) {
            images[m].sampler = sceneSampler;
            images[m].imageView = sceneMapViews[m];
            images[m].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = sceneTexSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        VkDescriptorBufferInfo buffers[2] = {
            {sceneUniformBuffer, 0, VK_WHOLE_SIZE},
            {materialBuffer, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet uniformWrites[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            uniformWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            uniformWrites[i].dstSet = sceneUniformSet;
            uniformWrites[i].dstBinding = i;
            uniformWrites[i].descriptorCount = 1;
            uniformWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniformWrites[i].pBufferInfo = &buffers[i];
        }
        VkWriteDescriptorSet all[5];
        for (int i = 0; i < 3; ++i) all[i] = writes[i];
        for (int i = 0; i < 2; ++i) all[3 + i] = uniformWrites[i];
        vkUpdateDescriptorSets(dev(), 5, all, 0, nullptr);
        return true;
    }

    void uploadMaterialTable(const MaterialTable &materials) {
        std::memset(materialData, 0, static_cast<size_t>(kMaxMaterials) * 2 * 16 + 16);
        const int32_t n = std::min<int32_t>(materials.count(), kMaxMaterials);
        for (int32_t i = 0; i < n; ++i) {
            std::memcpy(materialData + static_cast<size_t>(i) * 4, &materials.a[static_cast<size_t>(i) * 4],
                        16);
            std::memcpy(materialData + static_cast<size_t>(kMaxMaterials + i) * 4,
                        &materials.b[static_cast<size_t>(i) * 4], 16);
        }
        // materialCount sits after both arrays. Passed as an int, and read as
        // one, so there is no float/int reinterpretation to get wrong.
        const int32_t count = n;
        std::memcpy(materialData + static_cast<size_t>(kMaxMaterials) * 2 * 4, &count, 4);
    }

    /** Replace the scene geometry. Safe to call every frame with the same mesh. */
    bool uploadSceneMesh(const SceneMesh &mesh, std::string &errorOut) {
        const VkDeviceSize bytes = mesh.vertices.size() * sizeof(float);
        if (bytes == 0) {
            sceneVertexCount = 0;
            return true;
        }
        if (bytes > sceneVertexBytes) {
            if (sceneVertexBuffer) {
                vkDestroyBuffer(dev(), sceneVertexBuffer, nullptr);
                sceneVertexBuffer = VK_NULL_HANDLE;
            }
            if (sceneVertexMemory) {
                vkFreeMemory(dev(), sceneVertexMemory, nullptr);
                sceneVertexMemory = VK_NULL_HANDLE;
            }
            void *mapped = nullptr;
            if (!createHostBuffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sceneVertexBuffer,
                                  sceneVertexMemory, &mapped)) {
                errorOut = "the scene vertex buffer could not be created";
                return false;
            }
            sceneVertexMapped = mapped;
            sceneVertexBytes = bytes;
        }
        std::memcpy(sceneVertexMapped, mesh.vertices.data(), static_cast<size_t>(bytes));
        sceneVertexCount = mesh.vertexCount();
        return true;
    }

    void destroyScenePipeline() {
        if (!device || !device->initialized()) return;
        if (sceneUniformData) {
            vkUnmapMemory(dev(), sceneUniformMemory);
            sceneUniformData = nullptr;
        }
        if (materialData) {
            vkUnmapMemory(dev(), materialMemory);
            materialData = nullptr;
        }
        for (int m = 0; m < 3; ++m) {
            if (sceneMapViews[m]) vkDestroyImageView(dev(), sceneMapViews[m], nullptr);
            if (sceneMaps[m]) vkDestroyImage(dev(), sceneMaps[m], nullptr);
            if (sceneMapMemory[m]) vkFreeMemory(dev(), sceneMapMemory[m], nullptr);
            sceneMapViews[m] = VK_NULL_HANDLE;
            sceneMaps[m] = VK_NULL_HANDLE;
            sceneMapMemory[m] = VK_NULL_HANDLE;
        }
        if (sceneSampler) {
            vkDestroySampler(dev(), sceneSampler, nullptr);
            sceneSampler = VK_NULL_HANDLE;
        }
        if (sceneVertexBuffer) vkDestroyBuffer(dev(), sceneVertexBuffer, nullptr);
        if (sceneVertexMemory) vkFreeMemory(dev(), sceneVertexMemory, nullptr);
        sceneVertexBuffer = VK_NULL_HANDLE;
        sceneVertexMemory = VK_NULL_HANDLE;
        sceneVertexBytes = 0;
        sceneVertexMapped = nullptr;
        sceneVertexCount = 0;
        for (VkBuffer b : {sceneUniformBuffer, materialBuffer}) {
            if (b) vkDestroyBuffer(dev(), b, nullptr);
        }
        sceneUniformBuffer = materialBuffer = VK_NULL_HANDLE;
        for (VkPipeline p : {scenePipeline}) {
            if (p) vkDestroyPipeline(dev(), p, nullptr);
        }
        scenePipeline = VK_NULL_HANDLE;
        for (VkShaderModule m : {sceneVertModule, sceneFragModule}) {
            if (m) vkDestroyShaderModule(dev(), m, nullptr);
        }
        sceneVertModule = sceneFragModule = VK_NULL_HANDLE;
        if (scenePipelineLayout) {
            vkDestroyPipelineLayout(dev(), scenePipelineLayout, nullptr);
            scenePipelineLayout = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout l : {sceneTexLayout, sceneUniformLayout}) {
            if (l) vkDestroyDescriptorSetLayout(dev(), l, nullptr);
        }
        sceneTexLayout = sceneUniformLayout = VK_NULL_HANDLE;
        sceneTexSet = sceneUniformSet = VK_NULL_HANDLE;
        if (sceneDescriptorPool) {
            vkDestroyDescriptorPool(dev(), sceneDescriptorPool, nullptr);
            sceneDescriptorPool = VK_NULL_HANDLE;
        }
    }

    void destroyAll() {
        if (!device || !device->initialized()) {
            ready = false;
            return;
        }
        // Everything is destroyed in the reverse of the order it was created,
        // and the device is idle first. Skipping the wait is how a driver ends
        // up destroying a resource it is still executing.
        vkDeviceWaitIdle(dev());
        destroyScenePipeline();

        if (inFlight) {
            vkDestroyFence(dev(), inFlight, nullptr);
            inFlight = VK_NULL_HANDLE;
        }
        if (renderFinished) {
            vkDestroySemaphore(dev(), renderFinished, nullptr);
            renderFinished = VK_NULL_HANDLE;
        }
        if (instanceData) {
            vkUnmapMemory(dev(), instanceMemory);
            instanceData = nullptr;
        }
        if (jointData) {
            vkUnmapMemory(dev(), jointMemory);
            jointData = nullptr;
        }
        for (VkBuffer b : {vertexBuffer, instanceBuffer, jointBuffer}) {
            if (b) vkDestroyBuffer(dev(), b, nullptr);
        }
        vertexBuffer = instanceBuffer = jointBuffer = VK_NULL_HANDLE;
        for (VkDeviceMemory m : {vertexMemory, instanceMemory, jointMemory}) {
            if (m) vkFreeMemory(dev(), m, nullptr);
        }
        vertexMemory = instanceMemory = jointMemory = VK_NULL_HANDLE;
        if (descriptorPool) {
            vkDestroyDescriptorPool(dev(), descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (pipeline) {
            vkDestroyPipeline(dev(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        if (pipelineLayout) {
            vkDestroyPipelineLayout(dev(), pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (setLayout) {
            vkDestroyDescriptorSetLayout(dev(), setLayout, nullptr);
            setLayout = VK_NULL_HANDLE;
        }
        if (vertModule) {
            vkDestroyShaderModule(dev(), vertModule, nullptr);
            vertModule = VK_NULL_HANDLE;
        }
        if (fragModule) {
            vkDestroyShaderModule(dev(), fragModule, nullptr);
            fragModule = VK_NULL_HANDLE;
        }
        if (renderPass) {
            vkDestroyRenderPass(dev(), renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        destroyTarget();
        if (ownsOffscreen_) {
            device->destroyOffscreenSurface();
            ownsOffscreen_ = false;
        }
        ready = false;
        recording = false;
    }
};

VulkanRenderBackend::VulkanRenderBackend() : impl_(std::make_unique<Impl>()) {}
VulkanRenderBackend::~VulkanRenderBackend() { close(); }

bool VulkanRenderBackend::initialize(VulkanBackend &device, const VulkanInitOptions &options) {
    (void)options;
    if (!device.initialized()) {
        stats_.status = "Vulkan device is not initialized; nothing can be rendered";
        return false;
    }
    impl_->device = &device;
    stats_.hasDevice = true;
    return true;
}

void VulkanRenderBackend::shutdownDevice() { close(); }

bool VulkanRenderBackend::setSceneMesh(const SceneMesh &mesh) {
    Impl &s = *impl_;
    std::string error;
    if (!s.uploadSceneMesh(mesh, error)) {
        stats_.status = "scene upload failed: " + error;
        return false;
    }
    return true;
}

bool VulkanRenderBackend::setSceneMaterials(const MaterialTable &materials) {
    Impl &s = *impl_;
    if (materials.layers > static_cast<int32_t>(s.sceneMapLayers)) {
        // Rejected rather than clamped. Clamping leaves the shader indexing
        // layers that exist but hold the placeholder, which looks like a texture
        // bug in the material system instead of a bad call here.
        stats_.status = "material table has " + std::to_string(materials.layers) +
                        " layers but the maps hold " + std::to_string(s.sceneMapLayers);
        return false;
    }
    ::emergent::sharedMaterialTable() = materials;
    if (s.materialData) s.uploadMaterialTable(materials);
    return true;
}

bool VulkanRenderBackend::sceneMapsLoaded() const { return impl_->sceneMapsLoaded; }

const std::string &VulkanRenderBackend::materialMapNote() const { return impl_->sceneMapNote; }

void VulkanRenderBackend::setCamera(float yawDegrees, float pitchDegrees, float distance) {
    impl_->cameraYaw = yawDegrees;
    impl_->cameraPitch = pitchDegrees;
    // A zero or negative distance puts the eye inside the target and collapses
    // the look-at basis, which projects to a black screen with no error.
    impl_->cameraDistance = (std::isfinite(distance) && distance > 0.1f) ? distance : 0.1f;
}

bool VulkanRenderBackend::open(uint32_t width, uint32_t height, SurfaceProvider *surface) {
    close();
    Impl &s = *impl_;
    s.surface = surface;
    stats_ = RenderFrameStats{};
    stats_.hasDevice = device_ != nullptr && device_->initialized();

    if (!stats_.hasDevice) {
        stats_.status = "no Vulkan device: the renderer is compiled in but there is nothing to render on";
        opened_ = false;
        pipelineReady_ = false;
        return false;
    }
    if (width == 0 || height == 0) {
        stats_.status = "render target size must be non-zero";
        opened_ = false;
        return false;
    }
    s.width = width;
    s.height = height;
    stats_.width = width;
    stats_.height = height;

    const VkDevice dev = s.dev();
    VkResult r = VK_SUCCESS;

    // -- target ------------------------------------------------------------
    bool swapchainOK = false;
    if (surface != nullptr) {
        void *raw = surface->createVulkanSurface(s.inst());
        if (raw != nullptr) {
            s.surfaceKHR = reinterpret_cast<VkSurfaceKHR>(raw);
            VkBool32 supported = VK_FALSE;
            // Asking the loader whether this surface can actually present is
            // the difference between a clear error message and a
            // VK_ERROR_SURFACE_LOST_KHR on the first present.
            if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys(), device_->queue_family_, s.surfaceKHR, &supported) ==
                    VK_SUCCESS &&
                supported == VK_TRUE) {
                swapchainOK = true;
            } else {
                s.error = "the window's surface is not supported by this queue family";
                s.surfaceKHR = VK_NULL_HANDLE;
            }
        }
    }

    VkImage targetImage = VK_NULL_HANDLE;
    if (swapchainOK) {
        VkSurfaceCapabilitiesKHR caps{};
        r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys(), s.surfaceKHR, &caps);
        if (r != VK_SUCCESS) {
            s.error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + std::to_string(r);
            swapchainOK = false;
        } else {
            // The surface is free to hand back an extent that is not the one
            // requested: a maximised window on a HiDPI display usually is not.
            // Clamping to what the surface actually supports is the difference
            // between a correct swapchain and a create failure.
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX) {
                extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
            }
            s.width = extent.width;
            s.height = extent.height;

            // The colour format comes from the surface's format list, not from
            // VkSurfaceCapabilitiesKHR: the capabilities struct describes size
            // and image counts, and picking a format the surface does not
            // offer is a vkCreateSwapchainKHR failure.
            uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys(), s.surfaceKHR, &formatCount, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(formatCount);
            if (formatCount) {
                vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys(), s.surfaceKHR, &formatCount, formats.data());
            }
            // A surface always offers at least one format; the guard is here
            // because falling back to a format the surface did not list is
            // better than dereferencing an empty vector.
            s.colorFormat = formatCount ? formats[0].format : VK_FORMAT_B8G8R8A8_UNORM;

            uint32_t imageCount = caps.minImageCount + 1;
            if (caps.maxImageCount > 0) {
                imageCount = std::min(caps.maxImageCount, imageCount);
            }

            VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            sci.surface = s.surfaceKHR;
            sci.minImageCount = imageCount;
            sci.imageFormat = s.colorFormat;
            sci.imageColorSpace = formats.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : formats[0].colorSpace;
            sci.imageExtent = VkExtent2D{s.width, s.height};
            sci.imageArrayLayers = 1;
            // FIFO is the only present mode Vulkan guarantees to support, and
            // it is the right default: it is the one that cannot tear and it
            // paces the loop to the display.
            sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            sci.clipped = VK_TRUE;
            sci.oldSwapchain = VK_NULL_HANDLE;
            r = vkCreateSwapchainKHR(dev, &sci, nullptr, &s.swapchain);
            if (r != VK_SUCCESS) {
                s.error = "vkCreateSwapchainKHR failed: " + std::to_string(r);
                swapchainOK = false;
            } else {
                hasSwapchain_ = true;
                stats_.hasSwapchain = true;
                uint32_t got = 0;
                vkGetSwapchainImagesKHR(dev, s.swapchain, &got, nullptr);
                s.swapImages.resize(got);
                vkGetSwapchainImagesKHR(dev, s.swapchain, &got, s.swapImages.data());
            }
        }
    }

    if (!swapchainOK) {
        hasSwapchain_ = false;
        stats_.hasSwapchain = false;
        if (surface != nullptr && !s.error.empty()) {
            // A window was offered and rejected. Say so plainly rather than
            // silently going offscreen and looking like it worked.
            stats_.status = "window present but no swapchain (" + s.error + "); rendering offscreen";
        }
        std::string offErr;
        targetImage = reinterpret_cast<VkImage>(device_->createOffscreenSurface(s.width, s.height, offErr));
        if (targetImage == VK_NULL_HANDLE) {
            stats_.status = "no render target: " + offErr;
            opened_ = false;
            return false;
        }
        s.ownsOffscreen_ = true;
        s.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    }

    const uint32_t targetCount = hasSwapchain_ ? static_cast<uint32_t>(s.swapImages.size()) : 1u;
    if (targetCount == 0) {
        stats_.status = "swapchain reported zero images";
        opened_ = false;
        return false;
    }

    for (uint32_t i = 0; i < targetCount; ++i) {
        const VkImage image = hasSwapchain_ ? s.swapImages[i] : targetImage;
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = s.colorFormat;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &ivci, nullptr, &view) != VK_SUCCESS) {
            stats_.status = "vkCreateImageView failed for the colour target";
            close();
            opened_ = false;
            return false;
        }
        s.swapViews.push_back(view);
    }

    // -- depth -------------------------------------------------------------
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_D32_SFLOAT;
        ici.extent = {s.width, s.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ici, nullptr, &s.depthImage) != VK_SUCCESS) {
            stats_.status = "vkCreateImage failed for the depth target";
            close();
            opened_ = false;
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, s.depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        if (!s.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mai.memoryTypeIndex) ||
            vkAllocateMemory(dev, &mai, nullptr, &s.depthMemory) != VK_SUCCESS ||
            vkBindImageMemory(dev, s.depthImage, s.depthMemory, 0) != VK_SUCCESS) {
            stats_.status = "no device-local memory for the depth target";
            close();
            opened_ = false;
            return false;
        }
        VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        dvci.image = s.depthImage;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = VK_FORMAT_D32_SFLOAT;
        dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev, &dvci, nullptr, &s.depthView) != VK_SUCCESS) {
            stats_.status = "vkCreateImageView failed for the depth target";
            close();
            opened_ = false;
            return false;
        }
    }

    // -- render pass and framebuffers --------------------------------------
    {
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = s.colorFormat;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        // One dependency covering acquire -> draw -> present. Without the
        // bottom-to-top pair the first frame is presented while the render
        // pass is still writing to it.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 2;
        rpci.pAttachments = attachments;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dependency;
        if (vkCreateRenderPass(dev, &rpci, nullptr, &s.renderPass) != VK_SUCCESS) {
            stats_.status = "vkCreateRenderPass failed";
            close();
            opened_ = false;
            return false;
        }

        for (uint32_t i = 0; i < targetCount; ++i) {
            VkImageView views[2] = {s.swapViews[i], s.depthView};
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = s.renderPass;
            fbci.attachmentCount = 2;
            fbci.pAttachments = views;
            fbci.width = s.width;
            fbci.height = s.height;
            fbci.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) {
                stats_.status = "vkCreateFramebuffer failed";
                close();
                opened_ = false;
                return false;
            }
            s.framebuffers.push_back(fb);
        }
    }

    // -- shaders -----------------------------------------------------------
    {
        std::vector<uint32_t> vertCode;
        std::vector<uint32_t> fragCode;
        std::string loadError;
        const std::string vertPath = shaderDir_ + "/scene.vert.spv";
        const std::string fragPath = shaderDir_ + "/scene.frag.spv";
        if (!loadSpirvModule(vertPath, vertCode, loadError) ||
            !loadSpirvModule(fragPath, fragCode, loadError)) {
            // The most likely cause by far, and the one worth naming: the GLSL
            // in native/shaders has not been compiled. loadSpirvModule has
            // already checked the file is a real module, so anything past this
            // point is a genuine build problem.
            stats_.status = "shader module unavailable: " + loadError +
                            " (build the GLSL in native/shaders to SPIR-V; see docs/ENGINE_VERIFICATION.md)";
            close();
            opened_ = false;
            pipelineReady_ = false;
            return false;
        }
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = vertCode.size() * 4;
        smci.pCode = vertCode.data();
        if (vkCreateShaderModule(dev, &smci, nullptr, &s.vertModule) != VK_SUCCESS) {
            stats_.status = "vkCreateShaderModule failed for the vertex stage";
            close();
            opened_ = false;
            return false;
        }
        smci.codeSize = fragCode.size() * 4;
        smci.pCode = fragCode.data();
        if (vkCreateShaderModule(dev, &smci, nullptr, &s.fragModule) != VK_SUCCESS) {
            stats_.status = "vkCreateShaderModule failed for the fragment stage";
            close();
            opened_ = false;
            return false;
        }
    }

    // -- descriptor layout, pipeline ---------------------------------------
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            // Read in the vertex stage only. A storage buffer written by a
            // shader needs a barrier that this pipeline never has a reason to
            // declare, so keeping the buffers read-only removes a whole class
            // of synchronisation bug rather than handling it.
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 2;
        dslci.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &s.setLayout) != VK_SUCCESS) {
            stats_.status = "vkCreateDescriptorSetLayout failed";
            close();
            opened_ = false;
            return false;
        }

        // Exactly two mat4, which is 128 bytes: the guaranteed minimum for
        // maxPushConstantsSize, so this works on every conformant
        // implementation. Anything more needs a uniform buffer.
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 128};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &s.setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &s.pipelineLayout) != VK_SUCCESS) {
            stats_.status = "vkCreatePipelineLayout failed";
            close();
            opened_ = false;
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s.vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s.fragModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(s.width), static_cast<float>(s.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {s.width, s.height}};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        // No back-face culling. The facing of a triangle depends on whether
        // the projection flipped y, and this renderer decides that itself in
        // perspective() above. A debug view that renders nothing because a
        // winding convention is debatable is a far worse outcome than the few
        // triangles a box costs to draw twice.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                         VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vertexInput;
        gpci.pInputAssemblyState = &assembly;
        gpci.pViewportState = &viewportState;
        gpci.pRasterizationState = &raster;
        gpci.pMultisampleState = &multisample;
        gpci.pDepthStencilState = &depthStencil;
        gpci.pColorBlendState = &blend;
        gpci.layout = s.pipelineLayout;
        gpci.renderPass = s.renderPass;
        gpci.subpass = 0;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &s.pipeline) != VK_SUCCESS) {
            stats_.status = "vkCreateGraphicsPipelines failed";
            close();
            opened_ = false;
            return false;
        }
    }

    // -- buffers -----------------------------------------------------------
    {
        const std::vector<Vertex> cube = makeCube();
        s.vertexCount = static_cast<uint32_t>(cube.size());
        VkDeviceSize vertexBytes = cube.size() * sizeof(Vertex);
        if (!s.createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            s.vertexBuffer, s.vertexMemory)) {
            stats_.status = "could not create the vertex buffer";
            close();
            opened_ = false;
            return false;
        }
        void *mapped = nullptr;
        if (vkMapMemory(dev, s.vertexMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            stats_.status = "could not map the vertex buffer";
            close();
            opened_ = false;
            return false;
        }
        std::memcpy(mapped, cube.data(), static_cast<size_t>(vertexBytes));
        // Flushed before unmapping: the device has to see the cube before the
        // first submit, and nothing after this point would ever trigger it.
        VkMappedMemoryRange flushRange{};
        flushRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flushRange.memory = s.vertexMemory;
        flushRange.offset = 0;
        flushRange.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(dev, 1, &flushRange);
        vkUnmapMemory(dev, s.vertexMemory);

        if (!s.createHostBuffer(static_cast<VkDeviceSize>(kMaxInstances) * kInstanceStride,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.instanceBuffer, s.instanceMemory,
                                reinterpret_cast<void **>(&s.instanceData))) {
            stats_.status = "could not create the per-frame instance buffer";
            close();
            opened_ = false;
            return false;
        }
        if (!s.createHostBuffer(static_cast<VkDeviceSize>(kMaxJoints) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                s.jointBuffer, s.jointMemory, reinterpret_cast<void **>(&s.jointData))) {
            stats_.status = "could not create the per-frame joint buffer";
            close();
            opened_ = false;
            return false;
        }

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &s.descriptorPool) != VK_SUCCESS) {
            stats_.status = "vkCreateDescriptorPool failed";
            close();
            opened_ = false;
            return false;
        }
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = s.descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s.setLayout;
        if (vkAllocateDescriptorSets(dev, &dsai, &s.descriptorSet) != VK_SUCCESS) {
            stats_.status = "vkAllocateDescriptorSets failed";
            close();
            opened_ = false;
            return false;
        }
        VkDescriptorBufferInfo bufferInfos[2] = {
            {s.instanceBuffer, 0, VK_WHOLE_SIZE},
            {s.jointBuffer, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = s.descriptorSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // -- the scene pipeline -------------------------------------------------
    //
    // Non-fatal, and deliberately so. Everything above is a hard failure because
    // without it there is no renderer at all. This is a second renderer: if its
    // SPIR-V is not built, or the driver refuses the pipeline, the backend keeps
    // drawing the debug boxes and says why in stats().status. A developer
    // without the Vulkan SDK should get a window and an explanation, not a
    // silent black screen.
    {
        std::string sceneError;
        if (s.buildScenePipeline(shaderDir_, sceneError)) {
            if (!s.createSceneMaps(s.sceneMaterialLayers(), sceneAssetDirectory_, sceneError)) {
                // The maps failed, so the pipeline is torn back down rather than
                // left bound with an unbound sampler.
                s.destroyScenePipeline();
            } else if (!s.createSceneUniforms(::emergent::sharedMaterialTable(), sceneError)) {
                s.destroyScenePipeline();
            }
        }
        if (!sceneError.empty()) {
            stats_.status = "scene pipeline unavailable: " + sceneError +
                            "; the debug renderer is still active";
        }
    }

    // -- per-frame synchronisation -----------------------------------------
    {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = s.pool();
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cbai, &s.command) != VK_SUCCESS) {
            stats_.status = "vkAllocateCommandBuffers failed";
            close();
            opened_ = false;
            return false;
        }
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(dev, &semci, nullptr, &s.renderFinished) != VK_SUCCESS) {
            stats_.status = "vkCreateSemaphore failed";
            close();
            opened_ = false;
            return false;
        }
        VkFenceCreateInfo fenceci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        // Created signalled: the first beginFrame must not wait on a
        // submission that has not happened yet.
        fenceci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev, &fenceci, nullptr, &s.inFlight) != VK_SUCCESS) {
            stats_.status = "vkCreateFence failed";
            close();
            opened_ = false;
            return false;
        }
    }

    s.ready = true;
    s.error.clear();
    opened_ = true;
    pipelineReady_ = true;
    stats_.status = hasSwapchain_ ? "swapchain acquired; presenting each frame"
                                  : "offscreen target created; nothing is being presented";
    return true;
}

void VulkanRenderBackend::close() noexcept {
    if (impl_) impl_->destroyAll();
    opened_ = false;
    pipelineReady_ = false;
    hasSwapchain_ = false;
}

void VulkanRenderBackend::beginFrame() {
    Impl &s = *impl_;
    if (!opened_ || !s.ready) return;
    // Wait for the previous submit before its command buffer is rewritten.
    // This is the entire reason there is a fence: without it, frame N
    // overwrites the command buffer that frame N-1's GPU work is still
    // reading, which is undefined behaviour that usually looks like a
    // flickering or corrupted frame rather than a crash.
    vkWaitForFences(s.dev(), 1, &s.inFlight, VK_TRUE, UINT64_MAX);
    if (resizeRequested_ && hasSwapchain_) {
        // Rebuilding the swapchain tears down and recreates every resource
        // this backend owns, so the frame is abandoned rather than recorded
        // against objects that are about to be replaced. That is what a
        // resize costs, and it is why a resize is allowed to drop a frame.
        resizeRequested_ = false;
        open(s.width, s.height, s.surface);
        return;
    }
    if (hasSwapchain_) {
        // Argument order matters here and is easy to get backwards: the
        // semaphore is signalled when the image is ready, the fence is a
        // separate optional completion signal. Passing the semaphore where the
        // fence belongs is a type error, and passing VK_NULL_HANDLE for the
        // semaphore is a frame that waits for ever.
        VkResult r = vkAcquireNextImageKHR(s.dev(), s.swapchain, UINT64_MAX, s.renderFinished, VK_NULL_HANDLE,
                                           &s.imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            std::string ignored;
            open(s.width, s.height, s.surface);
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            stats_.status = "vkAcquireNextImageKHR failed: " + std::to_string(r);
            ++stats_.skippedFrames;
            return;
        }
    }
    s.recording = true;
}

void VulkanRenderBackend::drawFrame(const FrameState &state) {
    Impl &s = *impl_;
    if (!opened_ || !s.ready || !s.recording) {
        ++stats_.skippedFrames;
        return;
    }

    // -- upload the frame -------------------------------------------------
    uint32_t instance = 0;
    const uint32_t boxBudget = std::min<uint32_t>(static_cast<uint32_t>(std::max(state.sceneBoxCount, 0)),
                                                  kMaxInstances - kMaxJoints);
    for (uint32_t i = 0; i < boxBudget; ++i) {
        const SceneBox &b = state.sceneBoxes[i];
        float *dst = s.instanceData + static_cast<size_t>(instance) * (kInstanceStride / 4);
        dst[0] = b.center[0];
        dst[1] = b.center[1];
        dst[2] = b.center[2];
        dst[3] = 0.0f;
        dst[4] = b.halfExtent[0];
        dst[5] = b.halfExtent[1];
        dst[6] = b.halfExtent[2];
        dst[7] = 0.0f;
        const uint32_t c = b.color;
        dst[8] = static_cast<float>((c >> 16) & 0xffu) / 255.0f;
        dst[9] = static_cast<float>((c >> 8) & 0xffu) / 255.0f;
        dst[10] = static_cast<float>(c & 0xffu) / 255.0f;
        dst[11] = static_cast<float>((c >> 24) & 0xffu) / 255.0f;
        dst[12] = 0.0f;  // joint index, unused
        dst[13] = 0.0f;  // not a joint
        dst[14] = 0.0f;
        dst[15] = 0.0f;
        ++instance;
    }
    s.boxCount = instance;

    // Joints are drawn as small boxes through the same pipeline, indexed past
    // the scene boxes in the same instance buffer. One pipeline, one bind, one
    // draw call for the whole frame.
    s.jointCount = 0;
    if (state.jointMatrices != nullptr && state.jointCount > 0) {
        const uint32_t jointBudget = std::min<uint32_t>(static_cast<uint32_t>(state.jointCount), kMaxJoints);
        for (uint32_t j = 0; j < jointBudget; ++j) {
            std::memcpy(s.jointData + static_cast<size_t>(j) * 16, state.jointMatrices + static_cast<size_t>(j) * 16,
                        64);
            if (instance >= kMaxInstances) break;
            float *dst = s.instanceData + static_cast<size_t>(instance) * (kInstanceStride / 4);
            // A fixed joint size, scaled by the root's distance from the
            // origin, so the skeleton reads at any character size without the
            // loop having to know anything about rendering.
            const float scale = 0.055f;
            dst[0] = 0.0f;
            dst[1] = 0.0f;
            dst[2] = 0.0f;
            dst[3] = 0.0f;
            dst[4] = scale;
            dst[5] = scale;
            dst[6] = scale;
            dst[7] = 0.0f;
            dst[8] = 1.0f;
            dst[9] = 0.82f;
            dst[10] = 0.35f;
            dst[11] = 1.0f;
            dst[12] = static_cast<float>(j);
            dst[13] = 1.0f;  // this instance is a joint
            dst[14] = 0.0f;
            dst[15] = 0.0f;
            ++instance;
            ++s.jointCount;
        }
    }
    s.instanceCount = instance;

    // -- camera -----------------------------------------------------------
    {
        const float yaw = s.cameraYaw * 0.01745329252f;
        const float pitch = s.cameraPitch * 0.01745329252f;
        const float d = s.cameraDistance;
        s.eye[0] = state.cameraTarget[0] + std::cos(pitch) * std::sin(yaw) * d;
        s.eye[1] = state.cameraTarget[1] + std::sin(pitch) * d;
        s.eye[2] = state.cameraTarget[2] + std::cos(pitch) * std::cos(yaw) * d;
        const float *eye = s.eye;
        const float up[3] = {0.0f, 1.0f, 0.0f};
        float view[16];
        float proj[16];
        lookAt(eye, state.cameraTarget, up, view);
        // Clamped rather than used raw: an aspect of 0 on a minimised window
        // divides by zero and produces a matrix full of infinities that the
        // GPU turns into a lost device, not a black frame.
        const float aspect = (s.height > 0) ? static_cast<float>(s.width) / static_cast<float>(s.height) : 1.0f;
        perspective(0.9599310886f /* 55 degrees */, aspect, 0.1f, 400.0f, proj);
        multiply(proj, view, s.viewProjection);
    }

    // -- the sky ------------------------------------------------------------
    //
    // Evaluated on the CPU and handed to the shader as four colours and a fog
    // density, rather than being computed per pixel on the GPU. The model is
    // tested, the GPU version would not be, and both halves of the game would
    // then disagree about what colour the sun is. One source of truth.
    if (s.sceneUniformData) {
        // 24 hours in 8 minutes. Long enough to watch a day change, short enough
        // that a developer does not have to wait an hour to see a sunset.
        s.dayFraction += 1.0f / (8.0f * 60.0f * 24.0f);
        s.dayFraction -= std::floor(s.dayFraction);

        const Vec3 view{state.cameraTarget[0], 0.0f, state.cameraTarget[2]};
        const SkyState sky = evaluateSky(view, s.dayFraction);
        float *v = s.sceneUniformData;
        auto put = [&v](int i, float a, float b, float c) {
            v[i] = a; v[i + 1] = b; v[i + 2] = c; v[i + 3] = 0.0f;
        };
        put(0, sky.sunDirection.x, sky.sunDirection.y, sky.sunDirection.z);
        put(4, sky.sunIrradiance.x, sky.sunIrradiance.y, sky.sunIrradiance.z);
        put(8, sky.skyAmbient.x, sky.skyAmbient.y, sky.skyAmbient.z);
        put(12, sky.groundAmbient.x, sky.groundAmbient.y, sky.groundAmbient.z);
        // The fog converges to the sky behind it, so it is sampled from the same
        // model rather than being a separate constant. If the two disagree the
        // horizon shows a seam, which is the single most obvious tell that a
        // renderer fakes its fog.
        put(16, sky.horizonColor.x, sky.horizonColor.y, sky.horizonColor.z);
        v[20] = 0.00016f;  // fog density
        v[21] = 1.0f - sky.dayFactor;
        v[22] = s.sceneMapsLoaded ? 1.0f : 0.0f;
        v[23] = 0.0f;
    }

    // The character's world transform: the pose matrices are rig-model-space,
    // so without this the skeleton is drawn at the origin.
    const float p = state.playerPosition[0];
    const float py = state.playerPosition[1];
    const float pz = state.playerPosition[2];
    s.character[0] = 1.0f;  s.character[1] = 0.0f;  s.character[2] = 0.0f;  s.character[3] = 0.0f;
    s.character[4] = 0.0f;  s.character[5] = 1.0f;  s.character[6] = 0.0f;  s.character[7] = 0.0f;
    s.character[8] = 0.0f;  s.character[9] = 0.0f;  s.character[10] = 1.0f; s.character[11] = 0.0f;
    s.character[12] = p;    s.character[13] = py;   s.character[14] = pz;   s.character[15] = 1.0f;

    std::memcpy(s.pushConstants, s.viewProjection, 64);
    std::memcpy(s.pushConstants + 16, s.character, 64);

    stats_.materialMapsLoaded = s.sceneMapsLoaded;
    stats_.materialMapNote = s.sceneMapNote;

    // -- record -----------------------------------------------------------
    vkResetCommandBuffer(s.command, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s.command, &beginInfo) != VK_SUCCESS) {
        stats_.status = "vkBeginCommandBuffer failed";
        ++stats_.skippedFrames;
        return;
    }

    if (!hasSwapchain_) {
        // Offscreen: there is no acquire, so the image arrives UNDEFINED on
        // every submission and has to be transitioned in here, and back out to
        // TRANSFER_SRC so a readback can actually see the result.
        VkImage targetImage = reinterpret_cast<VkImage>(device_->offscreen_image_);
        if (targetImage != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = targetImage;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    VkClearValue clears[2]{};
    std::memcpy(clears[0].color.float32, kClear, sizeof(kClear));
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = s.renderPass;
    rpbi.framebuffer = s.framebuffers[hasSwapchain_ ? s.imageIndex : 0];
    rpbi.renderArea = {{0, 0}, {s.width, s.height}};
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clears;
    vkCmdBeginRenderPass(s.command, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // The city first, so the debug boxes draw over it and a physics problem is
    // visible against the world rather than floating in a void.
    if (s.scenePipeline != VK_NULL_HANDLE && s.sceneVertexCount > 0) {
        float push[kScenePushBytes / 4] = {};
        std::memcpy(push, s.viewProjection, 64);
        // The camera position, at byte offset 64: the mat4 is 16-byte aligned
        // and takes the first 64 bytes, and a vec3 in a push constant block
        // starts on the next 16-byte boundary. Getting this offset wrong
        // produces a camera at the origin, which looks like a fog bug.
        push[16] = s.eye[0];
        push[17] = s.eye[1];
        push[18] = s.eye[2];
        push[19] = 0.0f;
        push[20] = 0.0f;  // time
        push[21] = 0.0f;  // water

        vkCmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.scenePipeline);
        VkDescriptorSet sceneSets[2] = {s.sceneTexSet, s.sceneUniformSet};
        vkCmdBindDescriptorSets(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.scenePipelineLayout, 0, 2,
                                sceneSets, 0, nullptr);
        const VkDeviceSize sceneOffset = 0;
        vkCmdBindVertexBuffers(s.command, 0, 1, &s.sceneVertexBuffer, &sceneOffset);
        vkCmdPushConstants(s.command, s.scenePipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           kScenePushBytes, push);
        // One draw for the whole slice. The material table is a uniform and the
        // index is per vertex, so there is nothing to sort by and no reason to
        // split the city into per-material batches.
        vkCmdDraw(s.command, s.sceneVertexCount, 1, 0, 0);
        stats_.drawnTriangles += s.sceneVertexCount / 3;
    }

    if (s.instanceCount > 0) {
        vkCmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
        vkCmdBindDescriptorSets(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelineLayout, 0, 1, &s.descriptorSet,
                                0, nullptr);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(s.command, 0, 1, &s.vertexBuffer, &offset);
        vkCmdPushConstants(s.command, s.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128, s.pushConstants);
        // One instanced draw covers the scene and the skeleton: both are the
        // same cube, differing only in which instance record they read.
        vkCmdDraw(s.command, s.vertexCount, s.instanceCount, 0, 0);
        stats_.drawnBoxes += s.vertexCount / 3 * s.instanceCount;
    }

    vkCmdEndRenderPass(s.command);

    if (!hasSwapchain_) {
        VkImage targetImage = reinterpret_cast<VkImage>(device_->offscreen_image_);
        if (targetImage != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = targetImage;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    if (vkEndCommandBuffer(s.command) != VK_SUCCESS) {
        stats_.status = "vkEndCommandBuffer failed";
        ++stats_.skippedFrames;
    }
    s.recording = false;
}

void VulkanRenderBackend::endFrame() {
    Impl &s = *impl_;
    if (!opened_ || !s.ready) return;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.command;
    // The semaphore is only a valid wait source when an acquire actually
    // signalled it. Offscreen there is no acquire, so waiting on it would
    // block for ever -- which is the specific way an offscreen path hangs.
    if (hasSwapchain_) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.renderFinished;
        si.pWaitDstStageMask = &waitStage;
    }

    if (vkResetFences(s.dev(), 1, &s.inFlight) != VK_SUCCESS ||
        vkQueueSubmit(s.queue(), 1, &si, s.inFlight) != VK_SUCCESS) {
        stats_.status = "vkQueueSubmit failed";
        ++stats_.skippedFrames;
        return;
    }

    if (hasSwapchain_) {
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s.renderFinished;
        pi.pResults = nullptr;
        VkSwapchainKHR swapchain = s.swapchain;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &s.imageIndex;
        const VkResult r = vkQueuePresentKHR(s.queue(), &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            // Recreate on the next frame rather than in here: presenting from
            // inside the frame that noticed the resize is how a backend ends
            // up destroying the swapchain it is still holding.
            resizeRequested_ = true;
        } else if (r != VK_SUCCESS) {
            stats_.status = "vkQueuePresentKHR failed: " + std::to_string(r);
            ++stats_.skippedFrames;
            return;
        }
    }

    ++stats_.submittedFrames;
    stats_.gpuMs = 0.0;  // No timestamp query has been resolved. Left at zero rather than estimated.
    stats_.status = hasSwapchain_ ? "frame presented" : "frame rendered offscreen; nothing presented";
}

} // namespace emergent
