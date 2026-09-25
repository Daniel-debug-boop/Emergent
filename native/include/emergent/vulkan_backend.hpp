#pragma once
#include <cstdint>
#include <string>

namespace emergent {
struct VulkanCapabilities {
    bool loader = false;
    bool instance = false;
    bool physical_device = false;
    bool device = false;
    bool queue = false;
    bool compute = false;
    bool transfer = false;
    bool indirect_draw = false;
    bool descriptor_indexing = false;
    bool subgroup = false;
    bool mesh_shader = false;
    bool task_shader = false;
    std::string device_name;
    std::string api_version;
    std::string error;
};

struct NativeGpuRenderStats {
    bool executed = false;
    uint32_t width = 0;
    uint32_t height = 0;
    double submit_ms = 0.0;
    std::string error;
};

class VulkanBackend {
public:
    bool initialize();
    void shutdown();
    const VulkanCapabilities& capabilities() const { return caps_; }
    bool initialized() const { return initialized_; }
    NativeGpuRenderStats renderBootstrap(uint32_t width = 256, uint32_t height = 256);
private:
    void* loader_ = nullptr;
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    void* queue_ = nullptr;
    void* command_pool_ = nullptr;
    void* allocator_ = nullptr;
    uint32_t queue_family_ = 0;
    VulkanCapabilities caps_{};
    bool initialized_ = false;
};
}
