#include "emergent/navigation.hpp"
#include <algorithm>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourAlloc.h>
namespace emergent {
struct DetourNavigationWorld::Impl { dtNavMesh* nav_mesh=nullptr; dtNavMeshQuery* query=nullptr; dtCrowd* crowd=nullptr; };
DetourNavigationWorld::DetourNavigationWorld():impl_(std::make_unique<Impl>()){}
DetourNavigationWorld::~DetourNavigationWorld(){ if(impl_->crowd)dtFreeCrowd(impl_->crowd); if(impl_->query)dtFreeNavMeshQuery(impl_->query); if(impl_->nav_mesh)dtFreeNavMesh(impl_->nav_mesh); }
bool DetourNavigationWorld::available() const noexcept { return true; }
bool DetourNavigationWorld::loadNavMesh(const std::vector<std::uint8_t>& data){
    if(data.empty()||data.size()<sizeof(dtMeshHeader)) return false;
    auto* mesh=dtAllocNavMesh(); if(!mesh)return false;
    const auto* header=reinterpret_cast<const dtMeshHeader*>(data.data()); dtNavMeshParams params{};
    std::copy(header->bmin,header->bmin+3,params.orig); params.tileWidth=header->bmax[0]-header->bmin[0]; params.tileHeight=header->bmax[2]-header->bmin[2]; params.maxTiles=1; params.maxPolys=header->polyCount;
    if(dtStatusFailed(mesh->init(&params))){dtFreeNavMesh(mesh);return false;}
    const int size=static_cast<int>(data.size()); auto* owned=static_cast<unsigned char*>(dtAlloc(size,DT_ALLOC_PERM)); if(!owned){dtFreeNavMesh(mesh);return false;}
    std::copy(data.begin(),data.end(),owned); dtTileRef ref=0;
    if(dtStatusFailed(mesh->addTile(owned,size,DT_TILE_FREE_DATA,0,&ref))){dtFree(owned);dtFreeNavMesh(mesh);return false;}
    auto* query=dtAllocNavMeshQuery(); auto* crowd=dtAllocCrowd();
    if(!query||!crowd||dtStatusFailed(query->init(mesh,2048))||!crowd->init(2048,0.6f,mesh)){if(crowd)dtFreeCrowd(crowd);if(query)dtFreeNavMeshQuery(query);dtFreeNavMesh(mesh);return false;}
    if(impl_->crowd)dtFreeCrowd(impl_->crowd); if(impl_->query)dtFreeNavMeshQuery(impl_->query); if(impl_->nav_mesh)dtFreeNavMesh(impl_->nav_mesh);
    impl_->nav_mesh=mesh;impl_->query=query;impl_->crowd=crowd;return true;
}
int DetourNavigationWorld::addAgent(const float position[3],float radius,float height,float maxSpeed){
    if(!impl_->crowd)return -1; dtCrowdAgentParams p{}; p.radius=radius;p.height=height;p.maxAcceleration=8.0f;p.maxSpeed=maxSpeed;p.collisionQueryRange=radius*8.0f;p.pathOptimizationRange=radius*30.0f;p.separationWeight=2.0f;p.updateFlags=DT_CROWD_ANTICIPATE_TURNS|DT_CROWD_OBSTACLE_AVOIDANCE|DT_CROWD_SEPARATION|DT_CROWD_OPTIMIZE_VIS; return impl_->crowd->addAgent(position,&p);
}
bool DetourNavigationWorld::setTarget(int agentId,const float target[3]){
    if(!impl_->crowd||agentId<0)return false; const auto* agent=impl_->crowd->getAgent(agentId); if(!agent||!agent->active)return false; dtPolyRef ref=0;float nearest[3]{};const float extents[3]{2,4,2};dtQueryFilter filter; if(dtStatusFailed(impl_->crowd->getEditableQuery()->findNearestPoly(target,extents,&filter,&ref,nearest))||!ref)return false; return impl_->crowd->requestMoveTarget(agentId,ref,nearest);
}
void DetourNavigationWorld::update(float dt){if(impl_->crowd)impl_->crowd->update(dt,nullptr);}
bool DetourNavigationWorld::getAgentState(int agentId,NavigationAgentState& out) const{if(!impl_->crowd||agentId<0)return false;const auto* a=impl_->crowd->getAgent(agentId);if(!a||!a->active)return false;out.active=true;std::copy(a->npos,a->npos+3,out.position);std::copy(a->vel,a->vel+3,out.velocity);return true;}
}
