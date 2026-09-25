#include "emergent/audio.hpp"
#include <miniaudio.h>
#include <memory>

namespace emergent {
struct AudioSystem::Impl {
    ma_engine engine{};
    bool initialized = false;
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {}
AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::initialize() {
    if (impl_->initialized) return true;
    if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) return false;
    impl_->initialized = true;
    return true;
}

void AudioSystem::shutdown() noexcept {
    if (!impl_ || !impl_->initialized) return;
    ma_engine_uninit(&impl_->engine);
    impl_->initialized = false;
}

bool AudioSystem::play(const std::string& path) noexcept {
    return impl_->initialized && ma_engine_play_sound(&impl_->engine, path.c_str(), nullptr) == MA_SUCCESS;
}

bool AudioSystem::initialized() const noexcept { return impl_ && impl_->initialized; }
}
