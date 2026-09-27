#pragma once

// The render graph: resource bookkeeping, hazard derivation, sizing and pass
// ordering, with no Vulkan in it.
//
// ## What this is for
//
// The native renderer is currently one 2191-line translation unit
// (`vulkan_render.cpp`) whose `Impl` struct holds 102 members and whose `open()`
// is 582 lines of hand-written resource creation paired with hand-written
// destruction. Every resource added later is another pair the author has to
// remember to keep in step, and a missed destroy is a leak that only shows up
// under a validation layer this project cannot currently run.
//
// A render graph inverts that. A frame is *declared* — which pass writes which
// target, which pass reads it, which target is the same memory as another — and
// the graph derives what the hand-written code was doing by hand: barrier
// placement, image layouts, load/store operations, physical image assignment,
// and the order passes run in. Declaring is short; deriving is the part that is
// easy to get subtly wrong, so it is the part that belongs in one tested place
// rather than scattered through a 2000-line file.
//
// The shape follows Hans-Kristian Arntzen's Granite (`renderer/render_graph.hpp`,
// MIT). The core idea is one line: a resource records *which passes read it and
// which passes write it*, and everything else falls out of those two sets.
//
// ## Why there is no Vulkan here
//
// This machine has no `/dev/dri` and no `/usr/share/vulkan/icd.d`, so a Vulkan
// binding layer could be written but never executed. Splitting the logic out
// means the part that can be *proved* — sizing arithmetic, hazard derivation,
// ordering, transient eligibility — is provable headlessly, and the untestable
// binding is reduced to a thin translation from these types to `VkFormat` and
// `VkImage`.
//
// The vocabulary is deliberately CPU-only. `Format` is not `VkFormat`; the
// binding layer maps one to the other, and keeping them separate means this
// header compiles with no Vulkan headers at all.
//
// ## What "transient" means, precisely
//
// Transient is not "scratch" and it is not "temporary". It is a single,
// checkable claim: **the resource's entire lifetime falls inside one render
// pass.** Such a resource can live in a render pass's own transient memory and
// never touch a descriptor, an allocation or a barrier between passes.
//
// That is why the rule is derived rather than declared. A caller asking for
// "transient" is expressing a hope; the graph checks whether it happens to be
// true and silently demotes the resource to persistent when it is not. Getting
// this wrong in the other direction — treating a cross-pass resource as
// transient — destroys the contents of a target a later pass is about to read,
// which is the single worst class of bug in a renderer and one that produces a
// plausible-looking image rather than an error.

#include <cstdint>
#include <map>
#include <utility>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace emergent {

class RenderGraph;

/**
 * Pixel formats, named as Vulkan names them.
 *
 * A deliberately tiny subset. The point is not to be a general format
 * registry but to carry the two properties the graph's logic actually branches
 * on — whether a format is depth/stencil, and whether it can be an attachment —
 * without dragging `vulkan/vulkan.h` into a header that is meant to compile
 * anywhere.
 *
 * The binding layer maps these onto `VkFormat` values.
 */
enum class Format : uint8_t {
    Undefined = 0,

    // Colour.
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    R16G16B16A16_SFLOAT,
    R32G32B32A32_SFLOAT,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    R16_SFLOAT,
    R32_SFLOAT,
    R32_UINT,

    // Depth / stencil.
    D32_SFLOAT,
    D24_UNORM_S8_UINT,
    D16_UNORM,
};

/** True when the format carries a depth or stencil aspect. */
bool formatHasDepthOrStencil(Format format) noexcept;

/**
 * How a resource's extent is decided.
 *
 * The three cases exist because a render graph has to describe a fullscreen
 * HDR target, a half-resolution bloom pyramid and a fixed 1024x1024 lookup
 * table in the same frame, and hardcoding any of those as a constant is what
 * makes dynamic resolution a rewrite rather than a data change.
 */
enum class SizeClass : uint8_t {
    /** Fixed pixel extent, taken from `AttachmentInfo::size_x/y/z`. */
    Absolute,
    /** A fraction of the swapchain extent. This is the dynamic-resolution lever. */
    SwapchainRelative,
    /** A fraction of another resource's resolved extent, named by `size_relative_name`. */
    InputRelative,
};

