#include "emergent/frame_loop.hpp"
#include "emergent/profiling.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace emergent {
namespace {

double elapsedMs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Explicit LCG rather than <random>.
//
// This is not a stylistic choice and it is the reason the loop is
// reproducible. The distributions in <random> are explicitly permitted to
// produce different values on different standard library implementations, and
// the engine that uses them does so. A property-based test that seeds a scene
// and asserts on positions would pass locally and fail on the next machine to
// touch it. A LCG written out here has one defined answer forever.
struct Lcg {
    uint32_t state;
    explicit Lcg(uint32_t seed) : state(seed) {}
    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    // Top 24 bits, which are the well-mixed ones in this generator.
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

enum class Locomotion { Idle, Walk, Run };

const char *clipFor(Locomotion state) {
    switch (state) {
        case Locomotion::Walk: return "walk";
        case Locomotion::Run: return "run";
        case Locomotion::Idle: break;
    }
    return "idle";
}

constexpr double kTwoPi = 6.283185307179586;

} // namespace

float InputState::magnitude() const noexcept {
    return std::sqrt(moveX * moveX + moveZ * moveZ);
}

// Kept out of the loop's own checks: a prop ring with the inner radius beyond
// the outer one produces a ring of props at one radius, which is silly but
// harmless, and rejecting the whole configuration for it would be worse.
static bool propRadiusUsable(const FrameLoopConfig &config) {
    return config.propOuterRadius > config.propInnerRadius && config.propInnerRadius >= 0.0f;
}

bool validateFrameLoopConfig(const FrameLoopConfig &config, std::string &error) {
    error.clear();
    // fixedDelta is checked for finiteness as well as sign: a NaN compares
    // false against every bound, so `!(x > 0)` is the test that catches it.
    if (!(config.fixedDelta > 0.0) || !std::isfinite(config.fixedDelta)) {
        error = "fixedDelta must be a finite value greater than zero";
        return false;
    }
    // A step this long is not a simulation, it is a teleport, and Jolt clamps
    // collision steps to 1 internally so the caller would silently get worse
    // accuracy than the number implies.
    if (config.fixedDelta < 1.0 / 1000.0 || config.fixedDelta > 1.0 / 10.0) {
        error = "fixedDelta must be between 1ms and 100ms";
        return false;
    }
    if (!(config.maxFrameDelta >= config.fixedDelta) || !std::isfinite(config.maxFrameDelta)) {
        error = "maxFrameDelta must be finite and at least fixedDelta";
        return false;
    }
    if (config.maxSubSteps < 1) {
        error = "maxSubSteps must be at least 1, or the loop would never simulate";
        return false;
    }
    if (!(config.playerHalfExtent > 0.0f) || !std::isfinite(config.playerHalfExtent)) {
        error = "playerHalfExtent must be finite and greater than zero";
        return false;
    }
    if (!(config.groundTolerance >= 0.0f) || !std::isfinite(config.groundTolerance)) {
        error = "groundTolerance must be finite and non-negative";
        return false;
    }
    if (!(config.moveSpeed > 0.0f) || !std::isfinite(config.moveSpeed)) {
        error = "moveSpeed must be finite and greater than zero";
        return false;
    }
    if (!propRadiusUsable(config)) {
        error = "propOuterRadius must be greater than propInnerRadius, and propInnerRadius non-negative";
        return false;
    }
    return true;
}

struct FrameLoop::Impl {
    FrameLoopConfig config;
    JoltPhysicsWorld physics;
    AnimationLibrary anim;
    Animator animator;
    Pose pose;

    bool ready = false;
    std::string error;

    int player = -1;
    float spawn[3] = {0.0f, 0.0f, 0.0f};

    // Interpolation window. prev is the state before the first physics step of
    // the current frame, curr is the state after the last one.
    float prevPos[3] = {0.0f, 0.0f, 0.0f};
    float currPos[3] = {0.0f, 0.0f, 0.0f};
    float prevVel[3] = {0.0f, 0.0f, 0.0f};
    float currVel[3] = {0.0f, 0.0f, 0.0f};

