#pragma once
#include "emergent/render_backend.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

// Instance-level options.
//
// WSI extensions can only be requested at vkCreateInstance time, and the only
// way to know whether a window is going to be attached later is to say so up
// front. Asking after the instance exists cannot work, which is why this is
// not a setter.
struct VulkanInitOptions {
    // Request the surface and swapchain instance extensions. Costs nothing
    // when no window is ever attached, and is required when one is.
    bool wantSurface = false;
    uint32_t width = 0;
    uint32_t height = 0;
    // Platform surface extensions to enable, normally from a
    // SurfaceProvider's requiredInstanceExtensions(). Instance extensions
    // cannot be added after vkCreateInstance, so this has to be known here.
    std::vector<const char *> extraInstanceExtensions;
};

class VulkanBackend {
public:
    bool initialize(const VulkanInitOptions &options = VulkanInitOptions{});
    void shutdown();
    const VulkanCapabilities& capabilities() const { return caps_; }
    bool initialized() const { return initialized_; }
    NativeGpuRenderStats renderBootstrap(uint32_t width = 256, uint32_t height = 256);
    void* createOffscreenSurface(uint32_t width, uint32_t height, std::string &error);
    void destroyOffscreenSurface();
private:
    friend class VulkanRenderBackend;
    void* loader_ = nullptr;
    void* instance_ = nullptr;
    void* physical_device_ = nullptr;
    void* device_ = nullptr;
    void* queue_ = nullptr;
    void* command_pool_ = nullptr;
    void* allocator_ = nullptr;
    uint32_t queue_family_ = 0;
    // Offscreen render target, owned by the backend because the allocator that
    // backs it dies with the device.
    void *offscreen_image_ = nullptr;
    void *offscreen_allocation_ = nullptr;
    uint32_t offscreen_width_ = 0;
    uint32_t offscreen_height_ = 0;
    VulkanCapabilities caps_{};
    bool initialized_ = false;
};

/**
 * The windowed / offscreen renderer.
 *
 * Draws what the frame loop produces: the scene's boxes at their interpolated
 * positions, and the resolved ozz skeleton as joint boxes driven straight from
 * the pose matrices. That is deliberately a debug presentation rather than a
 * game renderer -- its job is to make the loop's output visible, so that a
 * wrong alpha or a stale joint matrix is something you can see rather than
 * something you have to infer.
 *
 * Two modes, chosen automatically:
 *   - With a SurfaceProvider that can produce a VkSurfaceKHR: a real
 *     swapchain, presented each frame.
 *   - Otherwise: an offscreen colour+depth target at the requested size,
 *     rendered and submitted every frame with nothing presented. This is a
 *     supported mode, not a fallback to apologise for -- it is what a server
 *     build, a frame-capture harness and the test suite want.
 *
 * The platform surface is created by the SurfaceProvider, not here. Creating
 * one means including that platform's window headers, and the whole reason
 * SurfaceProvider exists is that the engine should not have to know which
 * platform it is on.
 */
class VulkanRenderBackend final : public RenderBackend {
public:
    VulkanRenderBackend();
    ~VulkanRenderBackend() override;

    std::string_view name() const override { return "vulkan"; }

    // `device` is borrowed and must outlive this backend. A null device is a
    // caller error, not a runtime condition: the backend reports it.
    bool initialize(VulkanBackend &device, const VulkanInitOptions &options);
    void shutdownDevice();

    bool open(uint32_t width, uint32_t height, SurfaceProvider *surface) override;
    void close() noexcept override;
    bool opened() const override { return opened_; }

    void beginFrame() override;
    void drawFrame(const FrameState &state) override;
    void endFrame() override;

    const RenderFrameStats &stats() const override { return stats_; }
    bool presentsToDisplay() const override { return hasSwapchain_; }
    void requestResize() override { resizeRequested_ = true; }

    // Directory the SPIR-V modules are loaded from. Defaults to
    // "native/shaders", which is where the build puts them. Set this before
    // open().
    void setShaderDirectory(const std::string &dir) { shaderDir_ = dir; }
    const std::string &shaderDirectory() const { return shaderDir_; }

    // True when every pipeline-critical resource was created. False means the
    // backend will decline to draw, and stats().status says which piece is
    // missing. Reported separately from opened() because "the device is up"
    // and "I can draw a frame" are different claims.
    bool renderPipelineReady() const { return pipelineReady_; }

    // Orbit camera around the frame's cameraTarget. Degrees, and a world-space
    // distance. Set before or between frames; a static camera is fine and is
    // the default.
    void setCamera(float yawDegrees, float pitchDegrees, float distance);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VulkanBackend *device_ = nullptr;
    RenderFrameStats stats_{};
    bool opened_ = false;
    bool pipelineReady_ = false;
    bool hasSwapchain_ = false;
    bool resizeRequested_ = false;
    std::string shaderDir_ = "native/shaders";
};

} // namespace emergent
