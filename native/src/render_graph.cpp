// Render graph: resource bookkeeping, hazard derivation, sizing, ordering.
//
// The algorithms here are ported from Hans-Kristian Arntzen's Granite
// (`renderer/render_graph.cpp`, MIT), reduced to what can be expressed without
// a Vulkan device. Nothing in this file creates an image, a view or a command
// buffer; it decides *what* would be created and *in what order*, which is the
// part that has to be right and the part that can be checked without hardware.

#include "emergent/render_graph.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace emergent {

namespace {

/**
 * Bytes per pixel for each format.
 *
 * Returning 0 for a format not in the table is deliberate. A guessed size is a
 * plausible-looking number that silently over- or under-allocates; a zero makes
 * the omission visible in a size assertion instead.
 */
uint32_t formatBytesPerPixel(Format format) noexcept {
    switch (format) {
    case Format::R8G8B8A8_UNORM:
    case Format::R8G8B8A8_SRGB:
    case Format::B8G8R8A8_UNORM:
    case Format::B8G8R8A8_SRGB:
    case Format::R32_SFLOAT:
    case Format::R32_UINT:
    case Format::D32_SFLOAT:
    case Format::D24_UNORM_S8_UINT:
        return 4;
    case Format::R16G16B16A16_SFLOAT:
        return 8;
    case Format::R32G32B32A32_SFLOAT:
        return 16;
    case Format::R16_SFLOAT:
    case Format::D16_UNORM:
        return 2;
    case Format::Undefined:
        return 0;
    }
    return 0;
}

/**
 * Number of mip levels in a full chain.
 *
 * A 1x1 texture has one level, and each halving adds one, so this is the bit
 * length of the largest extent: 1024 gives 11 levels (1024 down to 1).
 */
uint32_t fullMipLevelCount(uint32_t width, uint32_t height, uint32_t depth) noexcept {
    uint32_t maxDim = std::max(std::max(width, height), depth);
    uint32_t levels = 0;
    while (maxDim != 0) {
        levels++;
        maxDim >>= 1;
    }
    return levels;
}

/** Resolve a fractional size, rounding up and never below one. */
uint32_t scaledExtent(float fraction, uint32_t base) noexcept {
    const double scaled = std::ceil(static_cast<double>(fraction) * static_cast<double>(base));
    if (!(scaled > 0.0)) {
        return 1;
    }
    if (scaled >= 4294967295.0) {
        return 0xFFFFFFFFu;
    }
    return std::max(static_cast<uint32_t>(scaled), 1u);
}

} // namespace

bool formatHasDepthOrStencil(Format format) noexcept {
    switch (format) {
    case Format::D32_SFLOAT:
    case Format::D24_UNORM_S8_UINT:
    case Format::D16_UNORM:
        return true;
    default:
        return false;
    }
}

bool ResourceDimensions::isBufferLike() const noexcept {
    return buffer_info.size != 0 || (flags & ATTACHMENT_INFO_INTERNAL_PROXY_BIT) != 0;
}

uint64_t ResourceDimensions::level0ByteSize() const noexcept {
    const uint64_t bpp = formatBytesPerPixel(format);
    if (bpp == 0) {
        return 0;
    }
    return static_cast<uint64_t>(bpp) * width * height * depth * layers * samples;
}

// -- RenderPass -------------------------------------------------------------

namespace {
// RenderPass needs to reach RenderGraph's resource factory, and RenderGraph
// needs to reach RenderPass's pass index. The friendship is one-directional
// and documented at the declaration.
} // namespace

