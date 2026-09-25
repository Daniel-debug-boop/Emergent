#include "emergent/navigation.hpp"

#include <algorithm>
#include <cstring>

#include <DetourAlloc.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

namespace emergent {
namespace {
constexpr int kMaxAgentSlots = 2048;
constexpr float kMaxAgentRadius = 0.6f;
} // namespace

struct DetourNavigationWorld::Impl {
    dtNavMesh* nav_mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;

    ~Impl() {
        if (crowd) dtFreeCrowd(crowd);
        if (query) dtFreeNavMeshQuery(query);
        if (nav_mesh) dtFreeNavMesh(nav_mesh);
    }
};

DetourNavigationWorld::DetourNavigationWorld() : impl_(std::make_unique<Impl>()) {}
DetourNavigationWorld::~DetourNavigationWorld() = default;

bool DetourNavigationWorld::available() const noexcept { return true; }

/**
 * Adopt a serialized Recast navigation mesh tile set and stand up a query and
 * a crowd against it. The world owns the query because Recast 1.6 only exposes
 * a const query from dtCrowd, and projecting a requested destination onto the
 * mesh needs a mutable one.
 */
bool DetourNavigationWorld::loadNavMesh(const std::vector<std::uint8_t>& data) {
    if (data.empty() || data.size() < sizeof(dtMeshHeader)) return false;

    auto* mesh = dtAllocNavMesh();
    if (!mesh) return false;

    const auto* header = reinterpret_cast<const dtMeshHeader*>(data.data());
    dtNavMeshParams params{};
    std::copy(header->bmin, header->bmin + 3, params.orig);
    params.tileWidth = header->bmax[0] - header->bmin[0];
    params.tileHeight = header->bmax[2] - header->bmin[2];
    params.maxTiles = 1;
    params.maxPolys = header->polyCount;
    if (dtStatusFailed(mesh->init(&params))) {
        dtFreeNavMesh(mesh);
        return false;
    }

    // DT_TILE_FREE_DATA makes the mesh take ownership of the buffer below.
    const int size = static_cast<int>(data.size());
    auto* owned = static_cast<unsigned char*>(dtAlloc(size, DT_ALLOC_PERM));
    if (!owned) {
        dtFreeNavMesh(mesh);
        return false;
    }
    std::copy(data.begin(), data.end(), owned);

    dtTileRef ref = 0;
    if (dtStatusFailed(mesh->addTile(owned, size, DT_TILE_FREE_DATA, 0, &ref))) {
        dtFree(owned);
        dtFreeNavMesh(mesh);
        return false;
    }

    auto* query = dtAllocNavMeshQuery();
    auto* crowd = dtAllocCrowd();
    if (!query || !crowd ||
        dtStatusFailed(query->init(mesh, kMaxAgentSlots)) ||
        !crowd->init(kMaxAgentSlots, kMaxAgentRadius, mesh)) {
        if (crowd) dtFreeCrowd(crowd);
        if (query) dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
        return false;
    }

    // Only adopt the new set once it is fully built, so a failed load leaves
    // the previously loaded world intact rather than half-destroyed.
    impl_ = std::make_unique<Impl>();
    impl_->nav_mesh = mesh;
    impl_->query = query;
    impl_->crowd = crowd;
    return true;
}

int DetourNavigationWorld::addAgent(const float position[3], float radius, float height, float maxSpeed) {
    if (!impl_->crowd) return -1;
    dtCrowdAgentParams params{};
    params.radius = radius;
    params.height = height;
    params.maxAcceleration = 8.0f;
    params.maxSpeed = maxSpeed;
    params.collisionQueryRange = radius * 8.0f;
    params.pathOptimizationRange = radius * 30.0f;
    params.separationWeight = 2.0f;
    params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE |
                         DT_CROWD_SEPARATION | DT_CROWD_OPTIMIZE_VIS;
    return impl_->crowd->addAgent(position, &params);
}

bool DetourNavigationWorld::setTarget(int agentId, const float target[3]) {
    if (!impl_->crowd || !impl_->query || agentId < 0) return false;
    const auto* agent = impl_->crowd->getAgent(agentId);
    if (!agent || !agent->active) return false;

    dtPolyRef ref = 0;
    float nearest[3]{};
    const float extents[3]{2.0f, 4.0f, 2.0f};
    dtQueryFilter filter;
    if (dtStatusFailed(impl_->query->findNearestPoly(target, extents, &filter, &ref, nearest)) || !ref) {
        return false;
    }
    return impl_->crowd->requestMoveTarget(agentId, ref, nearest);
}

void DetourNavigationWorld::update(float dt) {
    if (impl_->crowd && dt > 0.0f) impl_->crowd->update(dt, nullptr);
}

bool DetourNavigationWorld::getAgentState(int agentId, NavigationAgentState& out) const {
    if (!impl_->crowd || agentId < 0) return false;
    const auto* agent = impl_->crowd->getAgent(agentId);
    if (!agent || !agent->active) return false;
    out.active = true;
    std::copy(agent->npos, agent->npos + 3, out.position);
    std::copy(agent->vel, agent->vel + 3, out.velocity);
    return true;
}

} // namespace emergent
