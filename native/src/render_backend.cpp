#include "emergent/render_backend.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace emergent {
namespace {

// A frame is usable if the numbers in it are numbers. A NaN that reaches a
// vertex buffer does not render wrong, it takes the driver down, and the
// resulting report points at the renderer rather than at the loop that
// produced the NaN. Checking here means the failure is attributed correctly
// and happens on a machine with no GPU.
bool finite3(const float *v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool finite16(const float *v, int count) {
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

bool validate(const FrameState &state, std::string &why) {
    if (!std::isfinite(state.alpha) || state.alpha < 0.0 || state.alpha >= 1.0) {
        why = "alpha outside [0,1)";
        return false;
    }
    if (!std::isfinite(state.realDelta) || state.realDelta < 0.0) {
        why = "negative or non-finite real delta";
        return false;
    }
    if (state.physicsSteps < 0 || state.physicsSteps > 1024) {
        why = "implausible physics step count";
        return false;
    }
    if (!finite3(state.playerPosition) || !finite3(state.playerPrevious) || !finite3(state.playerNext) ||
        !finite3(state.playerVelocity) || !finite3(state.cameraTarget)) {
        why = "non-finite transform in frame state";
        return false;
    }
    if (state.jointMatrices == nullptr && state.jointCount != 0) {
        why = "joint count without joint matrices";
        return false;
    }
    if (state.jointCount < 0) {
        why = "negative joint count";
        return false;
    }
    if (state.jointMatrices != nullptr &&
        !finite16(state.jointMatrices, state.jointCount * 16)) {
        why = "non-finite joint matrix";
        return false;
    }
    if (state.sceneBoxes == nullptr && state.sceneBoxCount != 0) {
        why = "scene box count without scene boxes";
        return false;
    }
    for (int i = 0; i < state.sceneBoxCount; ++i) {
        const SceneBox &b = state.sceneBoxes[i];
        if (!finite3(b.center) || !finite3(b.halfExtent)) {
            why = "non-finite scene box";
            return false;
        }
    }
    return true;
}

} // namespace

NullRenderBackend::NullRenderBackend() = default;
NullRenderBackend::~NullRenderBackend() = default;

bool NullRenderBackend::open(uint32_t width, uint32_t height, SurfaceProvider *surface) {
    if (width == 0 || height == 0) {
        stats_.status = "null backend needs a non-zero framebuffer size";
        opened_ = false;
        return false;
    }
    stats_ = RenderFrameStats{};
    stats_.width = width;
    stats_.height = height;
    stats_.hasDevice = false;
    stats_.hasSwapchain = false;
    // The point of saying this out loud: a null backend attached to a real
    // window still does not put anything on it, and any tool reading this
    // needs to be able to tell the difference between "rendered offscreen" and
    // "rendered nothing".
    stats_.status = surface != nullptr
                        ? "headless: no window will be presented to"
                        : "headless: offscreen, nothing is presented";
    opened_ = true;
    return true;
}

void NullRenderBackend::close() noexcept {
    opened_ = false;
    inFrame_ = false;
    hasLast_ = false;
}

void NullRenderBackend::beginFrame() {
    inFrame_ = true;
    frameAccepted_ = false;
}

void NullRenderBackend::drawFrame(const FrameState &state) {
    if (!inFrame_) {
        ++stats_.skippedFrames;
        stats_.status = "drawFrame() outside beginFrame()/endFrame()";
        return;
    }
    std::string why;
    if (!validate(state, why)) {
        ++stats_.skippedFrames;
        stats_.status = "rejected frame: " + why;
        return;
    }
    // Copied rather than referenced: the loop's own storage is only valid
    // until its next tick, and anything inspecting this later needs the frame
    // that was actually validated, not a pointer into a buffer that has since
    // moved on.
    last_ = state;
    hasLast_ = true;
    frameAccepted_ = true;
}

void NullRenderBackend::endFrame() {
    if (!inFrame_) return;
    inFrame_ = false;
    // Only a frame that was actually drawn counts. Counting every endFrame()
    // would inflate the number whenever drawFrame() refused something, and a
    // frame counter that over-reports is worse than no counter: it is the
    // number people believe.
    if (!frameAccepted_) {
        stats_.status = "headless: frame rejected by validation, not accounted for";
        return;
    }
    ++stats_.submittedFrames;
    // "Submitted" here means accepted and accounted for, not put on a screen.
    // presentsToDisplay() is the flag that distinguishes the two, and it is
    // false by construction.
    stats_.status = "headless: frame accounted for, not presented";
}

bool NullRenderBackend::lastFrame(FrameState &out) const {
    if (!hasLast_) return false;
    out = last_;
    return true;
}

bool loadSpirvModule(const std::string &path, std::vector<uint32_t> &out, std::string &error) {
    error.clear();
    out.clear();

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open SPIR-V module: " + path;
        return false;
    }
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        error = "cannot seek SPIR-V module: " + path;
        return false;
    }
    const long size = std::ftell(f);
    std::rewind(f);
    if (size <= 0) {
        std::fclose(f);
        error = "SPIR-V module is empty: " + path;
        return false;
    }
    // A SPIR-V module is a sequence of 32-bit words. A file whose length is
    // not a multiple of four is truncated or corrupt, and handing that to
    // vkCreateShaderModule is undefined behaviour inside the driver. This is
    // the single check that turns a bad asset into an error message.
    if ((size % 4) != 0) {
        std::fclose(f);
        error = "SPIR-V module size is not a multiple of 4 bytes: " + path;
        return false;
    }

    out.resize(static_cast<size_t>(size) / 4u);
    const size_t read = std::fread(out.data(), 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (read != out.size() * 4u) {
        out.clear();
        error = "short read on SPIR-V module: " + path;
        return false;
    }
    if (out[0] != kSpirvMagic) {
        out.clear();
        error = "SPIR-V magic number missing (is this a GLSL source rather than a compiled module?): " + path;
        return false;
    }
    return true;
}

} // namespace emergent