/** Attachment flags, as declared by the caller. */
enum AttachmentFlagBits : uint32_t {
    /**
     * The resource outlives the frame.
     *
     * Set by default, because the *safe* answer to "will this survive?" has to
     * be yes: a resource that is persistent when it did not need to be merely
     * costs memory, while one that is transient when it should not be corrupts
     * a frame.
     */
    ATTACHMENT_INFO_PERSISTENT_BIT = 1u << 0,
    /** The resource may be read through an sRGB view of the same memory. */
    ATTACHMENT_INFO_UNORM_SRGB_ALIAS_BIT = 1u << 1,
    /** The resource may be rotated to match the swapchain transform. */
    ATTACHMENT_INFO_SUPPORTS_PREROTATE_BIT = 1u << 2,
    /** The graph should generate mip levels for the resource. */
    ATTACHMENT_INFO_MIPGEN_BIT = 1u << 3,
};

/** Bitwise-or of any `AttachmentFlagBits` or internal flag below. */
using AttachmentInfoFlags = uint32_t;

/**
 * Internal attachment flags, derived by the graph.
 *
 * Kept distinct from the declared flags above so that "the caller said this"
 * and "the graph worked this out" are never the same bit, and so a derived
 * flag can never be confused with a declared one.
 *
 * These are plain constants rather than a second enum because they are
 * routinely OR-ed and inverted together with the enum above, and a bitwise
 * operation between two different enum types is a diagnostic on some
 * toolchains for a mistake that cannot occur here.
 */
constexpr AttachmentInfoFlags ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT = 1u << 16;
constexpr AttachmentInfoFlags ATTACHMENT_INFO_INTERNAL_PROXY_BIT = 1u << 17;

/** Texture usage bits. Mirrors the `VK_IMAGE_USAGE_*` set the graph branches on. */
enum TextureUsageBits : uint32_t {
    TEXTURE_USAGE_SAMPLED_BIT = 1u << 0,
    TEXTURE_USAGE_STORAGE_BIT = 1u << 1,
    TEXTURE_USAGE_COLOR_ATTACHMENT_BIT = 1u << 2,
    TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 1u << 3,
    TEXTURE_USAGE_INPUT_ATTACHMENT_BIT = 1u << 4,
};

/** Buffer usage bits. Mirrors the `VK_BUFFER_USAGE_*` set the graph branches on. */
enum BufferUsageBits : uint32_t {
    BUFFER_USAGE_UNIFORM_BIT = 1u << 0,
    BUFFER_USAGE_STORAGE_BIT = 1u << 1,
    BUFFER_USAGE_TRANSFER_SRC_BIT = 1u << 2,
    BUFFER_USAGE_TRANSFER_DST_BIT = 1u << 3,
    BUFFER_USAGE_INDEX_BIT = 1u << 4,
    BUFFER_USAGE_VERTEX_BIT = 1u << 5,
    BUFFER_USAGE_INDIRECT_BIT = 1u << 6,
};

/** Which queue a pass records into, or a resource is used from. */
enum RenderGraphQueueFlagBits : uint32_t {
    RENDER_GRAPH_QUEUE_GRAPHICS_BIT = 1u << 0,
    RENDER_GRAPH_QUEUE_COMPUTE_BIT = 1u << 1,
    RENDER_GRAPH_QUEUE_ASYNC_COMPUTE_BIT = 1u << 2,
};
using RenderGraphQueueFlags = uint32_t;

/** How a render target's extent is described. */
struct AttachmentInfo {
    SizeClass size_class = SizeClass::SwapchainRelative;
    float size_x = 1.0f;
    float size_y = 1.0f;
    /** Array depth. Zero means one layer, which is what a 2D target wants. */
    float size_z = 0.0f;
    /** `Undefined` means "inherit the swapchain's format". */
    Format format = Format::Undefined;
    /** Required for `InputRelative`; names the resource the fraction applies to. */
    std::string size_relative_name;

    uint32_t samples = 1;
    /** 0 means "the full mip chain"; anything else caps it. */
    uint32_t levels = 1;
    uint32_t layers = 1;

    AttachmentInfoFlags flags = ATTACHMENT_INFO_PERSISTENT_BIT;
};

/** How a buffer's size is described. */
struct BufferInfo {
    uint64_t size = 0;
    BufferUsageBits usage = BUFFER_USAGE_UNIFORM_BIT;
    AttachmentInfoFlags flags = ATTACHMENT_INFO_PERSISTENT_BIT;

