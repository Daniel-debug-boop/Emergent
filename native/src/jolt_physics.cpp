#include "emergent/jolt_physics.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>

#include <memory>

namespace {
using namespace JPH;

// Two object layers (static geometry, simulated bodies) mapped onto two broad
// phase layers. Broad phase culling runs on the broad phase layer, so keeping
// this split coarse is what makes the broad phase cheap.
constexpr ObjectLayer kLayerStatic = 0;
constexpr ObjectLayer kLayerDynamic = 1;
constexpr BroadPhaseLayer kBroadPhaseStatic(0);
constexpr BroadPhaseLayer kBroadPhaseDynamic(1);
constexpr uint kNumObjectLayers = 2;
constexpr uint kNumBroadPhaseLayers = 2;

class StaticVsDynamic final : public ObjectLayerPairFilterTable {
public:
    StaticVsDynamic() : ObjectLayerPairFilterTable(kNumObjectLayers) {
        EnableCollision(kLayerStatic, kLayerDynamic);
        EnableCollision(kLayerDynamic, kLayerStatic);
        EnableCollision(kLayerDynamic, kLayerDynamic);
    }
};

class BroadPhaseInterface final : public BroadPhaseLayerInterfaceTable {
public:
    BroadPhaseInterface() : BroadPhaseLayerInterfaceTable(kNumObjectLayers, kNumBroadPhaseLayers) {
        MapObjectToBroadPhaseLayer(kLayerStatic, kBroadPhaseStatic);
        MapObjectToBroadPhaseLayer(kLayerDynamic, kBroadPhaseDynamic);
    }
};

// Derives its table from the object layer pair filter and the broad phase
// interface, so it must be constructed after both of them and needs no
// manual EnableCollision calls of its own.
class ObjectVsBroadPhase final : public ObjectVsBroadPhaseLayerFilterTable {
public:
    ObjectVsBroadPhase(BroadPhaseLayerInterfaceTable &inInterface, ObjectLayerPairFilter &inPairFilter)
        : ObjectVsBroadPhaseLayerFilterTable(inInterface, kNumBroadPhaseLayers, inPairFilter, kNumObjectLayers) {}
};

// Jolt's global factory and type registry are process-wide. Two live worlds
// would both try to install them, so registration is reference counted and the
// teardown only happens when the last world goes away.
int &worldRefCount() {
    static int count = 0;
    return count;
}

} // namespace