    double accum = 0.0;
    double simSeconds = 0.0;
    uint64_t frame = 0;
    bool grounded = false;
    bool jumpHeld = false;
    Locomotion locomotion = Locomotion::Idle;
    bool locomotionLocked = false;  // set by playClip()
    FrameStats stats;
    FrameState lastState;
    float lastMs = 0.0f;
    // Drawable scene. Entry 0 is the player and is rewritten every tick from
    // the interpolated position, so a renderer never has to be told which box
    // is the character.
    std::vector<SceneBox> boxes;

    void fail(std::string message) {
        error = std::move(message);
        ready = false;
    }

    void readState() {
        if (player < 0) return;
        if (!physics.bodyPosition(player, currPos)) {
            std::memset(currPos, 0, sizeof(currPos));
        }
        if (!physics.bodyVelocity(player, currVel)) {
            std::memset(currVel, 0, sizeof(currVel));
        }
    }

    void updateGrounded() {
        // The solver leaves the body resting a hair inside the ground, so an
        // exact comparison to the expected rest height would report the
        // player as airborne forever and every jump would be rejected.
        const float rest = config.groundTopY + config.playerHalfExtent;
        grounded = std::fabs(currPos[1] - rest) <= config.groundTolerance;
    }

    // Applied once per physics substep, not once per frame, so that a frame
    // with three substeps integrates the same total input over the same total
    // time as one that takes a single step. This is what keeps the simulation
    // a function of simulated time.
    void applyInput(const InputState &input, bool jumpEdge) {
        if (player < 0) return;

        float mx = input.moveX;
        float mz = input.moveZ;
        // Normalise a diagonal, or moveX=moveZ=1 would be 41% faster than a
        // cardinal move and the character would drift on any slightly
        // diagonal stick position.
        const float m = std::sqrt(mx * mx + mz * mz);
        if (m > 1.0f) {
            mx /= m;
            mz /= m;
        }

        float vx = mx * config.moveSpeed;
        float vz = mz * config.moveSpeed;
        float vy = 0.0f;
        if (!physics.bodyVelocity(player, currVel)) {
            std::memset(currVel, 0, sizeof(currVel));
        }
        // The vertical component is only ever overwritten by a jump. Writing
        // the current value back on every substep looks like a no-op, and is,
        // but skipping it entirely would be a bug: gravity accumulates inside
        // the solver and has to be allowed to.
        vy = currVel[1];

        if (mx == 0.0f && mz == 0.0f) {
            if (!grounded && !jumpEdge) {
                // Airborne with no input: leave the body entirely alone so
                // momentum is preserved. A character controller that zeroes
                // horizontal velocity mid-air makes every jump feel like glue.
                return;
            }
            // Grounded and idle: stop dead. Relying on friction alone makes
            // the character slide to a halt over a noticeable distance.
            vx = 0.0f;
            vz = 0.0f;
        }

        if (jumpEdge && grounded) {
            vy = config.jumpSpeed;
            // The jump itself should not be swallowed by a same-frame walk
            // input, and it should not be repeatable: the edge is consumed by
            // the caller, not here.
        }
        physics.setBodyVelocity(player, vx, vy, vz);
    }

    // Speed-driven clip selection with hysteresis.
    //
    // Without hysteresis a player hovering exactly on a threshold re-triggers
    // play() every substep, and because play() rewinds to zero, the character
    // vibrates between two clips forever. The exit thresholds are below the
    // entry thresholds so the state has to be clearly past the boundary before
    // it changes.
    void updateLocomotion() {
        const float speed = std::sqrt(currVel[0] * currVel[0] + currVel[2] * currVel[2]);
        lastMs = speed;

        const float runEnter = config.moveSpeed * 0.6f;
        const float runExit = runEnter * 0.85f;
        const float walkExit = 0.25f;
        switch (locomotion) {
            case Locomotion::Idle:
                if (speed > runEnter) locomotion = Locomotion::Run;
                else if (speed > walkExit) locomotion = Locomotion::Walk;
                break;
            case Locomotion::Walk:
                if (speed > runEnter) locomotion = Locomotion::Run;
                else if (speed < walkExit) locomotion = Locomotion::Idle;
                break;
            case Locomotion::Run:
                if (speed < runExit) locomotion = Locomotion::Walk;
                break;
        }

        if (locomotionLocked) return;

        if (animator.clip() != clipFor(locomotion)) {
            animator.play(clipFor(locomotion), true);
        }

        // Match the playback rate to the distance actually covered, so the
        // feet do not slide. A rig animating at 1x while sprinting is the
        // single most common sign that locomotion was never wired to
        // movement.
        float rate = 1.0f;
        if (locomotion == Locomotion::Run) {
            const float nominal = config.moveSpeed;
            if (nominal > 0.0f) rate = speed / nominal;
        } else if (locomotion == Locomotion::Walk) {
            const float nominal = config.moveSpeed * 0.5f;
            if (nominal > 0.0f) rate = speed / nominal;
        }
        if (rate < 0.35f) rate = 0.35f;
        if (rate > 2.0f) rate = 2.0f;
        animator.speed(rate);
    }

