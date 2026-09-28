#pragma once
#include "emergent/animation.hpp"
#include "emergent/jolt_physics.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace emergent {

// The frame loop: one fixed-rate simulation feeding one variable-rate
// presentation.
//
// The two rates are different on purpose, and confusing them is the single
// most common way an engine becomes unreproducible.
//
//   Physics is fixed at exactly `fixedDelta` seconds per step, always. A
//   solver is a function of its timestep; feeding it a 4 ms step one frame
//   and a 31 ms step the next produces different contact resolution for the
//   same input, and the difference compounds. Real time is accumulated and
//   drained in whole steps, so the simulation advances at a rate that depends
//   on nothing but the simulated time that has elapsed.
//
//   Presentation is variable. A frame renders whatever real time has passed,
//   with a fractional leftover, so motion stays smooth on a 144 Hz display and
//   does not judder at 30 fps. Nothing that affects the simulation is driven
//   from the variable delta.
//
// The two are joined by `alpha` (see FrameStats): the fraction of the way to
// the next physics step at which the frame is being drawn. Transforms are
// interpolated between the last two physics states by that fraction, so what
// reaches the screen is smooth while what reaches the simulation stays
// deterministic.

// Everything about the loop's pacing and its scene. Pacing fields are read once
// at initialize() and changing them afterwards has no effect until the loop is
// re-initialized; scene fields likewise.
struct FrameLoopConfig {
    // -- pacing ------------------------------------------------------------
    // Physics step. The only timestep the solver ever sees.
    double fixedDelta = 1.0 / 60.0;
    // Ceiling on one frame's real delta. A debugger break, a level load or a
    // stalled compositor can hand the loop a delta of seconds; without this
    // the accumulator would try to catch up with thousands of steps and never
    // return. The excess is discarded, which loses simulation time but keeps
    // the loop responsive.
    double maxFrameDelta = 0.25;
    // Ceiling on steps in a single frame. Second half of the same guard, for
    // the case where maxFrameDelta is generous on a very slow machine.
    int maxSubSteps = 8;

    // -- scene -------------------------------------------------------------
    float groundTopY = 0.0f;         // top face of the static ground
    // Half-size of the character body. The player is the dynamic body that
    // JoltPhysicsWorld::initialize() seeds, so this must match the 0.5 that
    // world creates; it is configurable so the same value drives the ground
    // test and a caller that built its own body.
    float playerHalfExtent = 0.5f;
    float playerSpawnY = 2.0f;
    // A body counts as grounded within this distance of the ground surface.
    // Jolt's contact solver leaves a small penetration at rest, so an exact
    // equality test would report the player as airborne while standing still.
    float groundTolerance = 0.12f;
    float moveSpeed = 7.0f;          // horizontal speed while a move axis is held
    float jumpSpeed = 6.0f;          // upward velocity applied on a jump
    uint32_t propCount = 24;         // static blocks scattered around the player
    // Inner and outer radius of the prop ring. The inner radius keeps the
    // spawn area clear, the same rule a level generator applies around a
    // player start: props scattered from the origin itself wall the character
    // in on the first frame, which looks like a physics bug and is not one.
    // The outer radius keeps every prop within the default camera's range.
    float propInnerRadius = 12.0f;
    float propOuterRadius = 28.0f;
    uint32_t propSeed = 0x9e3779b9u; // fixed seed: the scene must be identical every run
    // Playback rate applied on top of the clip's own speed while the player is
    // moving. A rig running at 1.0x while sprinting looks wrong.
    float locomotionSpeedScale = 0.5f;
};

// Held axes, not events. The loop reads the current value on every physics
// substep, which is what makes a fixed-step simulation reproducible: the same
// delta sequence and the same input sequence always produce the same state.
// An edge-triggered event would instead have to be latched and cleared by
// exactly one substep, and a frame with three substeps would jump three times.
struct InputState {
    float moveX = 0.0f;  // strafe, -1..1
    float moveZ = 0.0f;  // forward, -1..1
    bool jump = false;   // edge detected by the loop against the previous frame

