#pragma once
#include "emergent/frame_loop.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace emergent {

// A window, reduced to what a renderer actually needs from one.
//
// EMERGENT has no windowing dependency, and that is a decision rather than an
// omission. A window library owns a display connection, an event queue and a
// native surface handle; all three are platform-specific, none of them can be
// stubbed without inventing a fake display server, and the whole point of this
// interface is that the engine never has to know which one it is talking to.
//
// The consequence is that a real window is one class. Implement SurfaceProvider
// over GLFW, SDL, XCB or Win32, hand it to a renderer, and the loop starts
// presenting. Nothing in the frame loop, the physics, the animation or the
// render path changes to accommodate it.
class SurfaceProvider {
public:
    virtual ~SurfaceProvider() = default;

    // The native window handle for the target platform's Vulkan WSI extension:
    // HWND on Win32, xcb_window_t or an Xlib Window on Linux, NSWindow* on
    // macOS. nullptr is a valid answer meaning "no window": a renderer then
    // targets an offscreen image and reports that nothing is on a display,
    // rather than failing.
    virtual void *nativeWindow() = 0;

    // Drawable size in pixels. False when unavailable, which is how a
    // minimised window is represented.
    virtual bool framebufferSize(uint32_t &width, uint32_t &height) = 0;

    // False while minimised or hidden. A renderer should skip presenting
    // rather than submit into a zero-sized swapchain.
    virtual bool visible() = 0;

    virtual std::string_view kind() const = 0;

    /**
     * Create this platform's VkSurfaceKHR for the window, or return nullptr.
     *
     * The handle is returned as void* so this header stays free of vulkan.h,
     * and so the renderer can be built on a machine with no Vulkan SDK
     * headers. nullptr is the normal answer for a provider that has no
     * surface to offer: the renderer then renders offscreen and reports that
     * nothing is being presented. It is not an error.
     *
     * The surface belongs to the provider. The renderer destroys it through
     * the same provider.
     */
    virtual void *createVulkanSurface(void *vkInstance) { (void)vkInstance; return nullptr; }
    virtual void destroyVulkanSurface(void *vkSurface) { (void)vkSurface; }

    /**
     * Instance extensions this provider needs enabled.
     *
     * Returned before the instance exists, because instance extensions can
     * only be requested at vkCreateInstance time and a platform surface cannot
     * be created without its extension. An XCB provider returns
     * {"VK_KHR_surface", "VK_KHR_xcb_surface"}; a Win32 provider returns the
     * win32 pair. A headless provider returns nothing.
     */
    virtual std::vector<const char *> requiredInstanceExtensions() const { return {}; }
};

// What the renderer did, per run. Every counter here is a count of something
// that actually happened.
struct RenderFrameStats {
    uint64_t submittedFrames = 0;  // frames recorded, submitted and presented
    uint64_t skippedFrames = 0;    // frames the renderer declined to draw
    uint32_t width = 0;
    uint32_t height = 0;
    double cpuMs = 0.0;            // time in record+submit
    double gpuMs = 0.0;            // 0 unless a timestamp query was actually resolved
    bool hasDevice = false;
    bool hasSwapchain = false;
    /**
     * Triangles submitted by the scene pipeline, per frame.
     *
     * Separated from the debug-box count on purpose. "The renderer drew 1.2
     * million triangles" and "the renderer drew 900 boxes and 1.2 million
     * triangles" are very different claims, and averaging them into one number
     * is how a debug renderer ends up reporting a convincing performance number
     * for geometry that was never in the frame.
     */
    uint32_t drawnTriangles = 0;
    /** Triangles submitted through the debug instance path. */
    uint32_t drawnBoxes = 0;
    /** False while the material maps hold neutral placeholders, not the CC0 bake. */
    bool materialMapsLoaded = false;
    /** Empty when the maps are real; otherwise why the scene is untextured. */
    std::string materialMapNote;
    std::string status;
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    virtual std::string_view name() const = 0;