RenderTextureResource &RenderPass::addColorOutput(const std::string &name, const AttachmentInfo &info,
                                                  const std::string &input) {
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.setAttachmentInfo(info);
    res.addImageUsage(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
    res.addQueue(queue_);
    res.writtenInPass(index_);
    colorOutputs_.push_back(&res);

    // Naming an input is a subpass-merge declaration: the output reads what the
    // input holds, in this same pass. Recorded as a read so the dependency walk
    // orders this pass after whichever pass wrote it, and so the two can be
    // assigned one physical slot.
    if (!input.empty()) {
        RenderTextureResource &in = graph_->getOrCreateTexture(input);
        in.addImageUsage(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
        in.addQueue(queue_);
        in.readInPass(index_);
        colorInputs_.push_back(&in);
        mergedColor_.emplace_back(&res, &in);
    }
    return res;
}

RenderTextureResource &RenderPass::addResolveOutput(const std::string &name, const AttachmentInfo &info) {
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.setAttachmentInfo(info);
    res.addImageUsage(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
    res.addQueue(queue_);
    res.writtenInPass(index_);
    resolveOutputs_.push_back(&res);
    return res;
}

RenderTextureResource &RenderPass::setDepthStencilOutput(const std::string &name, const AttachmentInfo &info) {
    if (depthStencilOutput_ != nullptr) {
        throw std::logic_error("render pass '" + name_ + "' already has a depth/stencil output");
    }
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.setAttachmentInfo(info);
    res.addImageUsage(TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    res.addQueue(queue_);
    res.writtenInPass(index_);
    depthStencilOutput_ = &res;
    return res;
}

RenderTextureResource &RenderPass::addAttachmentInput(const std::string &name) {
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.addImageUsage(TEXTURE_USAGE_INPUT_ATTACHMENT_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    attachmentInputs_.push_back(&res);
    return res;
}

RenderTextureResource &RenderPass::addHistoryInput(const std::string &name) {
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.addImageUsage(TEXTURE_USAGE_SAMPLED_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    historyInputs_.push_back(&res);
    return res;
}

RenderTextureResource &RenderPass::addTextureInput(const std::string &name) {
    RenderTextureResource &res = graph_->getOrCreateTexture(name);
    res.addImageUsage(TEXTURE_USAGE_SAMPLED_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    attachmentInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addUniformInput(const std::string &name) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.addBufferUsage(BUFFER_USAGE_UNIFORM_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    storageInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addStorageReadOnlyInput(const std::string &name) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.addBufferUsage(BUFFER_USAGE_STORAGE_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    storageInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addVertexBufferInput(const std::string &name) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.addBufferUsage(BUFFER_USAGE_VERTEX_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    storageInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addIndexBufferInput(const std::string &name) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.addBufferUsage(BUFFER_USAGE_INDEX_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    storageInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addIndirectBufferInput(const std::string &name) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.addBufferUsage(BUFFER_USAGE_INDIRECT_BIT);
    res.addQueue(queue_);
    res.readInPass(index_);
    storageInputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addStorageOutput(const std::string &name, const BufferInfo &info) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.setBufferInfo(info);
    res.addBufferUsage(BUFFER_USAGE_STORAGE_BIT);
    res.addQueue(queue_);
    res.writtenInPass(index_);
    storageOutputs_.push_back(&res);
    return res;
}

RenderBufferResource &RenderPass::addTransferOutput(const std::string &name, const BufferInfo &info) {
    RenderBufferResource &res = graph_->getOrCreateBuffer(name);
    res.setBufferInfo(info);
    res.addBufferUsage(BUFFER_USAGE_TRANSFER_DST_BIT);
    res.addQueue(queue_);
    res.writtenInPass(index_);
    storageOutputs_.push_back(&res);
    return res;
}

// -- RenderGraph ------------------------------------------------------------

RenderPass &RenderGraph::addPass(const std::string &name, RenderGraphQueueFlagBits queue) {
    if (passByName_.find(name) != passByName_.end()) {
        throw std::logic_error("render graph already has a pass named '" + name + "'");
    }
    const uint32_t index = static_cast<uint32_t>(passes_.size());
    auto pass = std::unique_ptr<RenderPass>(new RenderPass(index, queue));
    pass->setName(name);
    pass->setGraph(this);
    passes_.push_back(std::move(pass));
    passByName_[name] = index;
    baked_ = false;
    return *passes_.back();
}

RenderPass *RenderGraph::findPass(const std::string &name) {
    auto itr = passByName_.find(name);
    return itr == passByName_.end() ? nullptr : passes_[itr->second].get();
}

const RenderPass *RenderGraph::findPass(const std::string &name) const {
    auto itr = passByName_.find(name);
    return itr == passByName_.end() ? nullptr : passes_[itr->second].get();
}

const std::string &RenderGraph::passName(uint32_t index) const noexcept {
    static const std::string kUnknown = "?";
    return index < passes_.size() ? passes_[index]->getName() : kUnknown;
}

RenderTextureResource &RenderGraph::getOrCreateTexture(const std::string &name) {
    auto itr = resourceByName_.find(name);
    if (itr != resourceByName_.end()) {
        auto *existing = dynamic_cast<RenderTextureResource *>(resources_[itr->second].get());
        if (existing == nullptr) {
            throw std::logic_error("render graph resource '" + name + "' was declared as a buffer");
        }
        return *existing;
    }
    const uint32_t index = static_cast<uint32_t>(resources_.size());
    auto res = std::unique_ptr<RenderTextureResource>(new RenderTextureResource(index));
    res->setName(name);
    resources_.push_back(std::move(res));
    resourceByName_[name] = index;
    baked_ = false;
    return *dynamic_cast<RenderTextureResource *>(resources_[index].get());
}

RenderBufferResource &RenderGraph::getOrCreateBuffer(const std::string &name) {
    auto itr = resourceByName_.find(name);
    if (itr != resourceByName_.end()) {
        auto *existing = dynamic_cast<RenderBufferResource *>(resources_[itr->second].get());
        if (existing == nullptr) {
            throw std::logic_error("render graph resource '" + name + "' was declared as a texture");
        }
        return *existing;
    }
    const uint32_t index = static_cast<uint32_t>(resources_.size());
    auto res = std::unique_ptr<RenderBufferResource>(new RenderBufferResource(index));
    res->setName(name);
    resources_.push_back(std::move(res));
    resourceByName_[name] = index;
    baked_ = false;
    return *dynamic_cast<RenderBufferResource *>(resources_[index].get());
}

ResourceDimensions RenderGraph::getResourceDimensions(const RenderTextureResource &resource) const {
    const AttachmentInfo &info = resource.getAttachmentInfo();

    ResourceDimensions dim;
    dim.name = resource.getName();
    dim.layers = info.layers;
    dim.samples = info.samples;
    dim.format = info.format;
    dim.queues = resource.getUsedQueues();
    dim.image_usage = resource.getImageUsage();
    // The caller never sets internal flags; those are derived in bake().
    dim.flags = info.flags & ~(ATTACHMENT_INFO_SUPPORTS_PREROTATE_BIT |
                                ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT |
                                ATTACHMENT_INFO_INTERNAL_PROXY_BIT);

    switch (info.size_class) {
    case SizeClass::SwapchainRelative:
        dim.width = scaledExtent(info.size_x, backbufferDimensions_.width);
        dim.height = scaledExtent(info.size_y, backbufferDimensions_.height);
        dim.depth = scaledExtent(info.size_z, 1u);
        break;

    case SizeClass::Absolute:
        // Absolute truncates rather than ceiling. The two modes differ
        // deliberately in the reference, and a fractional absolute extent means
        // the caller passed something that was not a pixel count.
        dim.width = std::max(static_cast<uint32_t>(info.size_x), 1u);
        dim.height = std::max(static_cast<uint32_t>(info.size_y), 1u);
        dim.depth = std::max(static_cast<uint32_t>(info.size_z), 1u);
        break;

    case SizeClass::InputRelative: {
        auto itr = resourceByName_.find(info.size_relative_name);
        if (itr == resourceByName_.end()) {
            throw std::logic_error("resource '" + resource.getName() + "' is InputRelative to '" +
                                   info.size_relative_name + "', which does not exist");
        }
        const auto *input = dynamic_cast<const RenderTextureResource *>(resources_[itr->second].get());
        if (input == nullptr) {
            throw std::logic_error("resource '" + resource.getName() + "' is InputRelative to buffer '" +
                                   info.size_relative_name + "'");
        }
        if (input == &resource) {
            throw std::logic_error("resource '" + resource.getName() + "' is InputRelative to itself");
        }
        // A -> B -> A is a cycle. The depth guard is a backstop; the self-check
        // above catches the one-node case and this catches the rest.
        if (input->getAttachmentInfo().size_class == SizeClass::InputRelative &&
            input->getAttachmentInfo().size_relative_name == resource.getName()) {
            throw std::logic_error("resource sizing cycle through '" + resource.getName() + "'");
        }
        const ResourceDimensions inputDim = getResourceDimensions(*input);
        dim.width = scaledExtent(info.size_x, inputDim.width);
        dim.height = scaledExtent(info.size_y, inputDim.height);
        dim.depth = scaledExtent(info.size_z, inputDim.depth);
        break;
    }
    }

    // An unspecified format means "whatever the swapchain presents", which is
    // what a plain backbuffer wants.
    if (dim.format == Format::Undefined) {
        dim.format = backbufferDimensions_.format;
    }

    // `levels == 0` requests the full chain; anything else caps it.
    const uint32_t fullChain = fullMipLevelCount(dim.width, dim.height, dim.depth);
    const uint32_t cap = info.levels == 0 ? 0xFFFFFFFFu : info.levels;
    dim.levels = std::min(fullChain, cap);

    return dim;
}

ResourceDimensions RenderGraph::getResourceDimensions(const RenderBufferResource &resource) const {
    ResourceDimensions dim;
    dim.name = resource.getName();
    dim.queues = resource.getUsedQueues();
    dim.buffer_info = resource.getBufferInfo();
    dim.format = Format::Undefined;
    dim.width = 1;
    dim.height = 1;
    dim.depth = 1;
    return dim;
}

const ResourceDimensions &RenderGraph::getPhysicalDimensions(uint32_t physicalIndex) const {
    if (physicalIndex >= physicalDimensions_.size()) {
        throw std::out_of_range("physical resource index is out of range");
    }
    return physicalDimensions_[physicalIndex];
}

void RenderGraph::buildPhysicalSlots() {
    physicalDimensions_.clear();
    physicalImageHasHistory_.clear();
    physicalIsBackbuffer_.clear();

    // Every physical index is cleared first, not just the unassigned ones.
    // Baking is expected to be repeatable -- a dynamic-resolution frame re-bakes
    // against a new swapchain extent every time the scale changes -- and leaving
    // the previous bake's indices in place made every resource on the second
    // bake look already-assigned, so the slot table came back empty and every
    // lookup returned nothing. A re-bake that quietly produces no targets is the
    // worst shape this bug could have taken.
    for (auto &res : resources_) {
        res->setPhysicalIndex(kUnusedResourceIndex);
    }

    // -- 1. which resources are the same memory -----------------------------
    //
    // An output declared to read one of its own pass's inputs is the same
    // memory as that input, provided the two passes merged and the two
    // descriptions agree. Aliasing is decided here, before any slot exists,
    // because a slot is a property of a *group* of resources rather than of one
    // resource. Assigning first and aliasing afterwards leaves the vacated slot
    // in the table, so the physical count reports memory nothing uses and the
    // whole point of the graph -- fewer physical resources than declared ones
    // -- never shows up in the number anyone reads.
    //
    // Union-find keeping the lowest member as the root, so the group's
    // representative is deterministic and the surviving dimensions are
    // whichever resource was declared first.
    std::vector<uint32_t> groupOf(resources_.size());
    std::iota(groupOf.begin(), groupOf.end(), 0u);
    std::function<uint32_t(uint32_t)> findGroup = [&](uint32_t x) {
        while (groupOf[x] != x) {
            groupOf[x] = groupOf[groupOf[x]];
            x = groupOf[x];
        }
        return x;
    };
    auto joinGroups = [&](uint32_t a, uint32_t b) {
        const uint32_t ra = findGroup(a);
        const uint32_t rb = findGroup(b);
        if (ra == rb) {
            return;
        }
        // The lower index wins, so the representative is a function of the
        // declaration rather than of union order.
        groupOf[std::max(ra, rb)] = std::min(ra, rb);
    };

    for (const auto &pass : passes_) {
        for (const auto &pair : pass->getMergedColor()) {
            const RenderTextureResource *out = pair.first;
            const RenderTextureResource *in = pair.second;
            const auto &writers = in->getWritePasses();
            // Exactly one writer, or the merge is ambiguous.
            if (writers.size() != 1) {
                continue;
            }
            const uint32_t writer = *writers.begin();
            if (writer == pass->getIndex()) {
                continue;
            }
            // Different physical passes are different render passes, so the
            // output is a distinct attachment no matter what it was declared to
            // read.
            if (passes_[writer]->getPhysicalPassIndex() != pass->getPhysicalPassIndex()) {
                continue;
            }
            // One slot has one extent and one format. A mismatched pair is left
            // as two slots: the frame asked for something a single attachment
            // cannot express, and quietly picking one of the two would render
            // the wrong image.
            const ResourceDimensions inDim =
                    getResourceDimensions(*const_cast<RenderTextureResource *>(in));
            const ResourceDimensions outDim = getResourceDimensions(*out);
            if (inDim.width != outDim.width || inDim.height != outDim.height ||
                inDim.format != outDim.format) {
                continue;
            }
            joinGroups(in->getIndex(), out->getIndex());
        }
    }

    // -- 2. one physical slot per group --------------------------------------
    for (size_t i = 0; i < resources_.size(); ++i) {
        if (groupOf[i] != static_cast<uint32_t>(i)) {
            // Not the representative; it inherits the slot assigned below.
            continue;
        }
        const uint32_t slot = static_cast<uint32_t>(physicalDimensions_.size());
        auto *rep = resources_[i].get();
        rep->setPhysicalIndex(slot);
        if (rep->getType() == RenderResource::Type::Texture) {
            physicalDimensions_.push_back(
                    getResourceDimensions(*static_cast<RenderTextureResource *>(rep)));
        } else {
            physicalDimensions_.push_back(getResourceDimensions(*static_cast<RenderBufferResource *>(rep)));
        }
        physicalImageHasHistory_.push_back(false);
        physicalIsBackbuffer_.push_back(false);

        // Every member of the group shares the image, so the image has to carry
        // all of their uses and queues. A slot that only recorded the
        // representative's usage would be allocated without the flags the
        // second pass needs, and the failure would surface as an invalid
        // layout transition rather than as anything obvious here.
        for (size_t j = 0; j < resources_.size(); ++j) {
            if (findGroup(static_cast<uint32_t>(j)) != static_cast<uint32_t>(i)) {
                continue;
            }
            auto *member = resources_[j].get();
            member->setPhysicalIndex(slot);
            if (member->getType() != rep->getType()) {
                throw std::logic_error("aliased resources '" + rep->getName() + "' and '" +
                                       member->getName() + "' are of different kinds");
            }
            if (member->getType() == RenderResource::Type::Texture) {
                physicalDimensions_[slot].image_usage |=
                        static_cast<uint32_t>(static_cast<RenderTextureResource *>(member)->getImageUsage());
            }
            physicalDimensions_[slot].queues |= member->getUsedQueues();
        }
    }

    // History reads disqualify their physical slot from ever being transient.
    for (const auto &pass : passes_) {
        for (const auto *hist : pass->getHistoryInputs()) {
            const uint32_t pi = hist->getPhysicalIndex();
            if (pi < physicalImageHasHistory_.size()) {
                physicalImageHasHistory_[pi] = true;
            }
        }
    }

    if (!backbufferSource_.empty()) {
        auto itr = resourceByName_.find(backbufferSource_);
        if (itr == resourceByName_.end()) {
            throw std::logic_error("backbuffer source '" + backbufferSource_ + "' does not exist");
        }
        const uint32_t pi = resources_[itr->second]->getPhysicalIndex();
        if (pi < physicalIsBackbuffer_.size()) {
            physicalIsBackbuffer_[pi] = true;
        }
    }

    for (size_t i = 0; i < physicalDimensions_.size(); ++i) {
        if (physicalIsBackbuffer_[i]) {
            // The presented image is owned by the presentation engine, not by
            // an allocation the graph makes.
            physicalDimensions_[i].flags |= ATTACHMENT_INFO_PERSISTENT_BIT;
        }
    }
}

void RenderGraph::assignPhysicalPasses() {
    // A pass merges with the pass that wrote its colour input, so that an
    // input/output pair occupies one render pass and can share a slot.
    //
    // Union-find rather than a fixed-point loop over the pass list: merging is
    // transitive (A reads B, B reads C, so A, B and C can all be one pass) and
    // a loop that stopped after one sweep would silently under-merge.
    std::vector<uint32_t> parent(passes_.size());
    std::iota(parent.begin(), parent.end(), 0u);

    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    for (const auto &pass : passes_) {
        for (const auto *in : pass->getColorInputs()) {
            const auto &writers = in->getWritePasses();
            // Exactly one writer, or the merge is ambiguous. Guessing here would
            // reorder a frame, and a reordered frame is a wrong frame.
            if (writers.size() != 1) {
                continue;
            }
            const uint32_t writer = *writers.begin();
            if (writer == pass->getIndex()) {
                continue;
            }
            if (passes_[writer]->getQueue() != pass->getQueue()) {
                // Different queues cannot share a render pass; they need a
                // semaphore instead, which is a real dependency, not a merge.
                continue;
            }
            const uint32_t ra = find(pass->getIndex());
            const uint32_t rb = find(writer);
            if (ra != rb) {
                parent[ra] = rb;
            }
        }
    }

    // Assign one physical pass index per group, in first-declaration order, so
    // the numbering is a deterministic function of the declaration.
    std::map<uint32_t, uint32_t> groupToPhysical;
    for (size_t i = 0; i < passes_.size(); ++i) {
        const uint32_t root = find(static_cast<uint32_t>(i));
        auto itr = groupToPhysical.find(root);
        if (itr == groupToPhysical.end()) {
            const uint32_t phys = static_cast<uint32_t>(groupToPhysical.size());
            groupToPhysical[root] = phys;
            passes_[i]->setPhysicalPassIndex(phys);
        } else {
            passes_[i]->setPhysicalPassIndex(itr->second);
        }
    }
}

void RenderGraph::deriveDependencies() {
    dependencies_.clear();
    passDependencies_.assign(passes_.size(), {});

    for (const auto &res : resources_) {
        const auto &writers = res->getWritePasses();
        const auto &readers = res->getReadPasses();

        // A read with no writer is a broken frame: the pass samples memory that
        // was never produced this frame. Left alone it renders whatever the
        // allocator happened to hand out.
        if (writers.empty() && !readers.empty()) {
            throw std::logic_error("resource '" + res->getName() + "' is read but never written");
        }
        if (writers.empty()) {
            continue;
        }

        for (uint32_t reader : readers) {
            for (uint32_t writer : writers) {
                if (reader == writer) {
                    continue;
                }
                // A pass that reads and writes the same resource has to read
                // before it writes; a pass that only reads just has to follow
                // the writer. The self-edge, where reader == writer, is skipped
                // above, so a single-writer read-modify-write still classifies
                // as WAR.
                const bool readerWrites = writers.count(reader) != 0;
                const auto kind = readerWrites ? PassDependency::Kind::WriteAfterRead
                                               : PassDependency::Kind::ReadAfterWrite;
                dependencies_.push_back(PassDependency{reader, writer, kind});
                passDependencies_[reader].push_back(writer);
            }
        }

        // Two passes writing the same target are ordered against each other as
        // well. Without this edge a reader of the target still gets ordered after
        // both writers, but the two writers themselves are unordered relative to
        // one another, so the scheduler is free to run them in either order and
        // the last writer is not the one the frame declared as final.
        //
        // Emitted after the reader edges, and only for pairs nothing else
        // already ordered, so that a pass which both reads and writes a target
        // is classified by the more specific read-modify-write rather than
        // being labelled twice for one ordering constraint.
        if (writers.size() > 1) {
            std::vector<uint32_t> orderedWriters(writers.begin(), writers.end());
            for (size_t i = 1; i < orderedWriters.size(); ++i) {
                const uint32_t earlier = orderedWriters[i - 1];
                const uint32_t later = orderedWriters[i];
                if (std::find(passDependencies_[later].begin(), passDependencies_[later].end(), earlier) !=
                    passDependencies_[later].end()) {
                    continue;
                }
                dependencies_.push_back(PassDependency{later, earlier, PassDependency::Kind::WriteAfterWrite});
                passDependencies_[later].push_back(earlier);
            }
        }
    }

    for (auto &deps : passDependencies_) {
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
}

void RenderGraph::buildPassOrder() {
    flattenedPasses_.clear();
    flattenedPasses_.reserve(passes_.size());

    // Kahn's algorithm. A pass with no unmet dependency goes first, and ties
    // break on declaration order, so the result is a deterministic function of
    // the declaration rather than of set iteration.
    std::vector<uint32_t> remaining(passes_.size());
    std::iota(remaining.begin(), remaining.end(), 0u);

    std::vector<uint32_t> indegree(passes_.size(), 0);
    for (size_t i = 0; i < passes_.size(); ++i) {
        for (uint32_t dep : passDependencies_[i]) {
            if (dep != static_cast<uint32_t>(i)) {
                indegree[i]++;
            }
        }
    }

    while (!remaining.empty()) {
        auto next = std::find_if(remaining.begin(), remaining.end(),
                                 [&](uint32_t p) { return indegree[p] == 0; });
        if (next == remaining.end()) {
            // Whatever is left is mutually dependent. Naming the passes makes
            // the cycle debuggable; a bare "cycle detected" does not.
            std::string names;
            for (size_t i = 0; i < remaining.size(); ++i) {
                if (i != 0) {
                    names += ", ";
                }
                names += passes_[remaining[i]]->getName();
            }
            throw std::logic_error("render graph has a dependency cycle among passes: " + names);
        }
        const uint32_t current = *next;
        remaining.erase(next);
        flattenedPasses_.push_back(current);

        for (uint32_t other : remaining) {
            const auto &deps = passDependencies_[other];
            if (std::find(deps.begin(), deps.end(), current) != deps.end()) {
                indegree[other]--;
            }
        }
    }
}

void RenderGraph::buildTransients() {
    // A physical slot is transient only if its whole lifetime is inside one
    // render pass. Track which physical pass each slot was last seen in; a
    // second, different one means it spans a boundary and must persist.
    std::vector<uint32_t> physicalPassUsed(physicalDimensions_.size(), RenderPass::kUnusedPassIndex);

    for (auto &dim : physicalDimensions_) {
        // Memory rather than an image never goes in transient attachment space.
        if (dim.isBufferLike()) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }
        // A resource read from last frame has no persistent memory at all.
        if (dim.isTransient()) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }

        const size_t index = static_cast<size_t>(&dim - physicalDimensions_.data());
        if (index < physicalImageHasHistory_.size() && physicalImageHasHistory_[index]) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }
        if (physicalIsBackbuffer_[index]) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }

        // Depth and stencil need tile memory to be transient at all, and not
        // every implementation offers it. Colour transient is legal
        // everywhere, so it stays a candidate and is decided by lifetime alone.
        if (formatHasDepthOrStencil(dim.format) && !useTransientDepthStencil_) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }
        dim.flags |= ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
    }

    for (const auto &res : resources_) {
        const uint32_t physicalIndex = res->getPhysicalIndex();
        if (physicalIndex == kUnusedResourceIndex || physicalIndex >= physicalPassUsed.size()) {
            continue;
        }
        for (uint32_t pass : res->getWritePasses()) {
            const uint32_t phys = passes_[pass]->getPhysicalPassIndex();
            if (phys == RenderPass::kUnusedPassIndex) {
                continue;
            }
            if (physicalPassUsed[physicalIndex] != RenderPass::kUnusedPassIndex &&
                phys != physicalPassUsed[physicalIndex]) {
                physicalDimensions_[physicalIndex].flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
                break;
            }
            physicalPassUsed[physicalIndex] = phys;
        }
        for (uint32_t pass : res->getReadPasses()) {
            const uint32_t phys = passes_[pass]->getPhysicalPassIndex();
            if (phys == RenderPass::kUnusedPassIndex) {
                continue;
            }
            if (physicalPassUsed[physicalIndex] != RenderPass::kUnusedPassIndex &&
                phys != physicalPassUsed[physicalIndex]) {
                physicalDimensions_[physicalIndex].flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
                break;
            }
            physicalPassUsed[physicalIndex] = phys;
        }
    }
}

void RenderGraph::bake() {
    if (passes_.empty()) {
        throw std::logic_error("cannot bake a render graph with no passes");
    }
    if (backbufferDimensions_.width == 0 || backbufferDimensions_.height == 0) {
        throw std::logic_error("cannot bake a render graph with a zero-sized backbuffer");
    }
    if (backbufferDimensions_.format == Format::Undefined) {
        throw std::logic_error("cannot bake a render graph whose backbuffer format is undefined");
    }

    assignPhysicalPasses();
    buildPhysicalSlots();
    deriveDependencies();
    buildPassOrder();
    buildTransients();
    baked_ = true;
}

void RenderGraph::reset() {
    passes_.clear();
    resources_.clear();
    passByName_.clear();
    resourceByName_.clear();
    physicalDimensions_.clear();
    dependencies_.clear();
    passDependencies_.clear();
    flattenedPasses_.clear();
    physicalImageHasHistory_.clear();
    physicalIsBackbuffer_.clear();
    backbufferSource_.clear();
    baked_ = false;
}

std::string RenderGraph::describe() const {
    std::string out;
    out += "render graph: " + std::to_string(passes_.size()) + " passes, " +
           std::to_string(resources_.size()) + " resources, " +
           std::to_string(physicalDimensions_.size()) + " physical slots\n";
    for (uint32_t index : flattenedPasses_) {
        out += "  [" + std::to_string(index) + "] " + passes_[index]->getName() + "\n";
    }
    for (const auto &dep : dependencies_) {
        out += "  " + passes_[dep.dependent]->getName() + " <- " + passes_[dep.dependency]->getName();
        switch (dep.kind) {
        case PassDependency::Kind::ReadAfterWrite:
            out += " (RAW)";
            break;
        case PassDependency::Kind::WriteAfterRead:
            out += " (WAR)";
            break;
        case PassDependency::Kind::WriteAfterWrite:
            out += " (WAW)";
            break;
        }
        out += "\n";
    }
    for (size_t i = 0; i < physicalDimensions_.size(); ++i) {
        const auto &dim = physicalDimensions_[i];
        if (dim.isBufferLike()) {
            out += "  slot " + std::to_string(i) + " " + dim.name + ": " +
                   std::to_string(dim.buffer_info.size) + " bytes (buffer)\n";
            continue;
        }
        out += "  slot " + std::to_string(i) + " " + dim.name + ": " + std::to_string(dim.width) + "x" +
               std::to_string(dim.height) + " " + std::to_string(dim.levels) + "m " +
               (dim.isTransient() ? "transient" : "persistent") + "\n";
    }
    return out;
}

} // namespace emergent