    bool operator==(const BufferInfo &other) const noexcept {
        return size == other.size && usage == other.usage && flags == other.flags;
    }
    bool operator!=(const BufferInfo &other) const noexcept { return !(*this == other); }
};

/** A resource's fully resolved physical description, after sizing. */
struct ResourceDimensions {
    Format format = Format::Undefined;
    BufferInfo buffer_info;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t layers = 1;
    uint32_t levels = 1;
    uint32_t samples = 1;
    AttachmentInfoFlags flags = ATTACHMENT_INFO_PERSISTENT_BIT;
    RenderGraphQueueFlags queues = 0;
    uint32_t image_usage = 0;
    /** Set when a pre-rotated resource had its width and height swapped. */
    bool preRotated = false;
    std::string name;

    /** True when the resource is backed by memory rather than an image. */
    bool isBufferLike() const noexcept;

    /** True when the graph derived that the resource is transient. */
    bool isTransient() const noexcept {
        return (flags & ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT) != 0;
    }

    /**
     * Bytes a single mip level 0 of a colour or depth image occupies.
     *
     * The format table is deliberately incomplete: it covers the formats in
     * `Format` and returns 0 for anything else, so an unmapped format shows up
     * as a zero-size target rather than as a plausible wrong number.
     */
    uint64_t level0ByteSize() const noexcept;
};

/** Sentinel for "this resource has not been assigned a physical slot". */
inline constexpr uint32_t kUnusedResourceIndex = ~0u;

/**
 * Base of the three resource kinds.
 *
 * The whole graph hangs off `read_in_pass` and `written_in_pass`. A pass does
 * not know what it depends on; it records which resources it touched, and the
 * graph walks those sets to find the dependencies. This is what makes a frame
 * declaration readable — `add_color_output("hdr", ...)` says what the pass
 * touches, and the ordering, the barriers and the load/store operations all
 * follow without being written down.
 */
class RenderResource {
public:
    enum class Type : uint8_t { Buffer, Texture };

    RenderResource(Type type, uint32_t index) : resourceType_(type), index_(index) {}
    virtual ~RenderResource() = default;

    Type getType() const noexcept { return resourceType_; }
    uint32_t getIndex() const noexcept { return index_; }

    const std::string &getName() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    void writtenInPass(uint32_t pass) { writtenInPasses_.insert(pass); }
    void readInPass(uint32_t pass) { readInPasses_.insert(pass); }

    const std::set<uint32_t> &getReadPasses() const noexcept { return readInPasses_; }
    const std::set<uint32_t> &getWritePasses() const noexcept { return writtenInPasses_; }

    /**
     * The physical slot two resources may share.
     *
     * A subpass-merged output and its input are the same memory, so they are
     * assigned the same index; that is what lets the graph alias them.
     */
    uint32_t getPhysicalIndex() const noexcept { return physicalIndex_; }
    void setPhysicalIndex(uint32_t index) noexcept { physicalIndex_ = index; }

    void addQueue(RenderGraphQueueFlagBits queue) noexcept { usedQueues_ |= queue; }
    RenderGraphQueueFlags getUsedQueues() const noexcept { return usedQueues_; }

private:
    Type resourceType_;
    uint32_t index_;
    uint32_t physicalIndex_ = kUnusedResourceIndex;
    std::set<uint32_t> writtenInPasses_;
    std::set<uint32_t> readInPasses_;
    RenderGraphQueueFlags usedQueues_ = 0;
    std::string name_;
};

/** A buffer resource: uniform, storage, vertex, index or indirect. */
class RenderBufferResource : public RenderResource {
public:
    explicit RenderBufferResource(uint32_t index) : RenderResource(Type::Buffer, index) {}

    void setBufferInfo(const BufferInfo &info) { info_ = info; }
    const BufferInfo &getBufferInfo() const noexcept { return info_; }

    void addBufferUsage(BufferUsageBits usage) noexcept {
        info_.usage = static_cast<BufferUsageBits>(info_.usage | usage);
    }
    BufferUsageBits getBufferUsage() const noexcept { return info_.usage; }

private:
    BufferInfo info_;
};