    void clear() noexcept {
        moveX = 0.0f;
        moveZ = 0.0f;
        jump = false;
    }
    // Length of the movement vector, before clamping. The loop scales a
    // diagonal so that moveX=moveZ=1 is not faster than moveZ=1 alone.
    float magnitude() const noexcept;
};

// What one frame did. Every field here is a fact about the frame that just
// happened, not an estimate, because every one of them is computed from the
// numbers the loop actually used.
struct FrameStats {
    uint64_t frameIndex = 0;
    double rawDelta = 0.0;   // real delta as measured, before any clamping
    double delta = 0.0;      // real delta after clamping; what the frame used
    int steps = 0;           // physics steps taken this frame
    double alpha = 0.0;      // [0,1) position between the last two physics states
    double simulatedSeconds = 0.0;  // total simulated time since initialize()
    bool clamped = false;    // rawDelta exceeded maxFrameDelta
    bool stepsDropped = false;  // the substep cap hit and the backlog was discarded
    double physicsMs = 0.0;
    double animationMs = 0.0;
    double totalMs = 0.0;
};

// One axis-aligned box in the drawable scene.
//
// The loop is the authority on what is in the world, so it is also the
// authority on what gets drawn. A renderer that went back to the physics world
// to enumerate bodies would be reading simulation state from a second place,
// and the two would disagree on the first frame where they were out of step.
struct SceneBox {
    float center[3] = {0, 0, 0};
    float halfExtent[3] = {0, 0, 0};
    uint32_t color = 0;  // 0xAABBGGRR
};

// The contract between the loop and whatever draws it.
//
// This is a borrowed view, not a copy: `jointMatrices` points into storage the
// loop owns and stays valid only until the next tick(). A renderer submits
// from it during drawFrame(), which is the normal contract, and copying 10
// joints per frame when nobody asked for it is not.
struct FrameState {
    uint64_t frameIndex = 0;
    double alpha = 0.0;
    double realDelta = 0.0;  // clamped delta the frame was drawn with
    int physicsSteps = 0;

    // Interpolated player transform: where to draw, which is between two
    // simulation states rather than equal to either.
    float playerPosition[3] = {0, 0, 0};
    // The two simulation states the interpolation was taken from. A renderer
    // that wants velocity for its own effects can use the difference rather
    // than differencing positions itself.
    float playerPrevious[3] = {0, 0, 0};
    float playerNext[3] = {0, 0, 0};
    float playerVelocity[3] = {0, 0, 0};
    bool grounded = false;

    // Model-space joint matrices from the resolved ozz pose, jointCount *
    // 16 floats, column-major. nullptr until the first tick completes.
    const float *jointMatrices = nullptr;
    int jointCount = 0;

    // Everything else in the world, as drawable boxes. Entry 0 is always the
    // player, at the interpolated position, so a renderer can sort or
    // highlight it without special-casing.
    const SceneBox *sceneBoxes = nullptr;
    int sceneBoxCount = 0;

    // A point a camera can follow, raised to head height. Deriving it here
    // means the camera and the character cannot disagree about where the
    // character is.
    float cameraTarget[3] = {0, 0, 0};
};

// Rejects a configuration the loop cannot honour, and says why. Checked before
// anything is built: a loop with fixedDelta=0 would divide by zero every frame,
// and maxSubSteps=0 would never simulate.
bool validateFrameLoopConfig(const FrameLoopConfig &config, std::string &error);

class FrameLoop {
public:
    FrameLoop();
    ~FrameLoop();
    FrameLoop(FrameLoop&&) noexcept;
    FrameLoop &operator=(FrameLoop&&) noexcept;
    FrameLoop(const FrameLoop &) = delete;
    FrameLoop &operator=(const FrameLoop &) = delete;