    void buildScene() {
        Lcg rng(config.propSeed);
        const float halfHeight = 0.75f;
        // Entry 0 is the player. It is resized in place every tick, so it has
        // to exist before any physics body does.
        SceneBox playerBox{};
        playerBox.halfExtent[0] = config.playerHalfExtent;
        playerBox.halfExtent[1] = config.playerHalfExtent;
        playerBox.halfExtent[2] = config.playerHalfExtent;
        playerBox.color = 0xff6b4a2a;
        boxes.push_back(playerBox);

        for (uint32_t i = 0; i < config.propCount; ++i) {
            // A ring, so the player always has something in view to walk into
            // and nothing can spawn on top of the spawn point.
            const float angle = rng.range(0.0f, static_cast<float>(kTwoPi));
            const float radius = rng.range(config.propInnerRadius, config.propOuterRadius);
            const float x = std::cos(angle) * radius;
            const float z = std::sin(angle) * radius;
            const float hx = rng.range(0.3f, 1.1f);
            const float hz = rng.range(0.3f, 1.1f);
            // Sitting on the ground rather than sunk into it: the y is the
            // block's half-height, so its lower face is at ground level.
            physics.addBox(x, halfHeight, z, hx, halfHeight, hz, false);

            SceneBox box{};
            box.center[0] = x;
            box.center[1] = halfHeight;
            box.center[2] = z;
            box.halfExtent[0] = hx;
            box.halfExtent[1] = halfHeight;
            box.halfExtent[2] = hz;
            // Alternating tint, so overlapping props stay distinguishable
            // without a lighting pass.
            box.color = (i % 2 == 0) ? 0xff4a5568u : 0xff3d4757u;
            boxes.push_back(box);
        }
    }

