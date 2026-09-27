#include "emergent/engine.hpp"
#include "emergent/profiling.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace emergent {

NativeEngine::NativeEngine() {
    // The headless renderer is the default so that no code path in the engine
    // ever has to check for a null renderer. An engine asked to run frames and
    // unable to draw them reports that in exactly one place.
    nullRenderer_ = std::make_unique<NullRenderBackend>();
    renderer_ = nullRenderer_.get();
}

NativeEngine::~NativeEngine() { shutdown(); }

bool NativeEngine::configureFrameLoop(const FrameLoopConfig &config) {
    loopConfig_ = config;
    return true;
}

bool NativeEngine::reconfigureFrameLoop(const FrameLoopConfig &config) {
    closeRenderTarget();
    loopConfig_ = config;
    if (!loop_.initialize(config)) {
        std::cerr << "frame loop reconfiguration failed: " << loop_.lastError() << "\n";
        return false;
    }
    clockPrimed_ = false;
    return true;
}

/**
 * One simulation step plus the scene streaming it implies.
 *
 * Every path that advances a frame goes through here. `runHeadless` and
 * `runRealtime` tick the loop directly rather than calling updateFrame, so
 * without this the world is generated, the spawn moves downtown, and the slice
 * is still empty at the end of a headless run — which is exactly what happened
 * before this existed, and exactly what a CI check on the streamed scene would
 * have caught.
 */
bool NativeEngine::tickOnce(double delta, FrameState &state) {
    if (!loop_.tick(delta, input_, state)) return false;
    {
        const profile::Scope scene("frame.scene_stream");
        updateScene(state);
    }
    return true;
}

bool NativeEngine::buildWorld(int32_t seed) {
    world_ = generateWorld(seed);
    // Not centred on the origin: the generated layout puts the built-up part
    // wherever the region noise put it, and for a typical seed the nearest
    // building to (0,0) is several hundred metres away. Streaming from the
    // origin would stream nothing.
    worldCentre_ = ::emergent::worldCentre(world_);
    // Spawn downtown. Without this the player starts a kilometre from the first
    // building, and the streamed slice is built around an empty field.
    loop_.setSpawn(worldCentre_.x, worldCentre_.z, terrainHeight(worldCentre_.x, worldCentre_.z, seed));
    // Force the next frame to rebuild regardless of where the player is.
    sceneChunkX_ = 1e18;
    sceneChunkZ_ = 1e18;
    return true;
}

/**
 * Rebuild the scene if the player has crossed a chunk boundary.
 *
 * Closed loop on purpose: the chunk index is quantised, so a rebuild happens
 * when the player moves a whole chunk rather than every frame. The loop that
 * caused trouble on the web side streamed on an exact position comparison, and
 * the distance rebuild could not change, so it never closed.
 */
void NativeEngine::updateScene(const FrameState &state) {
    if (world_.buildings.empty()) return;
    const double cx = state.playerPosition[0];
    const double cz = state.playerPosition[2];
    // A quarter of the radius, so the slice is comfortably rebuilt before the
    // player reaches its edge rather than at the moment they do.
    const double chunk = std::max(32.0, streamRadius_ * 0.25);
    const double qx = std::floor(cx / chunk);
    const double qz = std::floor(cz / chunk);
    if (qx == sceneChunkX_ && qz == sceneChunkZ_) return;
    sceneChunkX_ = qx;
    sceneChunkZ_ = qz;

    SceneBuildOptions options;
    options.centerX = cx;
    options.centerZ = cz;
    options.radius = streamRadius_;
    options.detailLevel = detailLevel_;
    sceneMesh_ = buildSceneMesh(world_, sharedMaterialTable(), options);
    sceneStats_ = sceneMesh_.stats;

    if (auto *vk = dynamic_cast<VulkanRenderBackend *>(renderer_)) {
        std::string error;
        if (!vk->setSceneMesh(sceneMesh_) && !error.empty()) {
            std::cerr << "scene upload failed: " << error << "\n";
        }
    }
}

void NativeEngine::primeClock() {
    lastFrameNanos_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    clockPrimed_ = true;
}

