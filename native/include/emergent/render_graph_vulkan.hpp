// The Vulkan binding layer for the render graph.
//
// `render_graph.hpp` derives a frame on the CPU: extents, pass order, physical
// slot assignment, transience, and the load/store operations each attachment
// needs. This file turns that description into Vulkan objects, and it is the
// only file in that path that names a Vk type.
//
// ## Why the split is here and not one layer deeper
//
// The graph uses its own `Format` rather than `VkFormat`. That is not
// fastidiousness: it means `render_graph.hpp` compiles with no Vulkan headers,
// so the logic that decides barrier placement and pass order is checked by
// ctest on a machine with no driver. This file is the translation, and a
// translation is the part that is cheap to get right and expensive to debug --
// a wrong `Format` mapping produces a correctly shaped frame of the wrong
// colour space, which no assertion on the CPU side can catch.
//
// So the functions here are split in two groups. The pure ones (`toVkFormat`,
// `toVkImageUsage`, `attachmentLoadStoreOps`, `deriveDescriptorLayout`,
// `planBarriers`) return Vulkan *enums* and take no device, and are unit
// tested directly. The stateful ones (`GraphResources`, `GraphRenderPasses`)
// own GPU objects and are exercised on a machine that has a driver.
//
// ## What the caches do and do not own
//
// The graph assigns every resource a *physical index*, and two resources that
// can never be live at the same time deliberately share one. `GraphResources`
// therefore allocates one `VkImage` per physical index, not per logical
// resource: aliasing is already decided, and re-deriving it here would be a
// second implementation of the same rule that could disagree with the first.
// Transient slots are additionally marked so the allocator can note that their
// memory is expected to be reused, which is what a tile-based GPU can exploit
// and a discrete GPU cannot.
//
// Nothing here is a cache in the sense of "may be recomputed". A slot's
// description is the graph's, and a mismatch is an error rather than a
// reallocation, because silently reallocating a resource other code holds a
// handle to is how a frame ends up sampling freed memory three passes later.

#pragma once

#include "emergent/render_graph.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// volk rather than <vulkan/vulkan.h>. Both provide the types, but volk also
// redirects every `vk*` entry point through a function pointer the loader
// fills in, which is how the rest of this renderer reaches Vulkan: the symbols
// are resolved at `volkLoadDevice` rather than linked, so a machine with no
// driver still builds and links. Including <vulkan/vulkan.h> here instead
// would produce a library that compiles and then fails to link with a wall of
// undefined `vkCreate*` references.
#include <volk.h>