    /**
     * Builds the scene and bakes the animation. Idempotent.
     *
     * Returns false, with lastError() set, if the configuration is invalid,
     * the physics world will not come up, the rig will not bake, or the seeded
     * player body is missing. A loop that half-built is worse than one that
     * refused, so nothing is left half-built.
     */
    bool initialize(const FrameLoopConfig &config = FrameLoopConfig{});
    void shutdown() noexcept;
    bool ready() const noexcept;

    const FrameLoopConfig &config() const noexcept;
    const JoltPhysicsWorld &physics() const noexcept;
    const AnimationLibrary &animation() const noexcept;
    // Playback state of the character. Borrowed; the animator lives as long as
    // the loop does. Exposed because "is the rig actually playing" is a
    // question worth being able to ask from outside.
    const Animator &animator() const noexcept;
    const std::string &lastError() const noexcept;

    /**
     * Advance one real-time frame.
     *
     * The whole of the frame happens here, in this order: drain the
     * accumulator into fixed physics steps, then advance animation once by the
     * real delta, then resolve the pose, then interpolate and hand the result
     * to `out`. A renderer therefore never observes a half-stepped world: it
     * runs after the accumulator loop has finished, and reads a snapshot.
     *
     * @param realDeltaSeconds Seconds since the previous tick. A negative or
     *        non-finite value is treated as zero rather than propagated; a bad
     *        clock must not be able to poison the accumulator.
     * @return false only if the loop is not initialized. A frame that ran with
     *         zero physics steps is a success, not a failure.
     */
    bool tick(double realDeltaSeconds, const InputState &input, FrameState &out);

    /**
     * Run `frames` frames of exactly `delta` each, ignoring the wall clock.
     * This is the deterministic entry point: the same arguments always produce
     * the same state, which is what makes the loop testable and what makes a
     * recorded input stream replayable. Returns the number of frames run.
     */
    uint32_t runDeterministic(uint32_t frames, double delta, const InputState &input, FrameState &out);

    // Selects a clip by name and stops locomotion from overriding it on the
    // next frame. Returns false for an unknown clip or an unbound loop.
    bool playClip(std::string_view clip, bool loop = true);
    // Hands the clip choice back to the speed-driven state machine.
    void resumeLocomotion();
    std::string_view currentClip() const noexcept;
    // Horizontal speed in m/s, from the last simulated state.
    float playerSpeed() const noexcept;
    bool grounded() const noexcept;

    const FrameStats &stats() const noexcept;
    // The most recent frame handed to a renderer. Retained so a caller can
    // inspect the result of a run after the fact; valid until the next tick.
    const FrameState &lastFrame() const noexcept;
    uint64_t frameIndex() const noexcept;
    int jointCount() const noexcept;
    int bodyCount() const noexcept;
    // Physics steps the loop would take to cover `seconds` of accumulated time.
    int stepsForSeconds(double seconds) const noexcept;
    // Handle of the character body, for callers that want to read it directly.
    int playerBody() const noexcept;

    /**
     * Re-centre the scene: the player back at its spawn, velocity zeroed, time
     * and the accumulator cleared, animation rewound, the frame index reset.
     * The configuration and the baked rig survive. Used between test cases and
     * by a level restart.
     */
    void reset() noexcept;

    /**
     * Move the spawn point, and put the character there.
     *
     * Exists because the spawn was hard-coded to the origin, and the generated
     * world is not centred on the origin: for a typical seed the built-up part
     * is a kilometre or more away. A player who starts in an empty field has to
     * walk to the city before seeing any of it, and a streaming slice built
     * around the origin streams nothing at all.
     *
     * `y` is ignored and the ground height under the spawn is used, because a
     * fixed height puts the player inside a hill on any terrain that is not
     * flat, and the physics resolve is not a substitute for spawning correctly.
     * Safe to call before or after initialize(); before, it is applied at
     * initialize, after, it moves the character immediately.
     */
    bool setSpawn(double x, double z, double groundY);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace emergent