    /**
     * Acquire whatever this backend needs to draw.
     *
     * @param surface May be nullptr, meaning offscreen. Offscreen is a fully
     *        supported mode, not a degraded one: it is what the test suite and
     *        the headless frame loop use.
     * @return false with stats().status explaining why. A backend must never
     *         return true and then quietly draw nothing.
     */
    virtual bool open(uint32_t width, uint32_t height, SurfaceProvider *surface) = 0;
    virtual void close() noexcept = 0;
    virtual bool opened() const = 0;

    // The three-phase frame. beginFrame acquires whatever the backend needs
    // for this frame, drawFrame records the scene, endFrame submits and
    // presents. Split rather than a single call because a real backend has to
    // wait on a fence between acquire and record, and a single call would
    // hide that.
    virtual void beginFrame() = 0;
    virtual void drawFrame(const FrameState &state) = 0;
    virtual void endFrame() = 0;

    virtual const RenderFrameStats &stats() const = 0;

    // True only if pixels are reaching a display. False for offscreen
    // rendering, and false when no window is attached, and this is the flag
    // that must never be true on a machine with no GPU.
    virtual bool presentsToDisplay() const = 0;

    // The framebuffer was resized since the last frame and the swapchain
    // needs rebuilding. A window backend sets this from a resize event.
    virtual void requestResize() = 0;
};

/**
 * A renderer that draws nothing, and says so.
 *
 * Not a stub. It is the backend the headless frame loop and the test suite
 * run against, and it does the one job that is useful without a display: it
 * validates the FrameState it is handed and remembers it. A frame loop that
 * hands a renderer null joint matrices or a non-finite position fails here
 * instead of at a swapchain, on a machine that has a GPU, weeks later.
 */
class NullRenderBackend final : public RenderBackend {
public:
    NullRenderBackend();
    ~NullRenderBackend() override;

    std::string_view name() const override { return "null"; }
    bool open(uint32_t width, uint32_t height, SurfaceProvider *surface) override;
    void close() noexcept override;
    bool opened() const override { return opened_; }

    void beginFrame() override;
    void drawFrame(const FrameState &state) override;
    void endFrame() override;

    const RenderFrameStats &stats() const override { return stats_; }
    bool presentsToDisplay() const override { return false; }
    void requestResize() override { resizeRequested_ = true; }

    // How many frames this backend refused because the frame state was
    // unusable. A non-zero value after a run means the loop produced a frame
    // the renderer could not draw, which is a bug in the loop.
    uint64_t rejectedFrames() const { return stats_.skippedFrames; }
    // True between beginFrame() and endFrame().
    bool inFrame() const { return inFrame_; }
    // A copy of the last frame state, so a caller can inspect what was drawn
    // after the fact. NullRenderBackend copies precisely because it is the
    // backend a test inspects, and because the loop's own buffer is only
    // valid until the next tick.
    bool lastFrame(FrameState &out) const;
    bool resizeRequested() const { return resizeRequested_; }

private:
    RenderFrameStats stats_{};
    FrameState last_{};
    bool hasLast_ = false;
    bool opened_ = false;
    bool inFrame_ = false;
    // Whether the frame currently open was drawn. endFrame() consults it so a
    // frame rejected by validation is never counted as submitted.
    bool frameAccepted_ = false;
    bool resizeRequested_ = false;
};

// The shared shader-loading and validation the Vulkan backend is built on.
// Split out because the validation is worth testing on a machine with no GPU:
// a corrupt or truncated SPIR-V module passed to vkCreateShaderModule is a
// crash or a driver-level abort, and the check that prevents it is pure logic.

// SPIR-V magic number. Every module begins with it, little-endian.
inline constexpr uint32_t kSpirvMagic = 0x07230203u;

/**
 * Reads a SPIR-V module from disk.
 *
 * Validates what can be validated without a device: the file exists, it is a
 * whole number of 4-byte words (a truncated module reads as garbage rather
 * than failing, which is how a bad download becomes a crash), and it starts
 * with the SPIR-V magic.
 *
 * @return false with `error` set. Never returns a partially filled buffer.
 */
bool loadSpirvModule(const std::string &path, std::vector<uint32_t> &out, std::string &error);

} // namespace emergent
