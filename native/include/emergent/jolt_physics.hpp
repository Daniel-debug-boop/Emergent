#pragma once
#include <cstdint>

namespace emergent {
class JoltPhysicsWorld {
public:
    JoltPhysicsWorld();
    ~JoltPhysicsWorld();
    bool initialize();
    void shutdown();
    void step(float dt);
    bool createBox(float x, float y, float z, float halfExtent, bool dynamic);
    float firstDynamicY() const;
    bool available() const;
private:
    struct Impl;
    Impl* impl_ = nullptr;
};
}