/** An image resource: colour target, depth target, storage image or sampled texture. */
class RenderTextureResource : public RenderResource {
public:
    explicit RenderTextureResource(uint32_t index) : RenderResource(Type::Texture, index) {}

    void setAttachmentInfo(const AttachmentInfo &info) { info_ = info; }
    const AttachmentInfo &getAttachmentInfo() const noexcept { return info_; }
    AttachmentInfo &getAttachmentInfo() noexcept { return info_; }

    void addImageUsage(TextureUsageBits usage) noexcept { imageUsage_ |= static_cast<uint32_t>(usage); }
    uint32_t getImageUsage() const noexcept { return imageUsage_; }

private:
    AttachmentInfo info_;
    uint32_t imageUsage_ = 0;
};

/**
 * One node in the frame: a pass, and the resources it touches.
 *
 * A pass is built by declaring its outputs and its inputs. Nothing here names a
 * barrier, a layout or a load operation, because deriving those is the graph's
 * job and a hand-written one is exactly the thing being replaced.
 */
class RenderPass {
public:
    RenderPass(uint32_t index, RenderGraphQueueFlagBits queue) : index_(index), queue_(queue) {}

    static constexpr uint32_t kUnusedPassIndex = ~0u;

    uint32_t getIndex() const noexcept { return index_; }
    const std::string &getName() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    /**
     * The graph that owns this pass's resources.
     *
     * A pass names resources by string and the graph interns them, so a pass
     * cannot create a resource on its own. Set once by `RenderGraph::addPass`
     * and null only between construction and insertion, which is why the
     * declaration methods below are not called on a default-constructed pass.
     */
    void setGraph(RenderGraph *graph) noexcept { graph_ = graph; }

    RenderGraphQueueFlagBits getQueue() const noexcept { return queue_; }

    /**
     * A pass the graph may merge with this one.
     *
     * Two passes merge when one can read what the other wrote. The graph
     * tracks these separately from real dependencies, because a *soft*
     * dependency that is ignored is merely a missed optimisation while ignoring
     * a real one is a frame-time correctness bug.
     */
    uint32_t getPhysicalPassIndex() const noexcept { return physicalPassIndex_; }
    void setPhysicalPassIndex(uint32_t index) noexcept { physicalPassIndex_ = index; }

    /** A colour target this pass writes. `input` names an attachment read in the same pass. */
    RenderTextureResource &addColorOutput(const std::string &name, const AttachmentInfo &info,
                                         const std::string &input = std::string());

    /** A resolve destination paired with a multisampled colour output. */
    RenderTextureResource &addResolveOutput(const std::string &name, const AttachmentInfo &info);

    /** A depth/stencil target this pass writes. */
    RenderTextureResource &setDepthStencilOutput(const std::string &name, const AttachmentInfo &info);

    /** An existing attachment read in this pass, without a load operation. */
    RenderTextureResource &addAttachmentInput(const std::string &name);

    /**
     * The same resource as last frame.
     *
     * This is the read side of a render target history — TAA, motion vectors,
     * temporal upsampling, and an adaptive controller that needs to see what it
     * decided last frame. It also permanently disqualifies a resource from
     * being transient, because a transient resource has no memory that survives
     * to be read next frame.
     */
    RenderTextureResource &addHistoryInput(const std::string &name);

    /** A texture read for sampling. */
    RenderTextureResource &addTextureInput(const std::string &name);

    /** A buffer read for sampling. */
    RenderBufferResource &addUniformInput(const std::string &name);
    RenderBufferResource &addStorageReadOnlyInput(const std::string &name);
    RenderBufferResource &addVertexBufferInput(const std::string &name);
    RenderBufferResource &addIndexBufferInput(const std::string &name);
    RenderBufferResource &addIndirectBufferInput(const std::string &name);

    /** A buffer this pass writes. */
    RenderBufferResource &addStorageOutput(const std::string &name, const BufferInfo &info);
    RenderBufferResource &addTransferOutput(const std::string &name, const BufferInfo &info);

    /**
     * Pairs of (output, input) declared through `addColorOutput`'s `input`.
     *
     * Kept as a pairing rather than two separate lists because the aliasing
     * decision needs the relationship: an output may only share a physical slot
     * with the specific input it was declared to read.
     */
    const std::vector<std::pair<RenderTextureResource *, RenderTextureResource *>> &getMergedColor() const noexcept {
        return mergedColor_;
    }

