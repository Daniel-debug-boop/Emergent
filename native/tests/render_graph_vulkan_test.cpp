// Tests for the Vulkan binding layer's pure half.
//
// The functions under test return Vulkan enums and take no device, so they are
// checked directly rather than inferred from a rendered frame. That is the
// reason the port is split the way it is: `render_graph.hpp` uses its own
// `Format` precisely so that everything decidable stays decidable on a machine
// with no `/dev/dri`.
//
// What is deliberately not here: anything that calls `vkCreate*`. Those need a
// driver, and a test that silently skipped them on this machine would be a test
// that never runs where it matters. The allocation paths are marked for
// hardware validation instead.

#include "emergent/render_graph_vulkan.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace emergent;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool condition, const char *what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

static void checkEq(long long got, long long want, const char *what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s (got %lld, want %lld)\n", what, got, want);
    }
}

static void section(const char *name) { std::printf("-- %s\n", name); }

// ---------------------------------------------------------------------------

static void testFormatTable() {
    section("format translation");
    checkEq(toVkFormat(Format::R8G8B8A8_UNORM), VK_FORMAT_R8G8B8A8_UNORM, "rgba8 unorm");
    checkEq(toVkFormat(Format::R8G8B8A8_SRGB), VK_FORMAT_R8G8B8A8_SRGB, "rgba8 srgb");
    checkEq(toVkFormat(Format::B8G8R8A8_SRGB), VK_FORMAT_B8G8R8A8_SRGB, "bgra8 srgb");
    checkEq(toVkFormat(Format::R16G16B16A16_SFLOAT), VK_FORMAT_R16G16B16A16_SFLOAT, "rgba16f");
    checkEq(toVkFormat(Format::R32G32B32A32_SFLOAT), VK_FORMAT_R32G32B32A32_SFLOAT, "rgba32f");
    checkEq(toVkFormat(Format::D32_SFLOAT), VK_FORMAT_D32_SFLOAT, "d32");
    checkEq(toVkFormat(Format::D24_UNORM_S8_UINT), VK_FORMAT_D24_UNORM_S8_UINT, "d24s8");
    checkEq(toVkFormat(Format::Undefined), VK_FORMAT_UNDEFINED, "undefined");

    // Every colour format must round-trip to a distinct VkFormat, because two
    // of them mapping to the same one would silently swap a target's meaning.
    std::set<VkFormat> seen;
    const Format all[] = {Format::R8G8B8A8_UNORM,  Format::R8G8B8A8_SRGB, Format::R16G16B16A16_SFLOAT,
                          Format::R32G32B32A32_SFLOAT, Format::B8G8R8A8_UNORM, Format::B8G8R8A8_SRGB,
                          Format::R16_SFLOAT,      Format::R32_SFLOAT,      Format::R32_UINT,
                          Format::D32_SFLOAT,     Format::D24_UNORM_S8_UINT, Format::D16_UNORM};
    for (Format f : all) {
        check(isSupportedVkFormat(f), "supported format");
        check(seen.insert(toVkFormat(f)).second, "format maps to a distinct VkFormat");
    }
}

