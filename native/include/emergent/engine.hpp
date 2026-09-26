#pragma once
#include "emergent/vulkan_backend.hpp"
#include "emergent/jolt_physics.hpp"
#include "emergent/optimization.hpp"
#include "emergent/navigation.hpp"
#include "emergent/ecs_world.hpp"
#include "emergent/audio.hpp"
#include "emergent/animation.hpp"
#include "emergent/asset_pack.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace emergent {

struct ObjectGPU {
    float x, y, z, radius;
    uint32_t material, lod;
    float importance, pad;
};

class NativeEngine {
public:
    bool initialize();
    int runSelfTest();
    NativeGpuRenderStats renderBootstrap(uint32_t width = 256, uint32_t height = 256);
    void shutdown();
    const VulkanCapabilities &vulkan() const { return vk_.capabilities(); }

    // Skeletal animation. The rig and its locomotion clips are baked during
    // initialize(), so `animation()` is usable once initialize() has returned
    // true.
    const AnimationLibrary &animation() const { return anim_; }

    // Content packs. A pack is a compressed bundle of named blobs; see
    // asset_pack.hpp for the format.
    bool writeContentPack(const std::string &path, int level = 9);
    bool loadContentPack(const std::string &path);
    size_t contentEntryCount() const;
    bool readContent(std::string_view name, std::vector<uint8_t> &out) const;
    double contentCompressionRatio() const;

private:
    VulkanBackend vk_;
    JoltPhysicsWorld physics_;
    EcsWorld ecs_;
    AudioSystem audio_;
    AnimationLibrary anim_;
    AssetPack content_;
};

} // namespace emergent
