#include "emergent/jolt_physics.hpp"
#include <memory>
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>

namespace {
using namespace JPH;
static constexpr ObjectLayer LAYER_STATIC = 0;
static constexpr ObjectLayer LAYER_DYNAMIC = 1;
static constexpr BroadPhaseLayer BP_STATIC(0);
static constexpr BroadPhaseLayer BP_DYNAMIC(1);
class ObjectLayerPairFilter final : public ObjectLayerPairFilterTable {
public: ObjectLayerPairFilter() : ObjectLayerPairFilterTable(2) { EnableCollision(LAYER_STATIC,LAYER_DYNAMIC); EnableCollision(LAYER_DYNAMIC,LAYER_DYNAMIC); }
};
class ObjectVsBroadPhase final : public ObjectVsBroadPhaseLayerFilterTable {
public: ObjectVsBroadPhase() : ObjectVsBroadPhaseLayerFilterTable(2,2) { EnableCollision(LAYER_STATIC,BP_DYNAMIC); EnableCollision(LAYER_DYNAMIC,BP_STATIC); EnableCollision(LAYER_DYNAMIC,BP_DYNAMIC); }
};
class BroadPhase final : public BroadPhaseLayerInterfaceTable {
public: BroadPhase() : BroadPhaseLayerInterfaceTable(2) { MapObjectToBroadPhaseLayer(LAYER_STATIC,BP_STATIC); MapObjectToBroadPhaseLayer(LAYER_DYNAMIC,BP_DYNAMIC); }
};
}
namespace emergent {
struct JoltPhysicsWorld::Impl {
    BroadPhase broad_phase;
    ObjectVsBroadPhase object_vs_bp;
    ObjectLayerPairFilter object_pair;
    JPH::PhysicsSystem physics;
    std::unique_ptr<JPH::TempAllocatorImpl> allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::BodyID dynamic_body;
    bool initialized = false;
};
JoltPhysicsWorld::JoltPhysicsWorld() : impl_(new Impl) {}
JoltPhysicsWorld::~JoltPhysicsWorld(){ shutdown(); delete impl_; }
bool JoltPhysicsWorld::initialize(){
    if(impl_->initialized) return true;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    impl_->physics.Init(10240,0,1024,1024,impl_->broad_phase,impl_->object_vs_bp,impl_->object_pair);
    impl_->allocator=std::make_unique<JPH::TempAllocatorImpl>(10*1024*1024);
    impl_->jobs=std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,JPH::cMaxPhysicsBarriers,-1);
    impl_->initialized=true;
    createBox(0,-0.5f,0,50.0f,false);
    createBox(0,4.0f,0,0.5f,true);
    return true;
}
void JoltPhysicsWorld::shutdown(){
    if(!impl_||!impl_->initialized) return;
    impl_->jobs.reset(); impl_->allocator.reset();
    JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance=nullptr;
    impl_->initialized=false;
}
void JoltPhysicsWorld::step(float dt){ if(impl_->initialized) impl_->physics.Update(dt,1,impl_->allocator.get(),impl_->jobs.get()); }
bool JoltPhysicsWorld::createBox(float x,float y,float z,float halfExtent,bool dynamic){
    if(!impl_->initialized) return false;
    JPH::BoxShapeSettings shape(JPH::Vec3(halfExtent,halfExtent,halfExtent));
    auto result=shape.Create(); if(result.HasError()) return false;
    JPH::BodyCreationSettings settings(result.Get(),JPH::RVec3(x,y,z),JPH::Quat::sIdentity(),dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,dynamic?LAYER_DYNAMIC:LAYER_STATIC);
    JPH::Body* body=impl_->physics.GetBodyInterface().CreateBody(settings); if(!body) return false;
    auto id=body->GetID(); impl_->physics.GetBodyInterface().AddBody(id,dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
    if(dynamic&&!impl_->dynamic_body.IsValid()) impl_->dynamic_body=id; return true;
}
float JoltPhysicsWorld::firstDynamicY() const { if(!impl_->initialized||!impl_->dynamic_body.IsValid()) return 0.0f; return impl_->physics.GetBodyInterface().GetPosition(impl_->dynamic_body).GetY(); }
bool JoltPhysicsWorld::available() const { return impl_&&impl_->initialized; }
}