namespace emergent {

// ---------------------------------------------------------------------------
// Pure translation
// ---------------------------------------------------------------------------

/** The graph's format as a Vulkan format. `Undefined` maps to `VK_FORMAT_UNDEFINED`. */
VkFormat toVkFormat(Format format) noexcept;

/** True for a format the translation table actually covers. */
bool isSupportedVkFormat(Format format) noexcept;

/** The image aspect a format occupies: colour, depth, stencil, or both. */
VkImageAspectFlags toVkAspect(Format format) noexcept;

/** True when a format may be used as a depth/stencil attachment. */
bool isDepthStencilFormat(Format format) noexcept;

/** True when a format must be created with an sRGB view. */
bool isSrgbFormat(Format format) noexcept;

/**
 * The image usage flags a physical slot needs.
 *
 * The graph records what a resource is *used for* in `image_usage`; this adds
 * the attachment bits implied by being an attachment at all, because a depth
 * target that is never sampled still needs `SAMPLED` to be readable as a
 * texture in the lighting pass, and forgetting that is a validation error at
 * `vkCreateImageView` rather than at draw time.
 */
VkImageUsageFlags toVkImageUsage(const ResourceDimensions &dim) noexcept;

/** Buffer usage flags for a physical slot. */
VkBufferUsageFlags toVkBufferUsage(const ResourceDimensions &dim) noexcept;

/**
 * The load and store operations one attachment needs.
 *
 * `needsLoad`/`needsStore` are the graph's decision -- it knows which passes
 * read a resource and whether a previous frame's content is still live -- and
 * this only maps them. A `DONT_CARE` store on a persistent target is the
 * single most common way to produce a frame that looks fine until the
 * attachment is read next frame, which is why the caller passes the graph's
 * answer through unchanged rather than second-guessing it here.
 */
void attachmentLoadStoreOps(bool needsLoad, bool needsStore, VkAttachmentLoadOp &loadOp,
                            VkAttachmentStoreOp &storeOp) noexcept;

/** Bytes one texel of a format occupies. Returns 0 for an unsupported format. */
uint32_t toVkFormatBlockSize(Format format) noexcept;

/** The pipeline barrier stage mask a pass on `queue` runs at. */
VkPipelineStageFlags toVkPipelineStage(RenderGraphQueueFlagBits queue) noexcept;

/** The access mask implied by what a pass declares it touched. */
VkAccessFlags toVkAccessForTexture(const RenderTextureResource &resource, bool isWrite) noexcept;
VkAccessFlags toVkAccessForBuffer(const RenderBufferResource &resource, bool isWrite) noexcept;

/**
 * The image layout a resource needs for one particular access.
 *
 * Takes `asAttachment` rather than deriving it, because a layout is a property
 * of *how a resource is being used right now*, not of what the resource is.
 * The first version of this function returned a layout chosen only from the
 * resource's own description, which meant the layout could never differ between
 * two passes -- so the planner's carry-forward was dead code and a texture
 * written as a colour attachment was left in `COLOR_ATTACHMENT_OPTIMAL` for the
 * pass that then sampled it. That is a validation error, not a slow frame.
 *
 * A resource read as a texture wants `SHADER_READ_ONLY_OPTIMAL`; written as an
 * attachment it wants the colour or depth-stencil layout; a transient resource
 * wants `GENERAL` throughout, since it may be both read and written inside one
 * pass and has no layout it can stay in between them.
 */
VkImageLayout toVkImageLayout(const ResourceDimensions &dim, bool isTransient,
                               bool asAttachment) noexcept;

// ---------------------------------------------------------------------------
// Descriptor layout
// ---------------------------------------------------------------------------

/** One binding the frame needs. */
struct DescriptorBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uint32_t count = 1;
    /** The graph resource this binding refers to, for reporting. */
    std::string resource;
};

/**
 * The descriptor sets a baked frame needs, grouped by set.
 *
 * The graph already knows every buffer and texture a pass touches. Deriving the
 * layout from that rather than hand-declaring one per pipeline is what makes a
 * new pass a declaration rather than an edit to a layout that five other passes
 * share -- and it is the part of the Forge's resource system that actually pays
 * for itself on a project this size.
 */
struct DescriptorLayoutPlan {
    std::vector<std::vector<DescriptorBinding>> sets;

    /** Total bindings across all sets. */
    uint32_t totalBindings() const noexcept;

    /** A one-line-per-set summary, for a log or a test failure. */
    std::string describe() const;
};

/**
 * Derive descriptor sets for a baked graph.
 *
 * Buffers and sampled textures each occupy a binding. Storage images become
 * storage-image descriptors; everything else that is a texture is a combined
 * image sampler. Bindings are numbered per set in first-use order, which is
 * stable for a fixed frame declaration and therefore usable as a cache key.
 *
 * Throws if the graph has not been baked: a layout derived from an unbaked
 * graph would silently omit every resource the graph has not yet discovered.
 */
DescriptorLayoutPlan deriveDescriptorLayout(const RenderGraph &graph);

// ---------------------------------------------------------------------------
// Barrier planning
// ---------------------------------------------------------------------------

/** One barrier to emit between two passes. */
struct BarrierPlan {
    uint32_t srcPass = 0;
    uint32_t dstPass = 0;
    PassDependency::Kind kind = PassDependency::Kind::ReadAfterWrite;
    /** The graph resource being transitioned. */
    std::string resource;
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    VkAccessFlags dstAccess = 0;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool isBuffer = false;
};

/**
 * Derive every image-layout transition and buffer hazard in a baked frame.
 *
 * The graph's ordering is a statement about *when*; a Vulkan barrier is a
 * statement about *what changed*, and the second cannot be written without
 * knowing the layout the resource was left in by the pass before. So this
 * walks the flattened order once, carrying the current layout of each resource
 * forward, and emits a transition wherever the layout it needs differs from
 * the one it has.
 *
 * A resource first written by a pass starts in `UNDEFINED`, which is what
 * lets the driver skip the copy: contents of a freshly-written attachment are
 * discarded by the load operation anyway.
 */
