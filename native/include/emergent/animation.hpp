#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace emergent {

// Skeletal animation built on ozz-animation.
//
// ozz separates authoring from playback: `ozz::animation::offline` turns
// skeleton/clip descriptions into the compressed runtime format, and
// `ozz::animation` runtime jobs (SamplingJob, BlendingJob, LocalToModelJob)
// evaluate it every frame. EMERGENT owns the authoring half: the biped rig and
// its locomotion clips below are generated procedurally from joint motion
// curves, so the engine ships a playable skeleton without a binary asset
// dependency or an offline tool run.
//
// No ozz type appears in this header. Callers see names, floats and matrices,
// which keeps the upstream library behind the boundary the rest of the native
// tree already uses.

// Floats in a 4x4 model-space joint transform, in ozz's column-major layout.
inline constexpr int kMatrixFloats = 16;

class Pose;

/// Owns the baked skeleton and every baked clip for one rig.
class AnimationLibrary {
public:
    AnimationLibrary();
    ~AnimationLibrary();
    AnimationLibrary(AnimationLibrary&&) noexcept;
    AnimationLibrary& operator=(AnimationLibrary&&) noexcept;
    AnimationLibrary(const AnimationLibrary&) = delete;
    AnimationLibrary& operator=(const AnimationLibrary&) = delete;

    // Bakes the procedural biped skeleton and its clips. Safe to call twice;
    // the second call is a no-op. Returns false only if ozz rejects the
    // generated data, in which case the library is left empty rather than
    // half-built.
    bool build();
    void clear();
    bool ready() const noexcept;

    int jointCount() const noexcept;
    // -1 when the name is not a joint of this skeleton.
    int jointIndex(std::string_view name) const;
    // Empty string for an out-of-range index.
    std::string jointName(int joint) const;
    std::vector<std::string> jointNames() const;

    int clipCount() const noexcept;
    bool hasClip(std::string_view name) const;
    std::vector<std::string> clipNames() const;
    // 0 for an unknown clip.
    float clipDuration(std::string_view name) const;
    // Joints driven by the clip set. Equal to jointCount() for the rig shipped
    // here; the distinction is kept because ozz allows partial animation.
    int animatedJointCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Animator;
    friend class Pose;
};

/// Playback state for one entity: which clip, how far through it, how fast.
///
/// An Animator borrows its library; the library must outlive every Animator
/// that plays from it.
class Animator {
public:
    Animator();
    ~Animator();
    Animator(Animator&&) noexcept;
    Animator& operator=(Animator&&) noexcept;
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;

    // Binds to `library`. Must be called before play(). Binds an already-bound
    // animator to a different library, discarding playback state.
    bool bind(const AnimationLibrary& library);
    bool bound() const noexcept;
    const AnimationLibrary* library() const noexcept;

    // Selects a clip and rewinds to its start. Returns false for an unbound
    // animator or an unknown clip, leaving the current playback untouched.
    bool play(std::string_view clip, bool loop = true);
    // True between a successful play() and the next play().
    bool playing() const noexcept;
    std::string_view clip() const noexcept;
    // Seconds elapsed in the current clip, always < duration() while playing.
    float time() const noexcept;
    float duration() const noexcept;
    // Playback rate multiplier. 0 freezes, negative plays backwards.
    float speed() const noexcept;
    void speed(float value) noexcept;
    bool loop() const noexcept;
    void loop(bool value) noexcept;

    // Rewinds to the start without changing the clip.
    void reset() noexcept;
    // Advances playback by `dt` seconds, wrapping or clamping per loop().
    // A non-positive or non-finite dt is ignored rather than corrupting time.
    void update(float dt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Pose;
};

/// A resolved set of joint transforms: local-space accumulation blended from
/// one or more Animators, then resolved to model space.
///
/// Sample each contributing animator with its weight, then call resolve()
/// once. Weights accumulate across sample() calls and normalise inside ozz's
/// BlendingJob; joints whose accumulated weight falls below the blend threshold
/// fall back to the skeleton rest pose.
class Pose {
public:
    Pose();
    ~Pose();
    Pose(Pose&&) noexcept;
    Pose& operator=(Pose&&) noexcept;
    Pose(const Pose&) = delete;
    Pose& operator=(const Pose&) = delete;

    // Allocates buffers for `library`'s joint count. Rebinding to a different
    // library is allowed and discards any accumulated pose.
    bool bind(const AnimationLibrary& library);
    bool bound() const noexcept;
    int jointCount() const noexcept;

    // Returns the pose to the rest pose, clearing accumulated weights.
    void reset();
    // Samples `animator` and blends it in at `weight`. A non-positive weight
    // contributes nothing. Returns false if the animator is not bound to this
    // pose's library.
    bool sample(const Animator& animator, float weight = 1.0f);
    // Resolves the accumulated local pose into model-space matrices. Must be
    // called after at least one sample() to produce meaningful output;
    // otherwise the rest pose is resolved.
    bool resolve();
    bool resolved() const noexcept;

    // Model-space transform of joint `joint`, or nullptr if unresolved.
    const float* modelMatrix(int joint) const;
    const float* modelMatrix(std::string_view name) const;
    // Copies into `out`. Returns false for an unknown joint or an unresolved
    // pose, leaving `out` untouched.
    bool modelMatrix(std::string_view name, float (&out)[kMatrixFloats]) const;
    // World-space translation of a joint: elements 12, 13, 14 of its matrix.
    bool jointTranslation(std::string_view name, float (&out)[3]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace emergent