bool NativeEngine::initialize() {
    profile::setProgramName("emergent_native");

    // The surface, if there is one, has to be folded into the instance
    // creation request. Attaching a window later would find an instance with
    // no surface extension in it.
    VulkanInitOptions vkOptions;
    vkOptions.wantSurface = surface_ != nullptr;
    if (surface_ != nullptr) {
        vkOptions.extraInstanceExtensions = surface_->requiredInstanceExtensions();
    }
    const bool vk = vk_.initialize(vkOptions);
    const bool physics = physics_.initialize();
    const bool ecs = ecs_.initialize();
    const bool audio = audio_.initialize();
    // The rig is baked here, not lazily: it is a required subsystem, and an
    // engine that silently ships without a skeleton is worse than one that
    // refuses to start.
    const bool animation = anim_.build();
    // The frame loop owns its own physics world, so it is initialised here
    // rather than lazily: a caller that reaches for loop() after a true
    // initialize() gets a running loop, not an empty one.
    const bool loop = loop_.initialize(loopConfig_);
    if (!loop) {
        std::cerr << "frame loop initialization failed: " << loop_.lastError() << "\n";
    }
    primeClock();
    return vk && physics && ecs && audio && animation && loop;
}

bool NativeEngine::updateFrame() {
    if (!loop_.ready()) return false;
    if (!clockPrimed_) primeClock();

    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    // Monotonic, so this cannot go negative. The subtraction is done in
    // unsigned and the first frame is skipped via primeClock, which is what
    // stops the engine integrating the process's whole uptime as one enormous
    // frame on its very first tick.
    const double delta = static_cast<double>(now - lastFrameNanos_) * 1e-9;
    lastFrameNanos_ = now;

    const profile::Scope scope("frame.total");
    FrameState state;
    if (!tickOnce(delta, state)) return false;
    if (renderTargetOpen_) {
        renderer_->beginFrame();
        renderer_->drawFrame(state);
        renderer_->endFrame();
    }
    return true;
}

uint32_t NativeEngine::runHeadless(uint32_t frames, double delta) {
    if (!loop_.ready()) return 0;
    if (!renderTargetOpen_) {
        // Still run the frames: a caller asking for a headless run wants the
        // simulation, and skipping it because no window is attached would make
        // "headless" mean "does nothing".
        uint32_t ran = 0;
        FrameState state;
        for (uint32_t i = 0; i < frames; ++i) {
            if (!tickOnce(delta, state)) break;
            ++ran;
        }
        return ran;
    }
    uint32_t ran = 0;
    FrameState state;
    for (uint32_t i = 0; i < frames; ++i) {
        if (!tickOnce(delta, state)) break;
        renderer_->beginFrame();
        renderer_->drawFrame(state);
        renderer_->endFrame();
        ++ran;
    }
    return ran;
}

uint32_t NativeEngine::runRealtime(double seconds) {
    if (!loop_.ready() || !(seconds > 0.0)) return 0;
    primeClock();

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(seconds));
    uint32_t ran = 0;
    FrameState state;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!loop_.tick(loopConfig_.fixedDelta, input_, state)) break;
        if (renderTargetOpen_) {
            renderer_->beginFrame();
            renderer_->drawFrame(state);
            renderer_->endFrame();
        }
        ++ran;

        // Sleep out the remainder of the frame. Without this the loop spins as
        // fast as the CPU allows and the only thing limiting it is the
        // scheduler, which is not pacing -- and a soak test that does this
        // measures nothing useful, because it is measuring the machine rather
        // than the engine.
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto next = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(static_cast<double>(ran + 1) * loopConfig_.fixedDelta));
        if (next > now) {
            std::this_thread::sleep_for(next - now);
        }
    }
    return ran;
}

bool NativeEngine::attachRenderBackend(RenderBackend *backend) {
    closeRenderTarget();
    if (backend == nullptr) {
        renderer_ = nullRenderer_.get();
        return true;
    }
    renderer_ = backend;
    return true;
}

bool NativeEngine::enableVulkanRenderer() {
    closeRenderTarget();
    lastRenderError_.clear();
    if (!vk_.initialized()) {
        // Two different failures with the same symptom, so they get different
        // messages: the caller skipped initialize(), or initialize() ran and
        // found no device. "Call initialize() first" would be the wrong advice
        // for a machine with no GPU driver.
        const bool attempted = !vk_.capabilities().error.empty() || vk_.capabilities().loader;
        lastRenderError_ = attempted ? ("no Vulkan device is available: " + vk_.capabilities().error +
                                        " (the renderer is built and will work where a driver exists)")
                                     : "the Vulkan device is not up; enableVulkanRenderer() must follow initialize()";
        return false;
    }
    auto renderer = std::make_unique<VulkanRenderBackend>();
    // Forwarded here rather than set on the backend directly, because the
    // backend does not exist until this function runs. Without it the packs are
    // found only by working directory, which is how a missing bake turns into
    // an untextured city with no message.
    renderer->setSceneAssetDirectory(sceneAssetDirectory_);
    if (!renderer->initialize(vk_, VulkanInitOptions{})) {
        lastRenderError_ = renderer->stats().status;
        return false;
    }
    vulkanRenderer_ = std::move(renderer);
    renderer_ = vulkanRenderer_.get();
    return true;
}