namespace emergent {

// Every Jolt object is held by pointer and created inside initialize().
//
// This is not stylistic. Jolt's type registry must be populated by
// RegisterTypes() *before* any Jolt object is constructed; a JPH::PhysicsSystem
// built earlier dereferences a null Factory and crashes. Keeping construction
// inside initialize() also means shutdown() is the single place that has to
// destroy them, in the right order.
struct JoltPhysicsWorld::Impl {
    std::unique_ptr<BroadPhaseInterface> broad_phase;
    std::unique_ptr<StaticVsDynamic> object_pair;
    std::unique_ptr<ObjectVsBroadPhase> object_vs_bp;
    std::unique_ptr<JPH::PhysicsSystem> physics;
    std::unique_ptr<JPH::TempAllocatorImpl> allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::BodyID first_dynamic;
    bool initialized = false;
};

JoltPhysicsWorld::JoltPhysicsWorld() : impl_(std::make_unique<Impl>()) {}

JoltPhysicsWorld::~JoltPhysicsWorld() { shutdown(); }

bool JoltPhysicsWorld::initialize() {
    if (impl_->initialized) return true;
    if (worldRefCount() == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ++worldRefCount();

    // Filter order matters: the broad phase interface and the pair filter must
    // both exist before the object-vs-broadphase table derives from them.
    impl_->broad_phase = std::make_unique<BroadPhaseInterface>();
    impl_->object_pair = std::make_unique<StaticVsDynamic>();
    impl_->object_vs_bp = std::make_unique<ObjectVsBroadPhase>(*impl_->broad_phase, *impl_->object_pair);
    impl_->physics = std::make_unique<JPH::PhysicsSystem>();

    // 10 MiB of temp scratch: enough for the contact solving this game needs
    // without pretending we know the real budget up front.
    impl_->allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    // -1 threads means "derive from the hardware"; Jolt requires at least one
    // worker beyond the calling thread to drain its job queue.
    impl_->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, -1);

    impl_->physics->Init(kMaxBodies, 0, 1024, 1024, *impl_->broad_phase, *impl_->object_vs_bp, *impl_->object_pair);
    impl_->first_dynamic = JPH::BodyID();
    impl_->initialized = true;

    // Ground plane plus a body resting above it, so a fresh world is already
    // meaningful instead of empty. The ground is a thin slab with its top face
    // at y = 0, not a cube: a 50-unit cube would enclose the falling body and
    // eject it instead of catching it.
    createBoxExtents(0.0f, -0.5f, 0.0f, 50.0f, 0.5f, 50.0f, false);
    createBox(0.0f, 4.0f, 0.0f, 0.5f, true);
    return true;
}

void JoltPhysicsWorld::shutdown() {
    if (!impl_->initialized) return;
    impl_->initialized = false;

    // Destroy the job system before the allocator: in-flight jobs would
    // otherwise be handed a freed TempAllocator. The physics system itself
    // must also go before the factory, since it holds registered shape types.
    impl_->jobs.reset();
    impl_->allocator.reset();
    impl_->first_dynamic = JPH::BodyID();
    impl_->physics.reset();
    impl_->object_vs_bp.reset();
    impl_->object_pair.reset();
    impl_->broad_phase.reset();

    if (--worldRefCount() == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

bool JoltPhysicsWorld::available() const { return impl_ && impl_->initialized; }

void JoltPhysicsWorld::step(float dt) {
    if (!impl_->initialized || dt <= 0.0f) return;
    // Collision steps are clamped to 1 so a long frame cannot explode the
    // solver; callers wanting more accuracy should step more often.
    impl_->physics->Update(dt, 1, impl_->allocator.get(), impl_->jobs.get());
}

bool JoltPhysicsWorld::createBox(float x, float y, float z, float halfExtent, bool dynamic) {
    return createBoxExtents(x, y, z, halfExtent, halfExtent, halfExtent, dynamic);
}

bool JoltPhysicsWorld::createBoxExtents(float x, float y, float z, float hx, float hy, float hz, bool dynamic) {
    if (!impl_->initialized) return false;
    // A zero or negative extent produces a degenerate shape that Jolt may
    // accept and then behave unpredictably, so reject it up front.
    if (!(hx > 0.0f) || !(hy > 0.0f) || !(hz > 0.0f)) return false;

    JPH::BoxShapeSettings shape_settings(JPH::Vec3(hx, hy, hz));
    JPH::ShapeSettings::ShapeResult shape = shape_settings.Create();
    if (shape.HasError()) return false;

    JPH::BodyCreationSettings settings(
        shape.Get(),
        JPH::RVec3(x, y, z),
        JPH::Quat::sIdentity(),
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
        dynamic ? kLayerDynamic : kLayerStatic
    );
    // Damping keeps resting bodies from creeping indefinitely; a game with no
    // sleeping enabled would otherwise pay for jitter forever.
    if (dynamic) {
        settings.mLinearDamping = 0.05f;
        settings.mAngularDamping = 0.05f;
        settings.mRestitution = 0.0f;
    }

    JPH::BodyInterface &bodies = impl_->physics->GetBodyInterface();
    JPH::Body *body = bodies.CreateBody(settings);
    if (body == nullptr) return false;

    const JPH::BodyID id = body->GetID();
    bodies.AddBody(id, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (dynamic && impl_->first_dynamic.IsInvalid()) impl_->first_dynamic = id;
    return true;
}

float JoltPhysicsWorld::firstDynamicY() const {
    if (!impl_->initialized || impl_->first_dynamic.IsInvalid()) return 0.0f;
    return impl_->physics->GetBodyInterface().GetPosition(impl_->first_dynamic).GetY();
}

int JoltPhysicsWorld::bodyCount() const {
    if (!impl_->initialized) return 0;
    return static_cast<int>(impl_->physics->GetNumBodies());
}

} // namespace emergent
