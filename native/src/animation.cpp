#include "emergent/animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/containers/string.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/transform.h>
#include <ozz/base/memory/allocator.h>
#include <ozz/base/span.h>

namespace emergent {
namespace {

using ozz::math::Float3;
using ozz::math::Quaternion;
using ozz::math::SoaTransform;

constexpr float kTwoPi = 6.2831853f;

// One rest-pose joint, in the order ozz stores them.
struct JointDef {
    const char* name;
    int parent;      // -1 for the root
    float offset[3]; // rest-pose translation, in parent space
};

// Depth-first order, and the order matters twice over: ozz's SkeletonBuilder
// stores joints in exactly the sequence IterateJointsDF visits them, and
// LocalToModelJob resolves a joint from its parent's index, so a parent must
// always precede its children. Animation tracks have to use the same order or
// sampled poses land on the wrong bones.
constexpr JointDef kRig[] = {
    {"Hips", -1, {0.00f, 0.00f, 0.00f}},
    {"Spine", 0, {0.00f, 0.22f, 0.00f}},
    {"Neck", 1, {0.00f, 0.18f, 0.00f}},
    {"Head", 2, {0.00f, 0.12f, 0.00f}},
    {"LeftArm", 1, {0.18f, 0.12f, 0.00f}},
    {"LeftHand", 4, {0.26f, 0.00f, 0.00f}},
    {"RightArm", 1, {-0.18f, 0.12f, 0.00f}},
    {"RightHand", 6, {-0.26f, 0.00f, 0.00f}},
    {"LeftLeg", 0, {0.09f, -0.05f, 0.00f}},
    {"RightLeg", 0, {-0.09f, -0.05f, 0.00f}},
};
constexpr int kJointCount = static_cast<int>(sizeof(kRig) / sizeof(kRig[0]));

// Indices into kRig, used to keep the motion curves readable.
enum RigJoint {
    kHips = 0,
    kSpine,
    kNeck,
    kHead,
    kLeftArm,
    kLeftHand,
    kRightArm,
    kRightHand,
    kLeftLeg,
    kRightLeg,
};

// A locomotion clip, described by the handful of numbers that define a walk
// cycle. Keeping the description this small is what lets the whole rig be
// authored in code instead of shipped as a binary asset.
struct ClipDef {
    const char* name;
    float duration;  // seconds for one full cycle
    float swing;     // limb swing amplitude, radians
    float bob;       // vertical hip bob, metres
    float lean;      // forward spine pitch, radians
    float headTurn;  // head yaw amplitude, radians
};

constexpr ClipDef kClips[] = {
    {"idle", 2.0f, 0.05f, 0.006f, 0.03f, 0.10f},
    {"walk", 1.0f, 0.55f, 0.030f, 0.14f, 0.05f},
    {"run", 0.60f, 0.95f, 0.055f, 0.32f, 0.03f},
};
constexpr int kClipCount = static_cast<int>(sizeof(kClips) / sizeof(kClips[0]));

// Five keys per track: a quarter-cycle apart, which is the sampling rate the
// curves below actually need. ozz interpolates linearly between them.
constexpr int kKeyCount = 5;

// Per-joint rotation for one clip at normalised cycle position `u` in [0,1].
// Returned as ozz's (yaw, pitch, roll) triple.
Float3 jointEuler(int joint, float u, const ClipDef& clip) {
    const float phase = u * kTwoPi;
    const float s = std::sin(phase);
    switch (joint) {
        case kHips:
            // The pelvis counter-rotates against the shoulders.
            return Float3(0.10f * clip.swing * s, 0.0f, 0.0f);
        case kSpine:
            return Float3(0.0f, clip.lean - 0.15f * clip.swing * s, 0.0f);
        case kNeck:
            return Float3(0.0f, -0.5f * clip.lean, 0.0f);
        case kHead:
            return Float3(clip.headTurn * s, -0.3f * clip.lean, 0.0f);
        case kLeftArm:
            return Float3(0.0f, -clip.swing * s, 0.06f);
        case kLeftHand:
            return Float3(0.0f, -0.6f * clip.swing * s, 0.0f);
        case kRightArm:
            return Float3(0.0f, clip.swing * s, -0.06f);
        case kRightHand:
            return Float3(0.0f, 0.6f * clip.swing * s, 0.0f);
        case kLeftLeg: {
            // The trailing leg bends outward on the recovery half of the cycle.
            const float bend = std::max(0.0f, -s);
            return Float3(0.0f, clip.swing * s, 0.35f * clip.swing * bend);
        }
        case kRightLeg: {
            const float bend = std::max(0.0f, s);
            return Float3(0.0f, -clip.swing * s, -0.35f * clip.swing * bend);
        }
        default:
            return Float3::zero();
    }
}

// Vertical hip offset. Two dips per cycle, once per footfall.
float hipBob(float u, const ClipDef& clip) {
    return clip.bob * (0.5f - 0.5f * std::cos(2.0f * u * kTwoPi));
}

using RawJoint = ozz::animation::offline::RawSkeleton::Joint;

// Recursively assembles the nested RawSkeleton tree ozz expects. The
// depth-first walk below must reproduce kRig's order exactly, which build()
// asserts before anything is baked.
RawJoint makeJoint(int index, const std::vector<std::vector<int>>& children) {
    RawJoint joint;
    joint.name = kRig[index].name;
    joint.transform.translation =
        Float3(kRig[index].offset[0], kRig[index].offset[1], kRig[index].offset[2]);
    for (const int child : children[index]) joint.children.push_back(makeJoint(child, children));
    return joint;
}

// RAII buffer for ozz's 16-byte-aligned SoA types. std::vector is not usable
// for them: its allocator only guarantees __STDCPP_DEFAULT_NEW_ALIGNMENT__.
template <typename T>
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    ~AlignedBuffer() { release(); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    // Returns nullptr if the count is zero, so callers can treat an empty
    // buffer the same way they treat an allocation failure.
    AlignedBuffer(AlignedBuffer &&other) noexcept : data_(other.data_), count_(other.count_) {
        other.data_ = nullptr;
        other.count_ = 0;
    }
    AlignedBuffer &operator=(AlignedBuffer &&other) noexcept {
        if (this != &other) {
            release();
            data_ = other.data_;
            count_ = other.count_;
            other.data_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }

    T* allocate(int count) {
        release();
        if (count <= 0) return nullptr;
        void* memory = ozz::memory::default_allocator()->Allocate(
            sizeof(T) * static_cast<size_t>(count), alignof(T));
        if (!memory) return nullptr;
        data_ = new (memory) T[static_cast<size_t>(count)];
        count_ = count;
        return data_;
    }

    void release() {
        if (!data_) return;
        for (int i = 0; i < count_; ++i) data_[i].~T();
        ozz::memory::default_allocator()->Deallocate(data_);
        data_ = nullptr;
        count_ = 0;
    }

    T* get() const { return data_; }
    int count() const { return count_; }

    ozz::span<T> span() const {
        return data_ ? ozz::span<T>(data_, static_cast<size_t>(count_)) : ozz::span<T>();
    }
    ozz::span<const T> const_span() const {
        return data_ ? ozz::span<const T>(data_, static_cast<size_t>(count_)) : ozz::span<const T>();
    }

private:
    T* data_ = nullptr;
    int count_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// AnimationLibrary
// ---------------------------------------------------------------------------

struct AnimationLibrary::Impl {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    std::vector<ozz::animation::Animation> clips;
    std::vector<std::string> clip_names;
    std::unordered_map<std::string, int> clip_lookup;
    std::vector<std::string> joint_names;
    std::unordered_map<std::string, int> joint_lookup;
    bool ready = false;

    int clipIndex(std::string_view name) const {
        const auto it = clip_lookup.find(std::string(name));
        return it == clip_lookup.end() ? -1 : it->second;
    }
};

AnimationLibrary::AnimationLibrary() : impl_(std::make_unique<Impl>()) {}
AnimationLibrary::~AnimationLibrary() = default;
AnimationLibrary::AnimationLibrary(AnimationLibrary&&) noexcept = default;
AnimationLibrary& AnimationLibrary::operator=(AnimationLibrary&&) noexcept = default;

bool AnimationLibrary::ready() const noexcept { return impl_ && impl_->ready; }

int AnimationLibrary::jointCount() const noexcept {
    return ready() ? static_cast<int>(impl_->joint_names.size()) : 0;
}

int AnimationLibrary::jointIndex(std::string_view name) const {
    if (!ready()) return -1;
    const auto it = impl_->joint_lookup.find(std::string(name));
    return it == impl_->joint_lookup.end() ? -1 : it->second;
}

std::string AnimationLibrary::jointName(int joint) const {
    if (!ready() || joint < 0 || joint >= static_cast<int>(impl_->joint_names.size())) return {};
    return impl_->joint_names[static_cast<size_t>(joint)];
}

std::vector<std::string> AnimationLibrary::jointNames() const {
    return ready() ? impl_->joint_names : std::vector<std::string>{};
}

int AnimationLibrary::clipCount() const noexcept {
    return ready() ? static_cast<int>(impl_->clip_names.size()) : 0;
}

bool AnimationLibrary::hasClip(std::string_view name) const {
    return ready() && impl_->clipIndex(name) >= 0;
}

std::vector<std::string> AnimationLibrary::clipNames() const {
    return ready() ? impl_->clip_names : std::vector<std::string>{};
}

float AnimationLibrary::clipDuration(std::string_view name) const {
    if (!ready()) return 0.0f;
    const int index = impl_->clipIndex(name);
    return index < 0 ? 0.0f : impl_->clips[static_cast<size_t>(index)].duration();
}

int AnimationLibrary::animatedJointCount() const noexcept {
    return ready() && !impl_->clips.empty() ? impl_->clips.front().num_tracks() : 0;
}

void AnimationLibrary::clear() {
    impl_->clips.clear();
    impl_->clip_names.clear();
    impl_->clip_lookup.clear();
    impl_->joint_names.clear();
    impl_->joint_lookup.clear();
    impl_->skeleton.reset();
    impl_->ready = false;
}

/**
 * Authors the biped rig and its locomotion clips, then hands both to ozz to
 * bake into the compressed runtime representation.
 *
 * Baking is the expensive half and is done once: the resulting Skeleton and
 * Animation objects are what every frame reads. A build that fails anywhere
 * leaves the library empty rather than partly populated, so `ready()` is a
 * sufficient check for callers.
 */
bool AnimationLibrary::build() {
    if (ready()) return true;

    const auto fail = [](const char *why) {
        std::fprintf(stderr, "[animation] build failed: %s\n", why);
        return false;
    };

    Impl next;

    // -- skeleton ---------------------------------------------------------
    std::vector<std::vector<int>> children(kJointCount);
    for (int i = 0; i < kJointCount; ++i) {
        if (kRig[i].parent >= 0) children[static_cast<size_t>(kRig[i].parent)].push_back(i);
    }

    ozz::animation::offline::RawSkeleton raw_skeleton;
    raw_skeleton.roots.push_back(makeJoint(0, children));
    if (!raw_skeleton.Validate()) return fail("RawSkeleton::Validate rejected the rig");

    const ozz::animation::offline::SkeletonBuilder skeleton_builder;
    next.skeleton = skeleton_builder(raw_skeleton);
    if (!next.skeleton) return fail("SkeletonBuilder produced no skeleton");
    if (next.skeleton->num_joints() != kJointCount) return fail("baked joint count differs from the rig table");

    // Bake time is the point of no return for the ordering assumption the
    // whole clip set depends on, so it is checked before anything is baked.
    const ozz::span<const char* const> baked_names = next.skeleton->joint_names();
    for (int i = 0; i < kJointCount; ++i) {
        if (std::strcmp(baked_names[static_cast<size_t>(i)], kRig[i].name) != 0) {
            return fail("baked joints are not in the expected depth-first order");
        }
    }
    for (int i = 0; i < kJointCount; ++i) {
        next.joint_names.emplace_back(kRig[i].name);
        next.joint_lookup.emplace(kRig[i].name, i);
    }

    // -- clips ------------------------------------------------------------
    ozz::animation::offline::AnimationBuilder animation_builder;
    // An iframe every 100ms lets the sampler seek without walking the whole
    // clip, which matters for short locomotion clips that loop constantly.
    animation_builder.iframe_interval = 0.1f;

    next.clips.reserve(static_cast<size_t>(kClipCount));
    for (const ClipDef& clip : kClips) {
        ozz::animation::offline::RawAnimation raw;
        raw.name = clip.name;
        raw.duration = clip.duration;
        raw.tracks.resize(static_cast<size_t>(kJointCount));

        for (int joint = 0; joint < kJointCount; ++joint) {
            auto& track = raw.tracks[static_cast<size_t>(joint)];
            for (int key = 0; key < kKeyCount; ++key) {
                const float u = static_cast<float>(key) / static_cast<float>(kKeyCount - 1);
                const float time = u * clip.duration;

                // A clip stores a joint's full local transform, not a delta on
                // top of the rest pose, so every animated joint carries its
                // rest offset. Dropping it would collapse the whole rig onto
                // the origin.
                Float3 position(kRig[joint].offset[0], kRig[joint].offset[1], kRig[joint].offset[2]);
                if (joint == kHips) position.y += hipBob(u, clip);

                track.translations.push_back({time, position});
                track.rotations.push_back({time, Quaternion::FromEuler(jointEuler(joint, u, clip))});
            }
            // Scales stay empty: ozz fills those tracks with Float3::one(),
            // which is the identity scale a locomotion clip wants.
        }

        if (!raw.Validate()) return fail("RawAnimation::Validate rejected a clip");
        ozz::unique_ptr<ozz::animation::Animation> baked = animation_builder(raw);
        if (!baked) return fail("AnimationBuilder produced no clip");
        if (baked->num_tracks() != kJointCount) return fail("a baked clip drives the wrong track count");
        if (baked->num_soa_tracks() != next.skeleton->num_soa_joints()) return fail("clip and skeleton SoA layouts differ");

        next.clip_names.emplace_back(clip.name);
        next.clip_lookup.emplace(clip.name, static_cast<int>(next.clips.size()));
        next.clips.push_back(std::move(*baked));
    }

    next.ready = true;
    *impl_ = std::move(next);
    return true;
}

// ---------------------------------------------------------------------------
// Animator
// ---------------------------------------------------------------------------

struct Animator::Impl {
    const AnimationLibrary* library = nullptr;
    const ozz::animation::Animation* clip = nullptr;
    std::string clip_name;
    float time = 0.0f;
    float speed = 1.0f;
    bool loop = true;
    bool has_clip = false;
    // Reused every frame by SamplingJob; ozz documents the context as the
    // place to keep per-clip state so sequential sampling stays cheap.
    std::unique_ptr<ozz::animation::SamplingJob::Context> sample_context;
};

Animator::Animator() : impl_(std::make_unique<Impl>()) {}
Animator::~Animator() = default;
Animator::Animator(Animator&&) noexcept = default;
Animator& Animator::operator=(Animator&&) noexcept = default;

bool Animator::bound() const noexcept { return impl_ && impl_->library != nullptr; }

const AnimationLibrary* Animator::library() const noexcept {
    return impl_ ? impl_->library : nullptr;
}

bool Animator::bind(const AnimationLibrary& library) {
    if (!library.ready()) return false;
    if (impl_->library == &library) return true;
    impl_->library = &library;
    impl_->clip = nullptr;
    impl_->clip_name.clear();
    impl_->has_clip = false;
    impl_->time = 0.0f;
    impl_->sample_context.reset();
    return true;
}

bool Animator::play(std::string_view clip, bool loop) {
    if (!bound() || !impl_->library->hasClip(clip)) return false;

    const int index = impl_->library->impl_->clipIndex(clip);
    if (index < 0) return false;

    impl_->clip = &impl_->library->impl_->clips[static_cast<size_t>(index)];
    impl_->clip_name = std::string(clip);
    impl_->loop = loop;
    impl_->has_clip = true;
    impl_->time = 0.0f;
    // A context sized for this clip's track count. SamplingJob invalidates it
    // itself when the clip changes, but the sizes differ per clip so a fresh
    // one is the only correct state. The context takes a *track* count and
    // rounds up to SoA slots internally.
    impl_->sample_context =
        std::make_unique<ozz::animation::SamplingJob::Context>(impl_->clip->num_tracks());
    return true;
}

bool Animator::playing() const noexcept { return impl_ && impl_->has_clip; }

std::string_view Animator::clip() const noexcept {
    return impl_ ? std::string_view(impl_->clip_name) : std::string_view();
}

float Animator::time() const noexcept { return impl_ ? impl_->time : 0.0f; }

float Animator::duration() const noexcept { return impl_ && impl_->clip ? impl_->clip->duration() : 0.0f; }

float Animator::speed() const noexcept { return impl_ ? impl_->speed : 0.0f; }

void Animator::speed(float value) noexcept {
    if (impl_ && std::isfinite(value)) impl_->speed = value;
}

bool Animator::loop() const noexcept { return impl_ && impl_->loop; }

void Animator::loop(bool value) noexcept {
    if (impl_) impl_->loop = value;
}

void Animator::reset() noexcept {
    if (!impl_) return;
    impl_->time = 0.0f;
    if (impl_->sample_context) impl_->sample_context->Invalidate();
}

void Animator::update(float dt) {
    if (!impl_ || !impl_->has_clip) return;
    // A NaN or negative step would either freeze playback or run the clock
    // backwards through the wrap below, so it is rejected outright.
    if (!std::isfinite(dt) || dt <= 0.0f) return;

    const float duration = impl_->clip->duration();
    if (!(duration > 0.0f)) return;

    impl_->time += dt * impl_->speed;
    if (impl_->loop) {
        if (impl_->time >= duration) {
            impl_->time = std::fmod(impl_->time, duration);
        } else if (impl_->time < 0.0f) {
            impl_->time = std::fmod(impl_->time, duration) + duration;
        }
    } else {
        impl_->time = std::clamp(impl_->time, 0.0f, duration);
    }
}

// ---------------------------------------------------------------------------
// Pose
// ---------------------------------------------------------------------------

struct Pose::Impl {
    const AnimationLibrary* library = nullptr;
    int joints = 0;       // skeleton joints
    int soa_joints = 0;   // SoA slots covering those joints
    AlignedBuffer<SoaTransform> local;
    AlignedBuffer<SoaTransform> scratch;
    AlignedBuffer<ozz::math::Float4x4> models;
    std::vector<float> matrices; // joints * kMatrixFloats
    std::vector<ozz::animation::BlendingJob::Layer> layers;
    bool sampled = false;
    bool resolved = false;
};

Pose::Pose() : impl_(std::make_unique<Impl>()) {}
Pose::~Pose() = default;
Pose::Pose(Pose&&) noexcept = default;
Pose& Pose::operator=(Pose&&) noexcept = default;

bool Pose::bound() const noexcept { return impl_ && impl_->library != nullptr; }

int Pose::jointCount() const noexcept { return impl_ ? impl_->joints : 0; }

bool Pose::bind(const AnimationLibrary& library) {
    if (!library.ready()) return false;
    if (impl_->library == &library && impl_->joints == library.jointCount()) return true;

    Impl next;
    next.library = &library;
    next.joints = library.jointCount();
    next.soa_joints = library.jointCount() > 0 ? (next.joints + 3) / 4 : 0;
    // Local and scratch hold SoA slots; model matrices are per joint, and the
    // runtime Skeleton does not expose how many SoA slots it needs, so it is
    // derived from the joint count the same way ozz derives it internally.
    if (next.joints > 0) {
        if (!next.local.allocate(next.soa_joints)) return false;
        if (!next.scratch.allocate(next.soa_joints)) return false;
        if (!next.models.allocate(next.joints)) return false;
        next.matrices.assign(static_cast<size_t>(next.joints) * kMatrixFloats, 0.0f);
    }

    *impl_ = std::move(next);
    reset();
    return true;
}

void Pose::reset() {
    if (!impl_ || !impl_->library) return;
    impl_->layers.clear();
    impl_->sampled = false;
    impl_->resolved = false;
    // Seed local space with the rest pose. This is also what a pose with no
    // contributors resolves to, and what ozz's blend threshold falls back to
    // for any joint that no layer covers.
    const ozz::span<const SoaTransform> rest = impl_->library->impl_->skeleton->joint_rest_poses();
    const std::size_t slots = static_cast<std::size_t>(impl_->local.count());
    for (std::size_t i = 0; i < rest.size() && i < slots; ++i) impl_->local.get()[i] = rest[i];
    for (int i = 0; i < impl_->models.count(); ++i) {
        impl_->models.get()[i] = ozz::math::Float4x4::identity();
    }
}

bool Pose::sample(const Animator& animator, float weight) {
    if (!impl_ || !impl_->library || impl_->soa_joints == 0) return false;
    if (!animator.bound() || animator.library() != impl_->library || !animator.playing()) return false;
    if (!std::isfinite(weight) || weight <= 0.0f) return true;

    // Refresh the shared context if this animator was reset or rebound since
    // its last sample, so the forward-optimised path never reads stale keys.
    if (animator.impl_->sample_context) animator.impl_->sample_context->Invalidate();

    const ozz::animation::SamplingJob sampler{
        .ratio = animator.duration() > 0.0f ? animator.time() / animator.duration() : 0.0f,
        .animation = animator.impl_->clip,
        .context = animator.impl_->sample_context.get(),
        .output = impl_->scratch.span(),
    };
    if (!sampler.Run()) {
        std::fprintf(stderr, "[animation] SamplingJob failed (soa=%d tracks=%d max_ctx=%d)\n",
            static_cast<int>(impl_->scratch.count()),
            static_cast<int>(animator.impl_->clip->num_soa_tracks()),
            static_cast<int>(animator.impl_->sample_context->max_soa_tracks()));
        return false;
    }

    ozz::animation::BlendingJob::Layer layer;
    layer.weight = weight;
    layer.transform = impl_->scratch.const_span();
    impl_->layers.push_back(layer);

    const ozz::animation::BlendingJob blend{
        .threshold = 0.1f,
        .layers = ozz::span<const ozz::animation::BlendingJob::Layer>(impl_->layers.data(),
                                                                     impl_->layers.size()),
        .additive_layers = ozz::span<const ozz::animation::BlendingJob::Layer>(),
        .rest_pose = impl_->library->impl_->skeleton->joint_rest_poses(),
        .output = impl_->local.span(),
    };
    if (!blend.Run()) {
        std::fprintf(stderr, "[animation] BlendingJob failed (layers=%d rest=%d out=%d)\n",
            static_cast<int>(impl_->layers.size()),
            static_cast<int>(impl_->library->impl_->skeleton->joint_rest_poses().size()),
            static_cast<int>(impl_->local.count()));
        impl_->layers.pop_back();
        return false;
    }

    impl_->sampled = true;
    impl_->resolved = false;
    return true;
}

bool Pose::resolve() {
    if (!impl_ || !impl_->library || impl_->joints == 0) return false;
    if (!impl_->sampled) reset();

    const ozz::animation::LocalToModelJob job{
        .skeleton = impl_->library->impl_->skeleton.get(),
        .input = impl_->local.const_span(),
        .output = impl_->models.span(),
    };
    if (!job.Run()) {
        std::fprintf(stderr, "[animation] LocalToModelJob failed\n");
        return false;
    }

    // ozz's matrices are SIMD registers; copy them out as plain floats so the
    // rest of the engine never depends on the build's SIMD configuration.
    const ozz::math::Float4x4* models = impl_->models.get();
    for (int joint = 0; joint < impl_->joints; ++joint) {
        float* out = impl_->matrices.data() + static_cast<size_t>(joint) * kMatrixFloats;
        for (int column = 0; column < 4; ++column) {
            ozz::math::StorePtrU(models[joint].cols[column], out + column * 4);
        }
    }

    impl_->resolved = true;
    return true;
}

bool Pose::resolved() const noexcept { return impl_ && impl_->resolved; }

const float* Pose::modelMatrix(int joint) const {
    if (!impl_ || !impl_->resolved) return nullptr;
    if (joint < 0 || joint >= impl_->joints) return nullptr;
    return impl_->matrices.data() + static_cast<size_t>(joint) * kMatrixFloats;
}

const float* Pose::modelMatrix(std::string_view name) const {
    if (!impl_ || !impl_->library) return nullptr;
    const int joint = impl_->library->jointIndex(name);
    return joint < 0 ? nullptr : modelMatrix(joint);
}

bool Pose::modelMatrix(std::string_view name, float (&out)[kMatrixFloats]) const {
    const float* matrix = modelMatrix(name);
    if (!matrix) return false;
    std::memcpy(out, matrix, sizeof(out));
    return true;
}

bool Pose::jointTranslation(std::string_view name, float (&out)[3]) const {
    const float* matrix = modelMatrix(name);
    if (!matrix) return false;
    // Column-major: the translation lives in the fourth column.
    out[0] = matrix[12];
    out[1] = matrix[13];
    out[2] = matrix[14];
    return true;
}

} // namespace emergent
