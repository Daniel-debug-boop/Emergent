#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace emergent {
struct NavigationAgentState {
    bool active = false;
    float position[3]{};
    float velocity[3]{};
};

class DetourNavigationWorld {
public:
    DetourNavigationWorld();
    ~DetourNavigationWorld();
    DetourNavigationWorld(const DetourNavigationWorld&) = delete;
    DetourNavigationWorld& operator=(const DetourNavigationWorld&) = delete;

    bool available() const noexcept;
    bool loadNavMesh(const std::vector<std::uint8_t>& data);
    int addAgent(const float position[3], float radius, float height, float maxSpeed);
    bool setTarget(int agentId, const float target[3]);
    void update(float dt);
    bool getAgentState(int agentId, NavigationAgentState& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
