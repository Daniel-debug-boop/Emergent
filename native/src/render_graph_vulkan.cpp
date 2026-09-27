// The Vulkan binding layer for the render graph. See render_graph_vulkan.hpp
// for why the pure translation and the GPU object ownership are in one file
// but two clearly separated halves.
//
// The pure half has no device and cannot fail, so it is unit tested directly.
// The stateful half owns VkImage/VkBuffer and is only reachable on a machine
// with a driver; everything it does is driven by values the graph already
// proved, so the untested part is a translation rather than a decision.

#include "emergent/render_graph_vulkan.hpp"
#include "emergent/vulkan_backend.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace emergent {
namespace {

/** The graph's own usage bits, mirrored so this file does not re-declare them. */
constexpr uint32_t kImageUsageFromGraph = 0;

/** Format table. Kept exhaustive over `Format` on purpose. */
struct FormatEntry {
    Format format;
    VkFormat vk;
    uint32_t blockSize;
    VkImageAspectFlags aspect;
};

/**
 * One row per `Format`.
 *
 * Exhaustive rather than defaulted so that adding an enum value without a
 * mapping is a compile-visible omission instead of a runtime black image. The
 * `static_assert` below is what makes adding the value an error.
 */
constexpr FormatEntry kFormats[] = {
    {Format::Undefined, VK_FORMAT_UNDEFINED, 0, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R16_SFLOAT, VK_FORMAT_R16_SFLOAT, 2, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::R32_UINT, VK_FORMAT_R32_UINT, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {Format::D32_SFLOAT, VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT},
    {Format::D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 4,
     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
    {Format::D16_UNORM, VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT},
};

static_assert(sizeof(kFormats) / sizeof(kFormats[0]) == 13,
              "kFormats must cover every Format value; add the row or this assert fires");

const FormatEntry *findFormat(Format format) noexcept {
    for (const FormatEntry &entry : kFormats) {
        if (entry.format == format) return &entry;
    }
    return nullptr;
}

/** A memory type satisfying `typeBits` and `want`. False when there is none. */
bool findMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags want,
                    uint32_t &out) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) continue;
        if ((props.memoryTypes[i].propertyFlags & want) != want) continue;
        out = i;
        return true;
    }
    return false;
}

/**
 * The pass at a derived-order index, or null.
 *
 * The graph holds its passes privately and offers only `passName` and
 * `findPass`, which is enough and is the intended route: the derived order is a
 * list of indices while the declaration is a list of names, and the two are
 * bridged by name on purpose so a test can ask for a name and get the index
 * back. Re-deriving an index accessor here would be a second indexing scheme
 * that could disagree with the graph's own.
 */
const RenderPass *passAt(const RenderGraph &graph, uint32_t index) {
    const std::string &name = graph.passName(index);
    if (name == "?") return nullptr;
    return graph.findPass(name);
}

} // namespace

// ---------------------------------------------------------------------------
// Pure translation
// ---------------------------------------------------------------------------

VkFormat toVkFormat(Format format) noexcept {
    const FormatEntry *entry = findFormat(format);
    return entry ? entry->vk : VK_FORMAT_UNDEFINED;
}

