#pragma once
#include "emergent/vulkan_backend.hpp"
#include "emergent/jolt_physics.hpp"
#include "emergent/optimization.hpp"
#include "emergent/navigation.hpp"
#include "emergent/ecs_world.hpp"
#include "emergent/audio.hpp"
#include "emergent/animation.hpp"
#include "emergent/asset_pack.hpp"
#include "emergent/frame_loop.hpp"
#include "emergent/render_backend.hpp"
#include <cstdint>
#include <memory>
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
    NativeEngine();
    ~NativeEngine();
    NativeEngine(const NativeEngine&) = delete;
    NativeEngine& operator=(const NativeEngine&) = delete;

    /**
     * Name the window the engine will present to, before initialize().
     *
     * Vulkan instance extensions can only be requested at instance creation,
     * and a platform surface extension is one of them. A window attached after
     * initialize() therefore finds an instance that cannot create a surface at
     * all -- so this has to be known up front, which is why it is a
     * configure-then-initialize call rather than a setter.
     */
    void setSurfaceProvider(SurfaceProvider *surface) { surface_ = surface; }
    SurfaceProvider *surface() const { return surface_; }

    bool initialize();
    int runSelfTest();
    NativeGpuRenderStats renderBootstrap(uint32_t width = 256, uint32_t height = 256);
    void shutdown();
    const VulkanCapabilities &vulkan() const { return vk_.capabilities(); }

    // -- the frame loop ----------------------------------------------------
    //
    // initialize() brings the loop up with the default pacing. Call this
    // before initialize() to change it; afterwards, reconfigureFrameLoop()
    // tears the loop down and rebuilds it, which is the only safe way to
    // change a timestep on a loop that is already simulating.
    bool configureFrameLoop(const FrameLoopConfig &config);
    bool reconfigureFrameLoop(const FrameLoopConfig &config);
    const FrameLoop &loop() const { return loop_; }

    // Input for the next updateFrame(). Held until replaced, so a caller that
    // polls input at a different rate than it ticks frames does not lose a
    // keypress between frames.
    void setInput(const InputState &input) { input_ = input; }
    const InputState &input() const { return input_; }

    /**
     * Run one frame, taking the delta from the monotonic clock.
     *
     * This is the real-time path: it measures how long the previous frame took
     * and hands that to the loop, which is what paces the simulation against
     * the wall clock. Returns false if the loop or the renderer is not ready.
     */
    bool updateFrame();

    /**
     * Run `frames` frames of exactly `delta` seconds, ignoring the clock.
     *
     * Deterministic: the same arguments always produce the same state, which
     * is what makes the engine testable and a recorded input stream
     * replayable. Returns the number of frames completed.
     */
    uint32_t runHeadless(uint32_t frames, double delta = 1.0 / 60.0);

    /**
     * Run for `seconds` of wall-clock time, using the real clock for pacing.
     *
     * Sleeps between frames when it is running ahead of real time, so this
     * is the loop's pacing behaviour rather than just its arithmetic. Returns
     * the number of frames completed. Intended for a soak test and for a
     * windowed binary whose window system is doing the presenting; a real
     * engine blocks in the window's event loop instead, which is a different
     * loop and belongs in the window's backend.
     */
    uint32_t runRealtime(double seconds);

    const FrameStats &frameStats() const { return loop_.stats(); }

    // -- rendering ---------------------------------------------------------

    /**
     * Attach a renderer and open its target.
     *
     * The backend is borrowed and must outlive the engine. Passing nullptr
     * installs the built-in headless backend, which is what the engine starts
     * with: an engine that has been asked to run frames and cannot draw them
     * should say so in one place rather than being a null pointer at the call
     * site.
     *
     * @param surface May be nullptr, meaning offscreen. Offscreen is a real
     *        mode: the frame loop runs, frames are rendered, and nothing is
     *        presented to a display.
     */
    bool attachRenderBackend(RenderBackend *backend);
    // Why the last renderTo()/enableVulkanRenderer() call failed, or an empty
    // string if it did not. The renderer being asked is a different object
    // from the one being reported on after a failed attach, so a caller needs
    // this to say anything useful about what went wrong.
    const std::string &lastRenderError() const { return lastRenderError_; }
    /**
     * Create, own and attach the engine's Vulkan renderer.
     *
     * Distinct from attachRenderBackend() because this one allocates the
     * backend and binds it to the engine's own device; a caller supplying its
     * own backend has to have bound it to a device itself. Requires
     * initialize() to have run, because there is no device before that.
     */
    bool enableVulkanRenderer();
    bool renderTo(uint32_t width, uint32_t height, SurfaceProvider *surface = nullptr);
    void closeRenderTarget();
    const RenderBackend &renderer() const { return *renderer_; }
    const RenderFrameStats &renderStats() const { return renderer_->stats(); }

    // -- content packs -----------------------------------------------------
    const AnimationLibrary &animation() const { return anim_; }
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
    FrameLoop loop_;

    // Not owned: the headless backend is the default so the engine is never
    // holding a null renderer, and a Vulkan renderer borrows the device above.
    RenderBackend *renderer_ = nullptr;
    std::unique_ptr<NullRenderBackend> nullRenderer_;
    std::unique_ptr<VulkanRenderBackend> vulkanRenderer_;

    FrameLoopConfig loopConfig_{};
    InputState input_{};
    SurfaceProvider *surface_ = nullptr;
    uint64_t lastFrameNanos_ = 0;
    bool clockPrimed_ = false;
    bool renderTargetOpen_ = false;
    std::string lastRenderError_;

    void primeClock();
};

} // namespace emergent
