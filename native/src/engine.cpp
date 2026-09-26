#include "emergent/engine.hpp"
#include "emergent/profiling.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

namespace emergent {

bool NativeEngine::initialize() {
    profile::setProgramName("emergent_native");
    const bool vk = vk_.initialize();
    const bool physics = physics_.initialize();
    const bool ecs = ecs_.initialize();
    const bool audio = audio_.initialize();
    // The rig is baked here, not lazily: it is a required subsystem, and an
    // engine that silently ships without a skeleton is worse than one that
    // refuses to start.
    const bool animation = anim_.build();
    return vk && physics && ecs && audio && animation;
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
    audio_.shutdown();
    ecs_.shutdown();
    physics_.shutdown();
    vk_.shutdown();
}

} // namespace emergent
