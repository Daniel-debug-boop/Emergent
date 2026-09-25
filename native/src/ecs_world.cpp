#include "emergent/ecs_world.hpp"
#include <flecs.h>
#include <utility>

namespace emergent {
struct EcsWorld::Impl {
    flecs::world world;
    flecs::entity transform;
    flecs::entity runtime_tag;
    bool initialized = false;
};

EcsWorld::EcsWorld() : impl_(std::make_unique<Impl>()) {}
EcsWorld::~EcsWorld() { shutdown(); }

bool EcsWorld::initialize() {
    if (impl_->initialized) return true;
    impl_->world.component<TransformComponent>();
    impl_->world.component<RuntimeTagComponent>();
    impl_->transform = impl_->world.entity("TransformComponent");
    impl_->runtime_tag = impl_->world.entity("RuntimeTagComponent");
    impl_->initialized = true;
    return true;
}

void EcsWorld::shutdown() noexcept {
    if (!impl_) return;
    if (impl_->initialized) {
        impl_ = std::make_unique<Impl>();
    }
}

std::uint32_t EcsWorld::spawn(std::uint32_t kind, float x, float y, float z) {
    if (!impl_->initialized) return 0;
    const auto entity = impl_->world.entity()
        .set<TransformComponent>({{x, y, z}, {0.0f, 0.0f, 0.0f, 1.0f}})
        .set<RuntimeTagComponent>({kind});
    return static_cast<std::uint32_t>(entity.id());
}

std::uint32_t EcsWorld::entityCount() const noexcept {
    if (!impl_->initialized) return 0;
    return static_cast<std::uint32_t>(impl_->world.count<TransformComponent>());
}

void EcsWorld::update(float dt) noexcept {
    if (!impl_->initialized || dt <= 0.0f) return;
    impl_->world.progress(dt);
}
}