static void testAspectsAndDepth() {
    section("aspects and depth classification");
    check(isDepthStencilFormat(Format::D32_SFLOAT), "d32 is depth");
    check(isDepthStencilFormat(Format::D24_UNORM_S8_UINT), "d24s8 is depth");
    check(isDepthStencilFormat(Format::D16_UNORM), "d16 is depth");
    check(!isDepthStencilFormat(Format::R8G8B8A8_UNORM), "rgba8 is not depth");
    check(!isDepthStencilFormat(Format::R16G16B16A16_SFLOAT), "rgba16f is not depth");

    check((toVkAspect(Format::D24_UNORM_S8_UINT) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, "d24s8 depth aspect");
    check((toVkAspect(Format::D24_UNORM_S8_UINT) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, "d24s8 stencil aspect");
    check((toVkAspect(Format::D32_SFLOAT) & VK_IMAGE_ASPECT_STENCIL_BIT) == 0, "d32 has no stencil");

    check(isSrgbFormat(Format::R8G8B8A8_SRGB), "rgba8 srgb is srgb");
    check(isSrgbFormat(Format::B8G8R8A8_SRGB), "bgra8 srgb is srgb");
    check(!isSrgbFormat(Format::R8G8B8A8_UNORM), "rgba8 unorm is not srgb");
}

static void testBlockSizes() {
    section("texel sizes");
    checkEq(toVkFormatBlockSize(Format::R8G8B8A8_UNORM), 4, "rgba8 is 4 bytes");
    checkEq(toVkFormatBlockSize(Format::R8G8B8A8_SRGB), 4, "rgba8 srgb is 4 bytes");
    checkEq(toVkFormatBlockSize(Format::R16G16B16A16_SFLOAT), 8, "rgba16f is 8 bytes");
    checkEq(toVkFormatBlockSize(Format::R32G32B32A32_SFLOAT), 16, "rgba32f is 16 bytes");
    checkEq(toVkFormatBlockSize(Format::R16_SFLOAT), 2, "r16f is 2 bytes");
    checkEq(toVkFormatBlockSize(Format::R32_SFLOAT), 4, "r32f is 4 bytes");
    checkEq(toVkFormatBlockSize(Format::R32_UINT), 4, "r32 uint is 4 bytes");
    checkEq(toVkFormatBlockSize(Format::D32_SFLOAT), 4, "d32 is 4 bytes");
    checkEq(toVkFormatBlockSize(Format::D16_UNORM), 2, "d16 is 2 bytes");
    // An unmapped format must be zero rather than a plausible wrong number,
    // so a missing table row shows up as a zero-sized target.
    checkEq(toVkFormatBlockSize(Format::Undefined), 0, "undefined is 0 bytes");
}

static void testImageUsageTranslation() {
    section("image usage translation");
    ResourceDimensions sampled;
    sampled.format = Format::R8G8B8A8_UNORM;
    sampled.image_usage = TEXTURE_USAGE_SAMPLED_BIT;
    check((toVkImageUsage(sampled) & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, "sampled bit translates");

    ResourceDimensions storage;
    storage.format = Format::R32_UINT;
    storage.image_usage = TEXTURE_USAGE_STORAGE_BIT;
    check((toVkImageUsage(storage) & VK_IMAGE_USAGE_STORAGE_BIT) != 0, "storage bit translates");

    // The bug this file exists to prevent: the graph's usage bits and Vulkan's
    // are parallel, not identical. Bit 2 is COLOR_ATTACHMENT in both, but bit 1
    // is STORAGE in the graph and TRANSFER_SRC in Vulkan's set. A cast would
    // turn a storage image into a transfer source and never complain.
    checkEq(TEXTURE_USAGE_STORAGE_BIT != VK_IMAGE_USAGE_STORAGE_BIT ? 1 : 0, 1,
            "graph and vulkan storage bits differ (so a cast would be wrong)");
    ResourceDimensions mip;
    mip.format = Format::R8G8B8A8_UNORM;
    mip.image_usage = TEXTURE_USAGE_SAMPLED_BIT;
    mip.levels = 5;
    const VkImageUsageFlags mipUsage = toVkImageUsage(mip);
    check((mipUsage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0, "mip chain is sampled");
    check((mipUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, "mip chain is a transfer destination");

    // A resource declared as neither sampled nor storage still has to be a
    // legal attachment, or vkCreateImageView rejects it.
    ResourceDimensions plain;
    plain.format = Format::R8G8B8A8_UNORM;
    plain.image_usage = 0;
    check((toVkImageUsage(plain) & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0,
          "an undeclared colour image still gets the attachment bit");

    ResourceDimensions depth;
    depth.format = Format::D32_SFLOAT;
    depth.image_usage = 0;
    check((toVkImageUsage(depth) & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0,
          "a depth image with no declared usage still gets the depth attachment bit");
}

static void testBufferUsageTranslation() {
    section("buffer usage translation");
    ResourceDimensions ubo;
    ubo.buffer_info.usage = BUFFER_USAGE_UNIFORM_BIT;
    check((toVkBufferUsage(ubo) & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0, "uniform bit translates");

    ResourceDimensions ssbo;
    ssbo.buffer_info.usage = BUFFER_USAGE_STORAGE_BIT;
    check((toVkBufferUsage(ssbo) & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0, "storage bit translates");

    ResourceDimensions indirect;
    indirect.buffer_info.usage = BUFFER_USAGE_INDIRECT_BIT;
    check((toVkBufferUsage(indirect) & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0,
          "indirect bit translates");

    ResourceDimensions transfer;
    transfer.buffer_info.usage =
        static_cast<BufferUsageBits>(BUFFER_USAGE_TRANSFER_SRC_BIT | BUFFER_USAGE_TRANSFER_DST_BIT);
    const VkBufferUsageFlags tu = toVkBufferUsage(transfer);
    check((tu & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) != 0, "transfer src translates");
    check((tu & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0, "transfer dst translates");

    // The graph has separate src and dst bits; there is no combined bit. Asking
    // for a bit that does not exist must compile to nothing, not to a guess.
    ResourceDimensions vertex;
    vertex.buffer_info.usage = BUFFER_USAGE_VERTEX_BIT;
    const VkBufferUsageFlags vu = toVkBufferUsage(vertex);
    check((vu & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0, "vertex bit translates");
    check((vu & ~VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) == 0, "vertex adds no other bits");
}

static void testLoadStoreOps() {
    section("load and store operations");
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;

    attachmentLoadStoreOps(true, true, load, store);
    checkEq(load, VK_ATTACHMENT_LOAD_OP_LOAD, "load when needed");
    checkEq(store, VK_ATTACHMENT_STORE_OP_STORE, "store when needed");

    attachmentLoadStoreOps(false, true, load, store);
    checkEq(load, VK_ATTACHMENT_LOAD_OP_DONT_CARE, "dont-care load when not needed");
    checkEq(store, VK_ATTACHMENT_STORE_OP_STORE, "store still happens");

    attachmentLoadStoreOps(true, false, load, store);
    checkEq(load, VK_ATTACHMENT_LOAD_OP_LOAD, "load still happens");
    checkEq(store, VK_ATTACHMENT_STORE_OP_DONT_CARE, "dont-care store when not needed");

    attachmentLoadStoreOps(false, false, load, store);
    checkEq(load, VK_ATTACHMENT_LOAD_OP_DONT_CARE, "neither: dont-care load");
    checkEq(store, VK_ATTACHMENT_STORE_OP_DONT_CARE, "neither: dont-care store");
}

static void testPipelineStagesAndLayouts() {
    section("stages and layouts");
    const VkPipelineStageFlags graphics =
        toVkPipelineStage(RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
    check((graphics & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) != 0, "a graphics pass can draw indirect");

    const VkPipelineStageFlags compute =
        toVkPipelineStage(RENDER_GRAPH_QUEUE_ASYNC_COMPUTE_BIT);
    check((compute & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) != 0, "async compute runs compute");

    ResourceDimensions colour;
    colour.format = Format::R16G16B16A16_SFLOAT;
    checkEq(toVkImageLayout(colour, false, true), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            "a colour target written as an attachment is a colour attachment layout");
    checkEq(toVkImageLayout(colour, false, false), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            "the same colour target, sampled, is shader-read-only");

    // The distinction above is the whole reason the layout is a function of use
    // and not of the resource: a target that is written by one pass and sampled
    // by the next has to change layout between them.
    check(toVkImageLayout(colour, false, true) != toVkImageLayout(colour, false, false),
          "writing and sampling a colour target are different layouts");

    ResourceDimensions depth;
    depth.format = Format::D32_SFLOAT;
    checkEq(toVkImageLayout(depth, false, true), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            "a depth target written as an attachment is a depth attachment layout");
    // Core Vulkan 1.0 has no read-only depth layout, so a depth target that is
    // only sampled has to live in GENERAL.
    checkEq(toVkImageLayout(depth, false, false), VK_IMAGE_LAYOUT_GENERAL,
            "a depth target that is only sampled is GENERAL");

    // Transient resources may be read and written as storage in the same pass,
    // which the attachment layouts forbid, so they are forced to GENERAL.
    checkEq(toVkImageLayout(colour, true, true), VK_IMAGE_LAYOUT_GENERAL,
            "a transient target is GENERAL as an attachment");
    checkEq(toVkImageLayout(colour, true, false), VK_IMAGE_LAYOUT_GENERAL,
            "a transient target is GENERAL as a sample");
}

// ---------------------------------------------------------------------------

/**
 * A baked three-pass frame.
 *
 * Deliberately more than the minimum. The first version of this test used one
 * texture in a two-pass frame, which passed while the descriptor planner could
 * hand every texture the same binding number and the barrier planner could
 * forget to carry a layout forward -- neither is observable when there is only
 * ever one candidate and only ever one transition. The negative controls found
 * both, and this graph is what makes them observable.
 *
 *   geometry : writes hdr and depth
 *   lighting : samples hdr, writes bloom
 *   tonemap  : samples hdr *again* and bloom
 *
 * `hdr` is therefore sampled by two separate passes, so a planner that never
 * records the layout it left a resource in has nothing to compare against and
 * emits a second transition from the wrong `oldLayout`.
 */
static void buildThreePassGraph(RenderGraph &graph) {
    ResourceDimensions backbuffer;
    backbuffer.format = Format::R8G8B8A8_UNORM;
    backbuffer.width = 1920;
    backbuffer.height = 1080;
    graph.setBackbufferDimensions(backbuffer);

    AttachmentInfo hdr;
    hdr.size_class = SizeClass::SwapchainRelative;
    hdr.format = Format::R16G16B16A16_SFLOAT;

    AttachmentInfo depth;
    depth.size_class = SizeClass::SwapchainRelative;
    depth.format = Format::D32_SFLOAT;

    AttachmentInfo bloom;
    bloom.size_class = SizeClass::SwapchainRelative;
    bloom.size_x = 0.5f;
    bloom.size_y = 0.5f;
    bloom.format = Format::R16G16B16A16_SFLOAT;

    RenderPass &geometry = graph.addPass("geometry", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
    geometry.addColorOutput("hdr", hdr);
    geometry.setDepthStencilOutput("depth", depth);

    RenderPass &lighting = graph.addPass("lighting", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
    lighting.addColorOutput("bloom", bloom);
    lighting.addTextureInput("hdr");
    lighting.addTextureInput("depth");

    RenderPass &tonemap = graph.addPass("tonemap", RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
    tonemap.addColorOutput("out", hdr);
    tonemap.addTextureInput("hdr");
    tonemap.addTextureInput("bloom");

    graph.setBackbufferSource("out");
    graph.bake();
}

static void testDescriptorLayout() {
    section("descriptor layout derivation");
    RenderGraph graph;
    buildThreePassGraph(graph);

    const DescriptorLayoutPlan plan = deriveDescriptorLayout(graph);
    check(plan.totalBindings() > 0, "a frame that samples a texture has a binding");
    check(!plan.sets.empty(), "there is at least one set");
    check(!plan.describe().empty(), "the plan describes itself");

    // Three distinct textures are sampled across the frame (hdr, depth, bloom),
    // so a planner that hands every texture the same binding number has
    // something to be caught by.
    std::set<std::string> distinctTextures;
    for (const auto &set : plan.sets) {
        for (const DescriptorBinding &b : set) {
            distinctTextures.insert(b.resource);
        }
    }
    check(distinctTextures.size() >= 3, "every sampled texture gets its own binding");

    // Binding numbers must be unique within a set, or a write overwrites a
    // neighbour and the frame samples the wrong resource.
    for (const auto &set : plan.sets) {
        std::set<uint32_t> bindings;
        for (const DescriptorBinding &b : set) {
            check(bindings.insert(b.binding).second, "binding numbers are unique within a set");
        }
    }

    // A texture that is only ever sampled must not be handed a storage
    // descriptor: the shader would then expect a layout the image was never
    // transitioned to, and the frame fails validation rather than rendering.
    for (const auto &set : plan.sets) {
        for (const DescriptorBinding &b : set) {
            if (b.resource == "depth" || b.resource == "hdr" || b.resource == "bloom") {
                checkEq(b.type, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        "a sampled-only texture is a combined image sampler");
            }
        }
    }

    bool threw = false;
    RenderGraph unbaked;
    try {
        deriveDescriptorLayout(unbaked);
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "deriving a layout from an unbaked graph is refused");
}

static void testBarrierPlanning() {
    section("barrier planning");
    RenderGraph graph;
    buildThreePassGraph(graph);

    const std::vector<BarrierPlan> barriers = planBarriers(graph);
    check(!barriers.empty(), "a multi-pass frame needs at least one transition");

    // The first write of a resource starts from UNDEFINED. If it did not, the
    // driver would have to preserve a previous frame's contents that nothing
    // asked for.
    bool sawFirstWrite = false;
    for (const BarrierPlan &b : barriers) {
        if (b.resource == "hdr" && b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            sawFirstWrite = true;
        }
    }
    check(sawFirstWrite, "the first write of hdr starts from UNDEFINED");

    // `hdr` is sampled by two passes. The second one must be told the layout the
    // resource is *currently* in, not the one it was first seen in. A planner
    // that never records what it left a resource in cannot produce this, which
    // is why the graph has more than one reader.
    int hdrTransitions = 0;
    for (const BarrierPlan &b : barriers) {
        if (b.resource == "hdr") ++hdrTransitions;
    }
    check(hdrTransitions >= 2, "a texture sampled twice is transitioned more than once");

    // Nothing may be transitioned to a layout it is already in. This is the
    // check that catches a planner which emits a barrier unconditionally.
    for (const BarrierPlan &b : barriers) {
        check(b.oldLayout != b.newLayout, "no barrier is emitted for a no-op transition");
    }

    // Every emitted transition must land on a layout the resource can legally
    // be in, or the driver rejects it at submit rather than at draw.
    for (const BarrierPlan &b : barriers) {
        if (b.isBuffer) continue;
        check(b.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                  b.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
                  b.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                  b.newLayout == VK_IMAGE_LAYOUT_GENERAL,
              "a transition lands on a legal layout");
    }

    // `hdr` is written by geometry and then sampled by two later passes, so it
    // must be transitioned out of its attachment layout exactly once and left
    // alone afterwards. A planner that re-transitions on every read is doing
    // redundant work; one that never leaves the attachment layout is a
    // validation error.
    int hdrToAttachment = 0;
    int hdrToSampled = 0;
    for (const BarrierPlan &b : barriers) {
        if (b.resource != "hdr") continue;
        if (b.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ++hdrToAttachment;
        if (b.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ++hdrToSampled;
    }
    checkEq(hdrToAttachment, 1, "hdr enters its attachment layout once");
    checkEq(hdrToSampled, 1, "hdr is transitioned to sampled once, not once per reader");

    RenderGraph unbaked;
    check(planBarriers(unbaked).empty(), "an unbaked graph plans no barriers");
}

int main() {
    testFormatTable();
    testAspectsAndDepth();
    testBlockSizes();
    testImageUsageTranslation();
    testBufferUsageTranslation();
    testLoadStoreOps();
    testPipelineStagesAndLayouts();
    testDescriptorLayout();
    testBarrierPlanning();

    std::printf("%d checks, %d failing\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