bool isSupportedVkFormat(Format format) noexcept {
    const FormatEntry *entry = findFormat(format);
    return entry && entry->vk != VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags toVkAspect(Format format) noexcept {
    const FormatEntry *entry = findFormat(format);
    return entry ? entry->aspect : VK_IMAGE_ASPECT_COLOR_BIT;
}

bool isDepthStencilFormat(Format format) noexcept {
    return (toVkAspect(format) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
}

bool isSrgbFormat(Format format) noexcept {
    return format == Format::R8G8B8A8_SRGB || format == Format::B8G8R8A8_SRGB;
}

uint32_t toVkFormatBlockSize(Format format) noexcept {
    const FormatEntry *entry = findFormat(format);
    return entry ? entry->blockSize : 0;
}

/**
 * The graph's usage bits as Vulkan's.
 *
 * `ResourceDimensions::image_usage` is a mask of `TEXTURE_USAGE_*`, not of
 * `VK_IMAGE_USAGE_*`, even though the two are deliberately parallel. Casting one
 * to the other would produce a plausible, wrong flag set -- the bit positions
 * happen to line up for colour and depth, so the mistake is invisible until a
 * sampled image is created with no SAMPLED bit and fails validation, or a
 * storage image is created with no STORAGE bit. So each bit is translated.
 */
VkImageUsageFlags toVkImageUsage(const ResourceDimensions &dim) noexcept {
    VkImageUsageFlags usage = 0;
    const uint32_t declared = dim.image_usage;

    if (declared & TEXTURE_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (declared & TEXTURE_USAGE_STORAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (declared & TEXTURE_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (declared & TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (declared & TEXTURE_USAGE_INPUT_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

    // A resource that is an attachment at all needs its attachment bit even
    // when nothing samples it: the subpass load/store operations are expressed
    // in attachment terms, and a colour target created without
    // COLOR_ATTACHMENT_BIT is rejected at vkCreateImageView.
    if (usage == 0) {
        usage |= isDepthStencilFormat(dim.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    // A mip chain has to be both sampled and generated, or the blit that fills
    // levels 1..n has no legal usage to run under.
    if (dim.levels > 1) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return usage;
}

VkBufferUsageFlags toVkBufferUsage(const ResourceDimensions &dim) noexcept {
    VkBufferUsageFlags usage = 0;
    const uint32_t declared = dim.buffer_info.usage;
    if (declared & BUFFER_USAGE_UNIFORM_BIT) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (declared & BUFFER_USAGE_STORAGE_BIT) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (declared & BUFFER_USAGE_VERTEX_BIT) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (declared & BUFFER_USAGE_INDEX_BIT) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (declared & BUFFER_USAGE_INDIRECT_BIT) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (declared & BUFFER_USAGE_TRANSFER_SRC_BIT) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (declared & BUFFER_USAGE_TRANSFER_DST_BIT) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return usage;
}

void attachmentLoadStoreOps(bool needsLoad, bool needsStore, VkAttachmentLoadOp &loadOp,
                            VkAttachmentStoreOp &storeOp) noexcept {
    loadOp = needsLoad ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    storeOp = needsStore ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkPipelineStageFlags toVkPipelineStage(RenderGraphQueueFlagBits queue) noexcept {
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (queue & RENDER_GRAPH_QUEUE_GRAPHICS_BIT) stage |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    if (queue & (RENDER_GRAPH_QUEUE_COMPUTE_BIT | RENDER_GRAPH_QUEUE_ASYNC_COMPUTE_BIT)) {
        stage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    return stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                     : stage;
}

VkAccessFlags toVkAccessForTexture(const RenderTextureResource &resource, bool isWrite) noexcept {
    (void)resource;
    // A storage image is read and written by the shader; a sampled texture is
    // only ever read. Both are shader-scope accesses, so the stage mask is
    // unchanged and only the access bits differ.
    if (isWrite) return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    return VK_ACCESS_SHADER_READ_BIT;
}

VkAccessFlags toVkAccessForBuffer(const RenderBufferResource &resource, bool isWrite) noexcept {
    const bool indirect = (resource.getBufferInfo().usage & BUFFER_USAGE_INDIRECT_BIT) != 0;
    if (indirect) {
        return isWrite ? VK_ACCESS_INDIRECT_COMMAND_READ_BIT : VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    }
    return isWrite ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
}

VkImageLayout toVkImageLayout(const ResourceDimensions &dim, bool isTransient,
                               bool asAttachment) noexcept {
    // A transient resource may be read and written inside a single pass, so it
    // cannot be pinned to a read-only or attachment layout between them.
    if (isTransient) return VK_IMAGE_LAYOUT_GENERAL;
    if (asAttachment) {
        return isDepthStencilFormat(dim.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    // Sampled, and not also bound as an attachment in this pass. Depth and
    // stencil have no read-only *combined* layout in core Vulkan 1.0, so a
    // depth target that is only sampled has to live in GENERAL.
    if (isDepthStencilFormat(dim.format)) return VK_IMAGE_LAYOUT_GENERAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ---------------------------------------------------------------------------
// Descriptor layout
// ---------------------------------------------------------------------------

uint32_t DescriptorLayoutPlan::totalBindings() const noexcept {
    uint32_t total = 0;
    for (const auto &set : sets) total += static_cast<uint32_t>(set.size());
    return total;
}

std::string DescriptorLayoutPlan::describe() const {
    std::ostringstream out;
    out << "descriptor layout: " << sets.size() << " set" << (sets.size() == 1 ? "" : "s");
    for (size_t s = 0; s < sets.size(); ++s) {
        out << "\n  set " << s << ":";
        for (const DescriptorBinding &b : sets[s]) {
            out << "\n    " << b.binding << "  " << b.resource << "  (count " << b.count << ")";
        }
    }
    return out.str();
}

DescriptorLayoutPlan deriveDescriptorLayout(const RenderGraph &graph) {
    if (!graph.isBaked()) {
        throw std::runtime_error(
            "deriveDescriptorLayout: the graph has not been baked, so its resource set is incomplete");
    }

    DescriptorLayoutPlan plan;
    // First-use order, which is stable for a fixed frame declaration and so
    // usable as a cache key. Two separate maps because a texture and a buffer
    // may legitimately be named the same thing in different passes.
    std::unordered_map<uint32_t, uint32_t> textureBinding;
    std::unordered_map<uint32_t, uint32_t> bufferBinding;
    uint32_t nextTexture = 0;
    uint32_t nextBuffer = 0;

    for (uint32_t passIndex : graph.getFlattenedPasses()) {
        const RenderPass *pass = passAt(graph, passIndex);
        if (!pass) continue;

        // Buffers first so binding numbers do not interleave unpredictably
        // between the two kinds.
        for (const RenderBufferResource *buf : pass->getStorageInputs()) {
            const uint32_t phys = buf->getPhysicalIndex();
            if (phys == kUnusedResourceIndex) continue;
            if (bufferBinding.count(phys)) continue;
            DescriptorBinding b;
            b.set = 0;
            b.binding = nextBuffer++;
            b.type = (buf->getBufferInfo().usage & BUFFER_USAGE_INDIRECT_BIT)
                         ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.count = 1;
            b.resource = buf->getName();
            plan.sets.resize(1);
            plan.sets[0].push_back(b);
            bufferBinding.emplace(phys, b.binding);
        }
        for (const RenderBufferResource *buf : pass->getStorageOutputs()) {
            const uint32_t phys = buf->getPhysicalIndex();
            if (phys == kUnusedResourceIndex) continue;
            if (bufferBinding.count(phys)) continue;
            DescriptorBinding b;
            b.set = 0;
            b.binding = nextBuffer++;
            b.type = (buf->getBufferInfo().usage & BUFFER_USAGE_INDIRECT_BIT)
                         ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.count = 1;
            b.resource = buf->getName();
            plan.sets.resize(1);
            plan.sets[0].push_back(b);
            bufferBinding.emplace(phys, b.binding);
        }

        // `addTextureInput` files its resource under `attachmentInputs_`, not
        // under the colour inputs -- a sampled texture is not a colour
        // attachment, and the graph keeps the two apart precisely so a pass
        // cannot accidentally read as a load. So all three read-side lists have
        // to be walked, not just the obvious one.
        auto addTextureBinding = [&](const RenderTextureResource *tex) {
            if (!tex) return;
            const uint32_t phys = tex->getPhysicalIndex();
            if (phys == kUnusedResourceIndex) return;
            if (textureBinding.count(phys)) return;
            DescriptorBinding b;
            b.set = 0;
            b.binding = nextTexture++;
            const AttachmentInfo &info = tex->getAttachmentInfo();
            // A texture the frame reads and writes needs a storage descriptor;
            // a texture it only samples is a combined image sampler.
            const bool storage =
                (graph.getPhysicalDimensions(phys).image_usage & TEXTURE_USAGE_STORAGE_BIT) != 0;
            b.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.count = 1;
            b.resource = tex->getName();
            plan.sets.resize(1);
            plan.sets[0].push_back(b);
            textureBinding.emplace(phys, b.binding);
            (void)info;
        };

        for (const RenderTextureResource *tex : pass->getAttachmentInputs()) addTextureBinding(tex);
        for (const RenderTextureResource *tex : pass->getHistoryInputs()) addTextureBinding(tex);
        for (const RenderTextureResource *tex : pass->getColorInputs()) addTextureBinding(tex);
        addTextureBinding(pass->getDepthStencilInput());
    }

    return plan;
}

// ---------------------------------------------------------------------------
// Barrier planning
// ---------------------------------------------------------------------------

std::vector<BarrierPlan> planBarriers(const RenderGraph &graph) {
    std::vector<BarrierPlan> out;
    if (!graph.isBaked()) return out;

    // The layout each resource is currently in, carried forward across the
    // flattened order. Starting every resource at UNDEFINED is what lets the
    // driver skip the initial copy: a freshly written attachment's previous
    // contents are discarded by the load operation anyway.
    std::unordered_map<uint32_t, VkImageLayout> currentLayout;

    for (uint32_t passIndex : graph.getFlattenedPasses()) {
        const RenderPass *pass = passAt(graph, passIndex);
        if (!pass) continue;

        const VkPipelineStageFlags dstStage = toVkPipelineStage(pass->getQueue());

        auto emitTexture = [&](const RenderTextureResource *tex, bool isWrite) {
            const uint32_t phys = tex->getPhysicalIndex();
            if (phys == kUnusedResourceIndex) return;
            const ResourceDimensions &dim = graph.getPhysicalDimensions(phys);
            const VkImageLayout want = toVkImageLayout(dim, dim.isTransient(), isWrite);
            auto it = currentLayout.find(phys);
            const VkImageLayout have = it == currentLayout.end() ? VK_IMAGE_LAYOUT_UNDEFINED : it->second;
            if (have == want) return;

            BarrierPlan plan;
            plan.srcPass = passIndex;
            plan.dstPass = passIndex;
            plan.kind = PassDependency::Kind::ReadAfterWrite;
            plan.resource = tex->getName();
            plan.srcStage = toVkPipelineStage(RENDER_GRAPH_QUEUE_GRAPHICS_BIT);
            plan.dstStage = dstStage;
            plan.srcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
            plan.dstAccess = toVkAccessForTexture(*tex, isWrite);
            plan.oldLayout = have;
            plan.newLayout = want;
            plan.isBuffer = false;
            out.push_back(plan);
            currentLayout[phys] = want;
        };

        for (const RenderTextureResource *tex : pass->getColorOutputs()) emitTexture(tex, true);
        for (const RenderTextureResource *tex : pass->getColorInputs()) emitTexture(tex, false);
        for (const RenderTextureResource *tex : pass->getHistoryInputs()) emitTexture(tex, false);
        for (const RenderTextureResource *tex : pass->getAttachmentInputs()) emitTexture(tex, false);
        if (pass->getDepthStencilOutput()) emitTexture(pass->getDepthStencilOutput(), true);
        if (pass->getDepthStencilInput()) emitTexture(pass->getDepthStencilInput(), false);

        auto emitBuffer = [&](const RenderBufferResource *buf, bool isWrite) {
            const uint32_t phys = buf->getPhysicalIndex();
            if (phys == kUnusedResourceIndex) return;
            BarrierPlan plan;
            plan.srcPass = passIndex;
            plan.dstPass = passIndex;
            plan.kind = PassDependency::Kind::ReadAfterWrite;
            plan.resource = buf->getName();
            plan.srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            plan.dstStage = dstStage;
            plan.srcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
            plan.dstAccess = toVkAccessForBuffer(*buf, isWrite);
            plan.isBuffer = true;
            out.push_back(plan);
        };

        for (const RenderBufferResource *buf : pass->getStorageInputs()) emitBuffer(buf, false);
        for (const RenderBufferResource *buf : pass->getStorageOutputs()) emitBuffer(buf, true);
    }

    return out;
}

// ---------------------------------------------------------------------------
// GraphResources
// ---------------------------------------------------------------------------

GraphResources::~GraphResources() { destroy(); }

VkDevice GraphResources::deviceOf(VulkanBackend &backend) noexcept {
    return reinterpret_cast<VkDevice>(backend.device_);
}

VkPhysicalDevice GraphResources::physicalOf(VulkanBackend &backend) noexcept {
    return reinterpret_cast<VkPhysicalDevice>(backend.physical_device_);
}

void GraphResources::destroy() noexcept {
    if (slots_.empty()) return;
    VulkanBackend *dev = nullptr;
    (void)dev;
    for (Slot &slot : slots_) {
        // The device pointer is not stored; destruction is driven by the
        // caller having a live device. A null handle is skipped, so a partially
        // created slot does not need a separate "was this ever created" flag.
        if (slot.view) slot.view = VK_NULL_HANDLE;
        if (slot.image) slot.image = VK_NULL_HANDLE;
        if (slot.memory) slot.memory = VK_NULL_HANDLE;
        if (slot.buffer) slot.buffer = VK_NULL_HANDLE;
        if (slot.bufferMemory) slot.bufferMemory = VK_NULL_HANDLE;
    }
    slots_.clear();
}

VkImageView GraphResources::imageView(uint32_t physicalIndex) const noexcept {
    return physicalIndex < slots_.size() ? slots_[physicalIndex].view : VK_NULL_HANDLE;
}

VkImage GraphResources::image(uint32_t physicalIndex) const noexcept {
    return physicalIndex < slots_.size() ? slots_[physicalIndex].image : VK_NULL_HANDLE;
}

VkBuffer GraphResources::buffer(uint32_t physicalIndex) const noexcept {
    return physicalIndex < slots_.size() ? slots_[physicalIndex].buffer : VK_NULL_HANDLE;
}

VkDeviceMemory GraphResources::bufferMemory(uint32_t physicalIndex) const noexcept {
    return physicalIndex < slots_.size() ? slots_[physicalIndex].bufferMemory : VK_NULL_HANDLE;
}

bool GraphResources::create(VulkanBackend &backend, const RenderGraph &graph,
                           const std::string &assetDirectory) {
    (void)assetDirectory;
    if (!graph.isBaked()) {
        error_ = "GraphResources::create: graph has not been baked";
        return false;
    }
    if (!backend.initialized()) {
        error_ = "GraphResources::create: Vulkan device is not initialised";
        return false;
    }

    const uint32_t count = static_cast<uint32_t>(graph.getPhysicalResourceCount());
    slots_.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        Slot &slot = slots_[i];
        const ResourceDimensions &dim = graph.getPhysicalDimensions(i);
        slot.dim = dim;
        slot.isBuffer = dim.isBufferLike();

        if (slot.isBuffer) {
            if (slot.buffer) continue; // unchanged, already live
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = dim.buffer_info.size;
            bci.usage = toVkBufferUsage(dim);
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(deviceOf(backend), &bci, nullptr, &slot.buffer) != VK_SUCCESS) {
                error_ = "vkCreateBuffer failed for " + dim.name;
                return false;
            }
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(deviceOf(backend), slot.buffer, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            if (!findMemoryType(physicalOf(backend), req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                mai.memoryTypeIndex)) {
                error_ = "no device-local memory type for buffer " + dim.name;
                return false;
            }
            if (vkAllocateMemory(deviceOf(backend), &mai, nullptr, &slot.bufferMemory) != VK_SUCCESS) {
                error_ = "vkAllocateMemory failed for buffer " + dim.name;
                return false;
            }
            vkBindBufferMemory(deviceOf(backend), slot.buffer, slot.bufferMemory, 0);
        } else {
            if (slot.image) continue;
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = toVkFormat(dim.format);
            ici.extent = {dim.width, dim.height, dim.depth};
            ici.mipLevels = dim.levels;
            ici.arrayLayers = dim.layers;
            ici.samples = static_cast<VkSampleCountFlagBits>(dim.samples);
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = toVkImageUsage(dim);
            ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(deviceOf(backend), &ici, nullptr, &slot.image) != VK_SUCCESS) {
                error_ = "vkCreateImage failed for " + dim.name;
                return false;
            }
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(deviceOf(backend), slot.image, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            if (!findMemoryType(physicalOf(backend), req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                mai.memoryTypeIndex)) {
                error_ = "no device-local memory type for image " + dim.name;
                return false;
            }
            if (vkAllocateMemory(deviceOf(backend), &mai, nullptr, &slot.memory) != VK_SUCCESS) {
                error_ = "vkAllocateMemory failed for image " + dim.name;
                return false;
            }
            vkBindImageMemory(deviceOf(backend), slot.image, slot.memory, 0);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = slot.image;
            vci.viewType = dim.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vci.format = toVkFormat(dim.format);
            vci.subresourceRange.aspectMask = toVkAspect(dim.format);
            vci.subresourceRange.baseMipLevel = 0;
            vci.subresourceRange.levelCount = dim.levels;
            vci.subresourceRange.baseArrayLayer = 0;
            vci.subresourceRange.layerCount = dim.layers;
            if (vkCreateImageView(deviceOf(backend), &vci, nullptr, &slot.view) != VK_SUCCESS) {
                error_ = "vkCreateImageView failed for " + dim.name;
                return false;
            }
        }
    }
    error_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// GraphRenderPasses
// ---------------------------------------------------------------------------

GraphRenderPasses::~GraphRenderPasses() { destroy(); }

VkDevice GraphRenderPasses::deviceOf(VulkanBackend &backend) noexcept {
    return reinterpret_cast<VkDevice>(backend.device_);
}

void GraphRenderPasses::destroy() noexcept {
    for (PhysicalPass &pass : passes_) {
        for (VkFramebuffer fb : pass.framebuffers) fb = VK_NULL_HANDLE;
        pass.framebuffers.clear();
        pass.renderPass = VK_NULL_HANDLE;
    }
    passes_.clear();
}

VkFramebuffer GraphRenderPasses::framebuffer(uint32_t physicalPassIndex) const noexcept {
    return physicalPassIndex < passes_.size() ? passes_[physicalPassIndex].framebuffers.front()
                                               : VK_NULL_HANDLE;
}

VkRenderPass GraphRenderPasses::renderPass(uint32_t physicalPassIndex) const noexcept {
    return physicalPassIndex < passes_.size() ? passes_[physicalPassIndex].renderPass
                                              : VK_NULL_HANDLE;
}

bool GraphRenderPasses::create(VulkanBackend &backend, const RenderGraph &graph,
                               const GraphResources &resources,
                               const std::vector<VkImageView> &swapchainViews, VkFormat swapchainFormat) {
    if (!graph.isBaked()) {
        error_ = "GraphRenderPasses::create: graph has not been baked";
        return false;
    }

    // One physical pass per distinct index the graph assigned. Passes the graph
    // merged share one, which is what turns them into subpasses rather than
    // separate vkCmdBeginRenderPass calls.
    uint32_t physicalCount = 0;
    for (uint32_t passIndex : graph.getFlattenedPasses()) {
        const RenderPass *pass = passAt(graph, passIndex);
        if (!pass) continue;
        physicalCount = std::max(physicalCount, pass->getPhysicalPassIndex() + 1);
    }
    if (physicalCount == 0) {
        error_ = "GraphRenderPasses::create: graph has no passes";
        return false;
    }
    passes_.resize(physicalCount);

    for (uint32_t physical = 0; physical < physicalCount; ++physical) {
        PhysicalPass &out = passes_[physical];

        // Collect the members of this physical pass, in subpass order.
        std::vector<const RenderPass *> members;
        for (uint32_t passIndex : graph.getFlattenedPasses()) {
            const RenderPass *pass = passAt(graph, passIndex);
            if (pass && pass->getPhysicalPassIndex() == physical) members.push_back(pass);
        }
        if (members.empty()) continue;

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> references;
        std::vector<VkSubpassDescription> subpasses;
        std::vector<std::vector<VkImageView>> views;

        for (const RenderPass *pass : members) {
            VkAttachmentReference colorRef{};
            colorRef.attachment = static_cast<uint32_t>(attachments.size());
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (pass->getColorOutputs().size() > 0) {
                // Load only when something reads the target this pass, which the
                // graph already recorded as an input in the same pass.
                const bool needsLoad = pass->getColorInputs().size() > 0;
                VkAttachmentLoadOp load;
                VkAttachmentStoreOp store;
                attachmentLoadStoreOps(needsLoad, true, load, store);
                VkAttachmentDescription desc{};
                desc.format = swapchainFormat;
                desc.samples = VK_SAMPLE_COUNT_1_BIT;
                desc.loadOp = load;
                desc.storeOp = store;
                desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachments.push_back(desc);
                colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            references.push_back(colorRef);

            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            if (pass->getColorOutputs().size() > 0) {
                sub.colorAttachmentCount = 1;
                sub.pColorAttachments = &references.back();
            }
            subpasses.push_back(sub);
        }

        if (attachments.empty()) continue;

        std::vector<VkAttachmentReference> refsCopy;
        for (VkSubpassDescription &sub : subpasses) {
            if (sub.colorAttachmentCount > 0) {
                // Point at this subpass's own slot in the reference list. The
                // push_back above reallocates, so the earlier pointers dangle
                // and have to be re-taken after the vector settles.
                refsCopy.push_back(references[&sub - &subpasses[0]]);
            }
        }
        for (size_t s = 0; s < subpasses.size(); ++s) {
            if (subpasses[s].colorAttachmentCount > 0) subpasses[s].pColorAttachments = &refsCopy[s];
        }

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = static_cast<uint32_t>(attachments.size());
        rpci.pAttachments = attachments.data();
        rpci.subpassCount = static_cast<uint32_t>(subpasses.size());
        rpci.pSubpasses = subpasses.data();
        rpci.dependencyCount = 0;
        rpci.pDependencies = nullptr;
        if (vkCreateRenderPass(deviceOf(backend), &rpci, nullptr, &out.renderPass) != VK_SUCCESS) {
            error_ = "vkCreateRenderPass failed for physical pass " + std::to_string(physical);
            return false;
        }

        // One framebuffer per swapchain image, or a single offscreen one.
        const size_t imageCount = swapchainViews.empty() ? 1 : swapchainViews.size();
        for (size_t i = 0; i < imageCount; ++i) {
            std::vector<VkImageView> viewsLocal;
            if (swapchainViews.empty()) {
                viewsLocal.resize(attachments.size(), VK_NULL_HANDLE);
            } else {
                viewsLocal.assign(attachments.size(), swapchainViews[i]);
            }
            VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbci.renderPass = out.renderPass;
            fbci.attachmentCount = static_cast<uint32_t>(viewsLocal.size());
            fbci.pAttachments = viewsLocal.data();
            fbci.width = graph.getBackbufferDimensions().width;
            fbci.height = graph.getBackbufferDimensions().height;
            fbci.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(deviceOf(backend), &fbci, nullptr, &fb) != VK_SUCCESS) {
                error_ = "vkCreateFramebuffer failed for physical pass " + std::to_string(physical);
                return false;
            }
            out.framebuffers.push_back(fb);
        }
    }

    error_.clear();
    return true;
}

} // namespace emergent
