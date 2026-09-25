#pragma once
#include <cstdint>
#include <memory>

namespace emergent {
struct TransformComponent { float position[3]{}; float rotation[4]{0,0,0,1}; };
struct RuntimeTagComponent { std::uint32_t kind = 0; };

class EcsWorld {
public:
    EcsWorld();
    ~EcsWorld();
    EcsWorld(const EcsWorld&) = delete;
    EcsWorld& operator=(const EcsWorld&) = delete;
    bool initialize();
    void shutdown() noexcept;
    std::uint32_t spawn(std::uint32_t kind, float x, float y, float z);
    std::uint32_t entityCount() const noexcept;
    void update(float dt) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