    // Rewrites entry 0 from the position the renderer is about to be handed.
    void syncPlayerBox(const float *position) {
        if (boxes.empty()) return;
        boxes[0].center[0] = position[0];
        boxes[0].center[1] = position[1];
        boxes[0].center[2] = position[2];
    }
};

FrameLoop::FrameLoop() : impl_(std::make_unique<Impl>()) {}
FrameLoop::~FrameLoop() { shutdown(); }
FrameLoop::FrameLoop(FrameLoop &&) noexcept = default;
FrameLoop &FrameLoop::operator=(FrameLoop &&) noexcept = default;

bool FrameLoop::initialize(const FrameLoopConfig &config) {
    shutdown();
    // Rebuilt from scratch every time rather than reset in place: a world that
    // was torn down halfway, or a rig that failed to rebind, would leave the
    // loop partially initialised, and a partially initialised frame loop
    // produces a game that looks like it works and simulates nothing.
    auto next = std::make_unique<Impl>();
    impl_ = std::move(next);
    Impl &s = *impl_;

    if (!validateFrameLoopConfig(config, s.error)) return false;
    s.config = config;

    if (!s.physics.initialize()) {
        s.fail("physics world failed to initialize");
        return false;
    }
    s.player = s.physics.primaryDynamicBody();
    if (s.player < 0) {
        s.fail("physics world seeded no dynamic body to drive");
        return false;
    }

    s.spawn[0] = 0.0f;
    s.spawn[1] = config.playerSpawnY;
    s.spawn[2] = 0.0f;

    s.buildScene();

    // Put the character at the configured spawn rather than leaving it where
    // JoltPhysicsWorld::initialize() seeded it. The seeded body is four units
    // up, which is a fine default for a world that wants to show gravity and
    // the wrong starting state for a loop that is about to be driven.
    if (!s.physics.setBodyPosition(s.player, s.spawn[0], s.spawn[1], s.spawn[2])) {
        s.fail("could not place the character at its spawn point");
        return false;
    }

    if (!s.anim.build()) {
        s.fail("animation rig failed to bake");
        return false;
    }
    if (!s.animator.bind(s.anim)) {
        s.fail("animator failed to bind to the baked rig");
        return false;
    }
    if (!s.pose.bind(s.anim)) {
        s.fail("pose failed to bind to the baked rig");
        return false;
    }
    if (!s.animator.play("idle", true)) {
        s.fail("the baked rig has no 'idle' clip");
        return false;
    }

    s.readState();
    std::memcpy(s.prevPos, s.currPos, sizeof(s.prevPos));
    std::memcpy(s.prevVel, s.currVel, sizeof(s.prevVel));
    s.updateGrounded();
    s.locomotion = Locomotion::Idle;
    s.ready = true;
    return true;
}

void FrameLoop::shutdown() noexcept {
    if (impl_) {
        impl_->ready = false;
        impl_->animator = Animator();
        impl_->pose = Pose();
        impl_->anim.clear();
        impl_->physics.shutdown();
    }
}

bool FrameLoop::ready() const noexcept { return impl_ && impl_->ready; }
const FrameLoopConfig &FrameLoop::config() const noexcept { return impl_->config; }
const JoltPhysicsWorld &FrameLoop::physics() const noexcept { return impl_->physics; }
const AnimationLibrary &FrameLoop::animation() const noexcept { return impl_->anim; }
const Animator &FrameLoop::animator() const noexcept { return impl_->animator; }
const std::string &FrameLoop::lastError() const noexcept { return impl_->error; }
const FrameStats &FrameLoop::stats() const noexcept { return impl_->stats; }
const FrameState &FrameLoop::lastFrame() const noexcept { return impl_->lastState; }
uint64_t FrameLoop::frameIndex() const noexcept { return impl_->frame; }
int FrameLoop::jointCount() const noexcept { return impl_->anim.jointCount(); }
int FrameLoop::bodyCount() const noexcept { return impl_->physics.bodyCount(); }
int FrameLoop::playerBody() const noexcept { return impl_->player; }
float FrameLoop::playerSpeed() const noexcept { return impl_->lastMs; }
bool FrameLoop::grounded() const noexcept { return impl_->grounded; }
std::string_view FrameLoop::currentClip() const noexcept { return impl_->animator.clip(); }

int FrameLoop::stepsForSeconds(double seconds) const noexcept {
    const double d = impl_->config.fixedDelta;
    if (!(seconds > 0.0) || !(d > 0.0)) return 0;
    return static_cast<int>(seconds / d);
}

bool FrameLoop::playClip(std::string_view clip, bool loop) {
    if (!ready()) return false;
    // Locking the state machine is what makes playClip() mean "play this",
    // rather than being silently overridden on the next substep by whatever
    // the speed happens to be.
    impl_->locomotionLocked = !impl_->animator.play(clip, loop);
    return !impl_->locomotionLocked;
}

void FrameLoop::resumeLocomotion() { impl_->locomotionLocked = false; }

bool FrameLoop::setSpawn(double x, double z, double groundY) {
    if (!impl_) return false;
    Impl &s = *impl_;
    s.spawn[0] = static_cast<float>(x);
    s.spawn[1] = static_cast<float>(groundY);
    s.spawn[2] = static_cast<float>(z);
    // Move the character too, not just the point it will return to. Setting only
    // the spawn leaves the player standing where they were, which reads as the
    // call having had no effect until something calls reset().
    if (s.player >= 0) {
        return s.physics.setBodyPosition(s.player, s.spawn[0], s.spawn[1], s.spawn[2]);
    }
    return true;
}

void FrameLoop::reset() noexcept {
    Impl &s = *impl_;
    if (s.player >= 0) s.physics.setBodyPosition(s.player, s.spawn[0], s.spawn[1], s.spawn[2]);
    s.readState();
    std::memcpy(s.prevPos, s.currPos, sizeof(s.prevPos));
    std::memcpy(s.prevVel, s.currVel, sizeof(s.prevVel));
    s.accum = 0.0;
    s.simSeconds = 0.0;
    s.frame = 0;
    s.jumpHeld = false;
    s.grounded = false;
    s.locomotion = Locomotion::Idle;
    s.locomotionLocked = false;
    s.lastMs = 0.0f;
    s.updateGrounded();
    s.syncPlayerBox(s.currPos);
    s.animator.reset();
    s.animator.play("idle", true);
    s.pose.reset();
    s.pose.sample(s.animator, 1.0f);
    s.pose.resolve();
    s.stats = FrameStats{};
    s.lastState = FrameState{};
}

bool FrameLoop::tick(double realDeltaSeconds, const InputState &input, FrameState &out) {
    if (!ready()) {
        if (impl_ && impl_->error.empty()) impl_->error = "frame loop is not initialized";
        return false;
    }
    Impl &s = *impl_;

    const auto frameStart = std::chrono::steady_clock::now();

    FrameStats st;
    st.frameIndex = s.frame;
    st.rawDelta = realDeltaSeconds;

    // A clock that reports a negative or non-finite delta is broken, and the
    // accumulator is the one place where that would compound into a permanent
    // error rather than a visible one. A bad frame is a zero-length frame.
    //
    // Note that this includes infinity, which is treated as zero rather than
    // clamped. A very large finite delta is a stall and gets clamped to
    // maxFrameDelta, which is the right response. An infinite one is a clock
    // that has failed outright, and there is no honest "elapsed time" to
    // recover from it -- clamping it would invent 250ms of simulation that did
    // not happen.
    double delta = realDeltaSeconds;
    if (!std::isfinite(delta) || delta < 0.0) delta = 0.0;
    if (delta > s.config.maxFrameDelta) {
        delta = s.config.maxFrameDelta;
        st.clamped = true;
    }
    st.delta = delta;
    s.accum += delta;

    // Edge detection against the previous *frame*, not the previous substep.
    // Detecting per substep would make a jump held across a three-substep
    // frame fire three times, and per frame is the only definition a player
    // can actually perform.
    const bool jumpEdge = input.jump && !s.jumpHeld;
    s.jumpHeld = input.jump;

    int steps = 0;
    double physicsMs = 0.0;

    if (s.accum >= s.config.fixedDelta) {
        // Shifted once per frame. With three substeps, `previous` must be the
        // state before the *first* of them: a frame covers 50 ms and has to
        // interpolate across all 50 ms, not across the last 16 of them.
        std::memcpy(s.prevPos, s.currPos, sizeof(s.prevPos));
        std::memcpy(s.prevVel, s.currVel, sizeof(s.prevVel));

        {
            const profile::Scope scope("frame.physics");
            const auto t0 = std::chrono::steady_clock::now();
            while (s.accum >= s.config.fixedDelta && steps < s.config.maxSubSteps) {
                s.applyInput(input, jumpEdge);
                s.physics.step(static_cast<float>(s.config.fixedDelta));
                s.readState();
                s.accum -= s.config.fixedDelta;
                s.simSeconds += s.config.fixedDelta;
                ++steps;
            }
            physicsMs = elapsedMs(t0, std::chrono::steady_clock::now());
        }

        if (s.accum >= s.config.fixedDelta) {
            // The substep cap was reached with time still owed. The backlog
            // is discarded rather than carried forward: carrying it means
            // every following frame starts already behind, costs more to
            // catch up, and falls further behind. That is the spiral of death,
            // and it starts exactly here. Losing simulation time is the
            // smaller failure, and stepsDropped records that it happened
            // rather than hiding it.
            s.accum = 0.0;
            st.stepsDropped = true;
        }

        s.updateGrounded();
        s.updateLocomotion();
    } else {
        // No physics step this frame, so nothing moved. Collapse the
        // interpolation window onto the current state: `previous` would
        // otherwise still hold the state from before the last stepping frame
        // and the renderer would be handed a position *behind* the
        // simulation. On a 240 Hz display running a 60 Hz simulation, half of
        // all frames come through here, so this is the common case and not a
        // corner.
        std::memcpy(s.prevPos, s.currPos, sizeof(s.prevPos));
        std::memcpy(s.prevVel, s.currVel, sizeof(s.prevVel));
    }

    st.steps = steps;
    st.alpha = s.accum / s.config.fixedDelta;
    // One-ulp guard on the documented [0,1) contract. On this path accum is
    // provably less than fixedDelta -- otherwise a step would have been taken
    // -- but dividing (fixedDelta - 1ulp) by fixedDelta rounds to exactly 1.0
    // in double, so a value that is arithmetically inside the range comes out
    // on the boundary. It is harmless for an interpolator, which produces the
    // same point either way, but a caller is entitled to the range it was
    // promised and a test is entitled to assert it. Pulling it back to the
    // largest double below 1.0 costs nothing and makes both true.
    if (!(st.alpha < 1.0)) st.alpha = std::nextafter(1.0, 0.0);
    st.simulatedSeconds = s.simSeconds;
    st.physicsMs = physicsMs;

    // Animation advances once per frame, by the *real* delta, and never
    // inside the substep loop. A clip is a pure function of elapsed time with
    // no state that can diverge, so driving it at the presentation rate is
    // safe and makes motion smooth at any refresh rate; driving it at the
    // fixed rate would make a 240 Hz display animate in visible 60 Hz steps.
    // This is the whole reason the two rates are kept apart.
    double animationMs = 0.0;
    {
        const profile::Scope scope("frame.animation");
        const auto t0 = std::chrono::steady_clock::now();
        s.animator.update(static_cast<float>(st.delta));
        s.pose.reset();
        s.pose.sample(s.animator, 1.0f);
        s.pose.resolve();
        animationMs = elapsedMs(t0, std::chrono::steady_clock::now());
    }
    st.animationMs = animationMs;

    // Assemble the render snapshot. Nothing below this line reads physics
    // state directly: the renderer gets a copy, so it can never observe a
    // half-stepped world no matter what it does with the pointer.
    out.frameIndex = st.frameIndex;
    out.alpha = st.alpha;
    out.realDelta = st.delta;
    out.physicsSteps = steps;
    for (int i = 0; i < 3; ++i) {
        out.playerPrevious[i] = s.prevPos[i];
        out.playerNext[i] = s.currPos[i];
        out.playerPosition[i] = s.prevPos[i] + (s.currPos[i] - s.prevPos[i]) * static_cast<float>(st.alpha);
        out.playerVelocity[i] = s.prevVel[i] + (s.currVel[i] - s.prevVel[i]) * static_cast<float>(st.alpha);
    }
    out.grounded = s.grounded;
    s.syncPlayerBox(out.playerPosition);
    out.sceneBoxes = s.boxes.data();
    out.sceneBoxCount = static_cast<int>(s.boxes.size());

    out.jointCount = s.pose.jointCount();
    out.jointMatrices = s.pose.modelMatrix(0);

    // The camera follows the animated head, not the body origin, so it cannot
    // disagree with the skeleton about where the character is looking. It
    // falls back to the body if the pose is somehow unresolved.
    float head[3] = {};
    if (out.jointMatrices && s.pose.jointTranslation("Head", head)) {
        for (int i = 0; i < 3; ++i) out.cameraTarget[i] = head[i];
    } else {
        for (int i = 0; i < 3; ++i) out.cameraTarget[i] = out.playerPosition[i];
        out.cameraTarget[1] += s.config.playerHalfExtent * 1.5f;
    }

    st.totalMs = elapsedMs(frameStart, std::chrono::steady_clock::now());
    s.stats = st;
    // Retained, not borrowed: the caller may hold this reference across the
    // next tick and a stale frame that reads as current is worse than a copy.
    s.lastState = out;
    ++s.frame;
    return true;
}

uint32_t FrameLoop::runDeterministic(uint32_t frames, double delta, const InputState &input, FrameState &out) {
    if (!ready()) return 0;
    uint32_t ran = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (!tick(delta, input, out)) break;
        ++ran;
    }
    return ran;
}

} // namespace emergent
