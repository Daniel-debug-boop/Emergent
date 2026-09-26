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
#include <vector>

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
    // Handle table. A BodyID is a compact index into Jolt's own storage but is
    // opaque to callers and is not guaranteed to stay valid across body
    // removal, so the world hands out indices into this vector instead and
    // keeps the mapping in one place.
    std::vector<JPH::BodyID> handles;
    int primary_dynamic = -1;
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
    impl_->handles.clear();
    impl_->primary_dynamic = -1;
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
    return addBox(x, y, z, hx, hy, hz, dynamic) >= 0;
}

int JoltPhysicsWorld::addBox(float x, float y, float z, float hx, float hy, float hz, bool dynamic) {
    if (!impl_->initialized) return -1;
    // A zero or negative extent produces a degenerate shape that Jolt may
    // accept and then behave unpredictably, so reject it up front.
    if (!(hx > 0.0f) || !(hy > 0.0f) || !(hz > 0.0f)) return -1;

    JPH::BoxShapeSettings shape_settings(JPH::Vec3(hx, hy, hz));
    JPH::ShapeSettings::ShapeResult shape = shape_settings.Create();
    if (shape.HasError()) return -1;

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
    if (body == nullptr) return -1;

    const JPH::BodyID id = body->GetID();
    bodies.AddBody(id, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (dynamic && impl_->first_dynamic.IsInvalid()) impl_->first_dynamic = id;
    if (dynamic && impl_->primary_dynamic < 0) impl_->primary_dynamic = static_cast<int>(impl_->handles.size());

    const int handle = static_cast<int>(impl_->handles.size());
    impl_->handles.push_back(id);
    return handle;
}

bool JoltPhysicsWorld::bodyPosition(int handle, float (&out)[3]) const {
    if (!impl_->initialized || handle < 0 || static_cast<size_t>(handle) >= impl_->handles.size()) return false;
    const JPH::RVec3 p = impl_->physics->GetBodyInterface().GetPosition(impl_->handles[static_cast<size_t>(handle)]);
    out[0] = p.GetX();
    out[1] = p.GetY();
    out[2] = p.GetZ();
    return true;
}

bool JoltPhysicsWorld::bodyVelocity(int handle, float (&out)[3]) const {
    if (!impl_->initialized || handle < 0 || static_cast<size_t>(handle) >= impl_->handles.size()) return false;
    const JPH::Vec3 v = impl_->physics->GetBodyInterface().GetLinearVelocity(impl_->handles[static_cast<size_t>(handle)]);
    out[0] = v.GetX();
    out[1] = v.GetY();
    out[2] = v.GetZ();
    return true;
}

bool JoltPhysicsWorld::setBodyVelocity(int handle, float x, float y, float z) {
    if (!impl_->initialized || handle < 0 || static_cast<size_t>(handle) >= impl_->handles.size()) return false;
    JPH::BodyInterface &bodies = impl_->physics->GetBodyInterface();
    const JPH::BodyID id = impl_->handles[static_cast<size_t>(handle)];
    if (!bodies.IsAdded(id)) return false;
    // A sleeping body ignores a velocity write until it is woken, so wake it
    // first. Skipping this is why "the character stops responding after
    // standing still" is such a common engine bug.
    bodies.ActivateBody(id);
    bodies.SetLinearVelocity(id, JPH::Vec3(x, y, z));
    return true;
}

bool JoltPhysicsWorld::setBodyPosition(int handle, float x, float y, float z) {
    if (!impl_->initialized || handle < 0 || static_cast<size_t>(handle) >= impl_->handles.size()) return false;
    JPH::BodyInterface &bodies = impl_->physics->GetBodyInterface();
    const JPH::BodyID id = impl_->handles[static_cast<size_t>(handle)];
    if (!bodies.IsAdded(id)) return false;
    // Teleport, not a swept move: anything the body overlapped is left
    // untouched, so this is only ever correct for a reset.
    bodies.SetPositionAndRotation(id, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(),
                                  JPH::EActivation::Activate);
    bodies.SetLinearVelocity(id, JPH::Vec3::sZero());
    bodies.SetAngularVelocity(id, JPH::Vec3::sZero());
    return true;
}

int JoltPhysicsWorld::primaryDynamicBody() const {    if (!impl_->initialized) return -1;
    return impl_->primary_dynamic;
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
