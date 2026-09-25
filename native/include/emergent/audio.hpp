#pragma once
#include <memory>
#include <string>

namespace emergent {
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    bool initialize();
    void shutdown() noexcept;
    bool play(const std::string& path) noexcept;
    bool initialized() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