std::vector<BarrierPlan> planBarriers(const RenderGraph &graph);

// ---------------------------------------------------------------------------
// GPU object caches
// ---------------------------------------------------------------------------

class VulkanBackend;

/**
 * Physical storage for a baked graph: one image or buffer per physical slot.
 *
 * Construction is explicit about the device because these objects outlive the
 * graph they were created for -- a resize re-bakes the frame, and the next
 * frame must find the existing slot rather than recreate every image.
 */
class GraphResources {
public:
    GraphResources() = default;
    ~GraphResources();
    GraphResources(const GraphResources &) = delete;
    GraphResources &operator=(const GraphResources &) = delete;

    /**
     * Create or resize storage so it matches a baked graph.
     *
     * Existing slots whose description is unchanged are kept; changed ones are
     * destroyed and rebuilt. Returns false and fills `error()` on a Vulkan
     * failure, rather than throwing, because this runs during frame setup
     * where an exception has nowhere useful to go.
     */
    bool create(VulkanBackend &device, const RenderGraph &graph, const std::string &assetDirectory);

    /** Destroy every GPU object. Safe to call twice. */
    void destroy() noexcept;

    /** The view for a physical slot, or `VK_NULL_HANDLE`. */
    VkImageView imageView(uint32_t physicalIndex) const noexcept;
    VkImage image(uint32_t physicalIndex) const noexcept;

    /** The buffer for a physical slot, or `VK_NULL_HANDLE`. */
    VkBuffer buffer(uint32_t physicalIndex) const noexcept;
    VkDeviceMemory bufferMemory(uint32_t physicalIndex) const noexcept;

    uint32_t physicalCount() const noexcept { return static_cast<uint32_t>(slots_.size()); }

    const std::string &error() const noexcept { return error_; }

private:
    struct Slot {
        ResourceDimensions dim;
        bool isBuffer = false;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    };

    /**
     * The device and physical device behind a `VulkanBackend`.
     *
     * Members rather than free functions because `VulkanBackend` keeps them
     * private and only friends the two cache classes -- friendship does not
     * extend to a helper in an anonymous namespace, which is a mistake worth
     * recording because the failure is a confusing access error rather than an
     * obvious "you cannot call that from here".
     */
    static VkDevice deviceOf(VulkanBackend &backend) noexcept;
    static VkPhysicalDevice physicalOf(VulkanBackend &backend) noexcept;

    std::vector<Slot> slots_;
    std::string error_;
};

/**
 * Render passes and framebuffers for a baked graph.
 *
 * A pass the graph merged with another becomes a *subpass*, not a second
 * `vkCmdBeginRenderPass`. The graph decided the merge -- an output declared as
 * reading an input in the same pass -- so this only has to honour
 * `getPhysicalPassIndex()`.
 */
class GraphRenderPasses {
public:
    GraphRenderPasses() = default;
    ~GraphRenderPasses();
    GraphRenderPasses(const GraphRenderPasses &) = delete;
    GraphRenderPasses &operator=(const GraphRenderPasses &) = delete;

    /** Create render passes and framebuffers matching a baked graph. */
    bool create(VulkanBackend &device, const RenderGraph &graph, const GraphResources &resources,
                const std::vector<VkImageView> &swapchainViews, VkFormat swapchainFormat);

    void destroy() noexcept;

    /** The framebuffer for a physical pass index. */
    VkFramebuffer framebuffer(uint32_t physicalPassIndex) const noexcept;
    VkRenderPass renderPass(uint32_t physicalPassIndex) const noexcept;

    uint32_t physicalPassCount() const noexcept { return static_cast<uint32_t>(passes_.size()); }

    const std::string &error() const noexcept { return error_; }

private:
    struct PhysicalPass {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> framebuffers;
    };

    /** See `GraphResources::deviceOf` -- same reason, same fix. */
    static VkDevice deviceOf(VulkanBackend &backend) noexcept;

    std::vector<PhysicalPass> passes_;
    std::string error_;
};

} // namespace emergent