    const std::vector<RenderTextureResource *> &getColorOutputs() const noexcept { return colorOutputs_; }
    const std::vector<RenderTextureResource *> &getColorInputs() const noexcept { return colorInputs_; }
    const std::vector<RenderTextureResource *> &getAttachmentInputs() const noexcept { return attachmentInputs_; }
    const std::vector<RenderTextureResource *> &getHistoryInputs() const noexcept { return historyInputs_; }
    const std::vector<RenderTextureResource *> &getResolveOutputs() const noexcept { return resolveOutputs_; }
    const std::vector<RenderBufferResource *> &getStorageOutputs() const noexcept { return storageOutputs_; }
    const std::vector<RenderBufferResource *> &getStorageInputs() const noexcept { return storageInputs_; }

    RenderTextureResource *getDepthStencilOutput() const noexcept { return depthStencilOutput_; }
    RenderTextureResource *getDepthStencilInput() const noexcept { return depthStencilInput_; }

private:
    uint32_t index_;
    RenderGraphQueueFlagBits queue_;
    std::string name_;
    uint32_t physicalPassIndex_ = kUnusedPassIndex;

    std::vector<RenderTextureResource *> colorOutputs_;
    std::vector<std::pair<RenderTextureResource *, RenderTextureResource *>> mergedColor_;
    std::vector<RenderTextureResource *> colorInputs_;
    std::vector<RenderTextureResource *> attachmentInputs_;
    std::vector<RenderTextureResource *> historyInputs_;
    std::vector<RenderTextureResource *> resolveOutputs_;
    std::vector<RenderBufferResource *> storageOutputs_;
    std::vector<RenderBufferResource *> storageInputs_;
    RenderTextureResource *depthStencilOutput_ = nullptr;
    RenderTextureResource *depthStencilInput_ = nullptr;
    RenderGraph *graph_ = nullptr;
};

/**
 * One derived ordering edge between two passes.
 *
 * Exposed rather than applied silently, because "the graph ordered these two
 * passes" is only useful if a test can ask *why*. The three kinds are the three
 * ways a read can depend on a write.
 */
struct PassDependency {
    uint32_t dependent = 0;  // the pass that reads
    uint32_t dependency = 0; // the pass that writes
    enum class Kind : uint8_t {
        /** Read-after-write: the ordinary case. */
        ReadAfterWrite,
        /** Write-after-read: a read-modify-write, such as an accumulation buffer. */
        WriteAfterRead,
        /** Write-after-write: two passes writing the same target. */
        WriteAfterWrite,
    } kind = Kind::ReadAfterWrite;
};

/**
 * The frame.
 *
 * Declare, then `bake()`. Baking resolves extents, derives the ordering, and
 * decides which resources can be transient. It is idempotent in the sense that
 * re-baking after only changing the swapchain dimensions produces a correctly
 * resized frame without redeclaring anything — which is the property dynamic
 * resolution is built on.
 */
class RenderGraph {
public:
    RenderGraph() = default;

    /** `RenderPass` interns resources through the graph that owns it. */
    friend class RenderPass;

    // -- declaration ---------------------------------------------------------

    /**
     * Add a pass. Names must be unique; a duplicate throws.
     *
     * Throwing rather than silently replacing matters because a shadowed pass
     * name is otherwise invisible: the frame appears to declare a pass that is
     * never scheduled, and the symptom is missing output rather than an error.
     */
    RenderPass &addPass(const std::string &name, RenderGraphQueueFlagBits queue);

    RenderPass *findPass(const std::string &name);
    const RenderPass *findPass(const std::string &name) const;

    /**
     * The name of the pass at an index, or "?" for an index not in the graph.
     *
     * The derived order is a list of indices and the declaration is a list of
     * names, so anything reporting the frame -- a test, a HUD, a log line --
     * has to cross that gap. Returning "?" rather than a reference to a default
     * object keeps an out-of-range index a visible wrong answer.
     */
    const std::string &passName(uint32_t index) const noexcept;

    /**
     * Set the extent every `SwapchainRelative` resource is measured against.
     *
     * This is the render-scale control point. Changing it and re-baking resizes
     * the entire frame, which is what makes dynamic resolution a data change
     * rather than an engine rewrite.
     */
    void setBackbufferDimensions(const ResourceDimensions &dim) { backbufferDimensions_ = dim; }
    const ResourceDimensions &getBackbufferDimensions() const noexcept { return backbufferDimensions_; }