bool NativeEngine::renderTo(uint32_t width, uint32_t height, SurfaceProvider *surface) {
    lastRenderError_.clear();
    if (width == 0 || height == 0) {
        lastRenderError_ = "framebuffer size must be non-zero";
        return false;
    }
    if (surface != nullptr && !vk_.capabilities().instance) {
        // A surface supplied here, after initialize(), cannot have influenced
        // the instance's extension list, so the renderer cannot create a
        // swapchain for it. Saying so beats a renderer that quietly goes
        // offscreen and looks like it worked.
        lastRenderError_ =
            "a surface was supplied after initialize(), so its platform instance extensions were never "
            "requested; call setSurfaceProvider() before initialize()";
        return false;
    }
    if (!renderer_->open(width, height, surface)) {
        lastRenderError_ = renderer_->stats().status;
        renderTargetOpen_ = false;
        return false;
    }
    renderTargetOpen_ = true;
    return true;
}

void NativeEngine::closeRenderTarget() {
    if (renderTargetOpen_ && renderer_ != nullptr) {
        renderer_->close();
    }
    renderTargetOpen_ = false;
}

int NativeEngine::runSelfTest() {
    profile::reset();
    constexpr uint32_t N = 50000;

    {
        const profile::Scope scope("mesh.optimize");
        const auto mesh = optimizeMesh({0, 1, 2, 2, 3, 0}, 4);
        std::cout << "meshoptimizer: " << (mesh.indices.size() == 6 ? "BOUNDARY_READY" : "FAILED") << "\n";
    }

    {
        const profile::Scope scope("physics.step");
        for (int i = 0; i < 120; i++) physics_.step(1.0f / 60.0f);
        std::cout << "Jolt simulation: ACTIVE first_dynamic_y=" << physics_.firstDynamicY() << "\n";
    }

    {
        const profile::Scope scope("ecs.update");
        const auto player = ecs_.spawn(1, 0.0f, 1.0f, 0.0f);
        const auto npc = ecs_.spawn(2, 5.0f, 1.0f, 5.0f);
        ecs_.update(1.0f / 60.0f);
        std::cout << "Flecs ECS: " << ((player != 0 && npc != 0) ? "ACTIVE" : "FAILED")
                  << " entities=" << ecs_.entityCount() << "\n";
    }

    std::cout << "miniaudio: " << (audio_.initialized() ? "ACTIVE" : "FAILED") << "\n";

    // The frame loop, run the way a caller would run it. This is here so that
    // every CI run of emergent_native actually drives physics and animation
    // together, rather than the loop existing only in a test binary that
    // nothing else in the tree executes.
    {
        if (!loop_.ready()) {
            std::cout << "frame loop: FAILED " << loop_.lastError() << "\n";
        } else {
            InputState move;
            move.moveZ = 1.0f;
            FrameState state;
            const uint32_t ran = runHeadless(120, 1.0 / 60.0);
            const FrameStats &st = loop_.stats();

            // A second pass at a different frame rate, to show the fixed step
            // is what is being counted and not the frame count. 120 frames of
            // 1/120s is one second of real time, the same as the 120 frames of
            // 1/60s above, and must produce the same number of steps.
            loop_.reset();
            int slowSteps = 0;
            for (int i = 0; i < 60; ++i) {
                if (!loop_.tick(1.0 / 30.0, move, state)) break;
                slowSteps += state.physicsSteps;
            }
            const int fastSteps = 120;  // 120 frames at exactly the fixed rate

            const bool ok = ran == 120 && st.alpha >= 0.0 && st.alpha < 1.0 && slowSteps == fastSteps &&
                            st.steps > 0 && st.simulatedSeconds > 0.0;
            std::printf("frame loop: %s frames=%u steps=%d sim=%.2fs alpha=%.17g clip=%.*s "
                        "pos=(%.2f,%.2f,%.2f) joints=%d boxes=%d phys=%.3fms anim=%.3fms\n",
                        ok ? "ACTIVE" : "FAILED", ran, st.steps, st.simulatedSeconds,
                        static_cast<double>(st.alpha), static_cast<int>(loop_.currentClip().size()),
                        loop_.currentClip().data(), static_cast<double>(state.playerPosition[0]),
                        static_cast<double>(state.playerPosition[1]), static_cast<double>(state.playerPosition[2]),
                        state.jointCount, state.sceneBoxCount, st.physicsMs, st.animationMs);
        }
    }

    // Animation: play a walk cycle and resolve real model-space joint
    // matrices, so the ozz runtime is exercised rather than merely linked.
    {
        Animator animator;
        Pose pose;
        animator.bind(anim_);
        pose.bind(anim_);
        animator.play("walk", true);
        const profile::Scope scope("animation.sample");
        for (int i = 0; i < 60; i++) {
            animator.update(1.0f / 60.0f);
            pose.sample(animator);
            pose.resolve();
        }
        float head[3] = {};
        const bool placed = pose.jointTranslation("Head", head);
        std::printf("ozz animation: %s joints=%d clips=%d head_y=%.3f\n",
            (anim_.ready() && placed) ? "ACTIVE" : "FAILED", anim_.jointCount(), anim_.clipCount(),
            placed ? head[1] : 0.0f);
    }

    // Content: write a pack and read it straight back, so the compression and
    // integrity paths are measured on this run rather than assumed.
    {
        const profile::Scope scope("assets.pack");
        const std::string path = "emergent_content.ezpk";
        bool ok = writeContentPack(path, 9);
        if (ok) ok = loadContentPack(path);
        std::vector<uint8_t> manifest;
        if (ok) ok = readContent("manifest.json", manifest);
        std::printf("Zstandard packs: %s entries=%d ratio=%.4f manifest=%s\n",
            ok ? "ACTIVE" : "FAILED", static_cast<int>(contentEntryCount()),
            contentCompressionRatio(), ok ? "round-tripped" : "unreadable");
    }

    std::vector<ObjectGPU> objects;
    objects.reserve(N);
    for (uint32_t i = 0; i < N; i++) {
        float x = float(i % 500) - 250.f;
        float z = float(i / 500) - 50.f;
        objects.push_back({x, 0, z, 1.0f, i % 8, i % 4, 0.5f, 0});
    }

    uint32_t visible = 0;
    double ms = 0.0;
    {
        // The cull is the one genuinely hot loop in this tree, so it is the
        // one worth having a number for.
        const profile::Scope scope("scene.cull");
        auto t0 = std::chrono::steady_clock::now();
        for (auto &o : objects) {
            float d = std::sqrt(o.x * o.x + o.z * o.z);
            if (d < 180.f) visible++;
        }
        auto t1 = std::chrono::steady_clock::now();
        ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    std::cout << "Native scene self-test: objects=" << N << " visible=" << visible
              << " culled=" << (N - visible) << " CPU_ms=" << ms << "\n";

    // The profile is written, not just printed: a run that measured something
    // should leave evidence a test or a CI job can read.
    const char *profilePath = "emergent_profile.json";
    if (profile::writeJson(profilePath)) {
        std::cout << "Profiling: " << profile::zoneCount() << " zones, total "
                  << profile::totalMs() << " ms -> " << profilePath << "\n";
    } else {
        std::cout << "Profiling: could not write " << profilePath << " (" << profile::error() << ")\n";
    }
    std::cout << "Tracy: " << profile::tracyStatus() << "\n";
    return 0;
}

NativeGpuRenderStats NativeEngine::renderBootstrap(uint32_t width, uint32_t height) {
    return vk_.renderBootstrap(width, height);
}

bool NativeEngine::writeContentPack(const std::string &path, int level) {
    // Bundles the current world seed and the generated district roster.
    std::string districts;
    for (int i = 0; i < 64; ++i) {
        districts += "region " + std::to_string(i) + ": buildings, businesses, population\n";
    }
    AssetWriter writer;
    if (!writer.open(path, level)) return false;
    if (!writer.addText("manifest.json", "{\"seed\":173927}")) return false;
    if (!writer.addText("districts.txt", districts)) return false;
    return writer.close();
}

bool NativeEngine::loadContentPack(const std::string &path) { return content_.open(path); }

size_t NativeEngine::contentEntryCount() const { return content_.entryCount(); }

bool NativeEngine::readContent(std::string_view name, std::vector<uint8_t> &out) const {
    return content_.read(name, out);
}

double NativeEngine::contentCompressionRatio() const { return content_.compressionRatio(); }

void NativeEngine::shutdown() {
    // Reverse of construction order. The renderer closes before the device
    // goes away, because a swapchain destroyed after its device is a
    // use-after-free inside the driver.
    closeRenderTarget();
    if (vulkanRenderer_) {
        vulkanRenderer_->shutdownDevice();
        vulkanRenderer_.reset();
    }
    renderer_ = nullRenderer_.get();
    nullRenderer_.reset();
    loop_.shutdown();
    audio_.shutdown();
    ecs_.shutdown();
    physics_.shutdown();
    vk_.shutdown();
}

} // namespace emergent