    /** Name the resource that is the presented backbuffer. */
    void setBackbufferSource(const std::string &name) { backbufferSource_ = name; }

    /**
     * Set a known capability of the target.
     *
     * A tile-based GPU can alias transient memory that a discrete GPU cannot,
     * so the same frame is transient on one and persistent on the other. Left
     * false — the conservative answer, matching a discrete GPU — because
     * getting it wrong corrupts a frame rather than merely costing memory.
     */
    void setUseTransientColor(bool enable) noexcept { useTransientColor_ = enable; }
    void setUseTransientDepthStencil(bool enable) noexcept { useTransientDepthStencil_ = enable; }
    bool getUseTransientColor() const noexcept { return useTransientColor_; }
    bool getUseTransientDepthStencil() const noexcept { return useTransientDepthStencil_; }

    // -- resolution ----------------------------------------------------------

    /**
     * Resolve a texture's physical description.
     *
     * `InputRelative` is resolved recursively, so the chain works in any
     * declaration order. Throws if the named input does not exist or if the
     * chain is cyclic.
     */
    ResourceDimensions getResourceDimensions(const RenderTextureResource &resource) const;
    ResourceDimensions getResourceDimensions(const RenderBufferResource &resource) const;

    // -- baking --------------------------------------------------------------

    /**
     * Derive everything: extents, ordering, physical slots, transient status.
     *
     * Throws on a resource that is read but never written, on a dependency
     * cycle, and on an `InputRelative` resource naming something that is not a
     * texture. Those are the three ways a declaration can be wrong in a way
     * that would otherwise produce a plausible but incorrect frame.
     */
    void bake();

    /** Clear all passes, resources and cached state. The graph is reusable. */
    void reset();

    // -- results -------------------------------------------------------------

    /** Passes in the order the graph derived they must run. */
    const std::vector<uint32_t> &getFlattenedPasses() const noexcept { return flattenedPasses_; }

    /** Every derived ordering edge, in derivation order. */
    const std::vector<PassDependency> &getDependencies() const noexcept { return dependencies_; }

    /** The physical description of the slot a resource was assigned. */
    const ResourceDimensions &getPhysicalDimensions(uint32_t physicalIndex) const;

    /** Number of distinct physical slots. The lower the number, the more aliasing. */
    size_t getPhysicalResourceCount() const noexcept { return physicalDimensions_.size(); }

    /** True once `bake()` has run and the results are current. */
    bool isBaked() const noexcept { return baked_; }

    /**
     * A one-line-per-pass summary, for a test failure message or a debug HUD.
     *
     * Takes the work out of "assert the order is right" by hand: a wrong order
     * prints the order the graph actually chose and the edges it believed in.
     */
    std::string describe() const;

public:
    /**
     * Intern a texture by name, creating it on first use.
     *
     * Public so `RenderPass` can reach it; not part of the frame-declaration
     * API. Declaring the same name as both a texture and a buffer throws,
     * because otherwise one silently displaces the other.
     */
    RenderTextureResource &getOrCreateTexture(const std::string &name);
    RenderBufferResource &getOrCreateBuffer(const std::string &name);

private:
    void buildPhysicalSlots();
    void deriveDependencies();
    void buildPassOrder();
    void buildTransients();
    void assignPhysicalPasses();

    std::vector<std::unique_ptr<RenderPass>> passes_;
    std::vector<std::unique_ptr<RenderResource>> resources_;
    std::map<std::string, uint32_t> passByName_;
    std::map<std::string, uint32_t> resourceByName_;

    ResourceDimensions backbufferDimensions_;
    std::string backbufferSource_;

    std::vector<ResourceDimensions> physicalDimensions_;
    std::vector<PassDependency> dependencies_;
    std::vector<std::vector<uint32_t>> passDependencies_;
    std::vector<uint32_t> flattenedPasses_;
    std::vector<bool> physicalImageHasHistory_;
    std::vector<bool> physicalIsBackbuffer_;

    bool useTransientColor_ = false;
    bool useTransientDepthStencil_ = false;
    bool baked_ = false;
};

} // namespace emergent
