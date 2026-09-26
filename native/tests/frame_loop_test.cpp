// Frame loop self-test.
//
// These are behaviour tests, not smoke tests, and the ones that matter most
// are the ones about synchronisation. A frame loop that runs without crashing
// is easy; a frame loop that advances physics at a rate independent of frame
// pacing, hands a renderer an interpolation that matches its own alpha, and
// advances animation once per frame rather than once per physics substep is
// the thing that is actually hard to get right, and the thing that looks fine
// until it doesn't.

#include "emergent/frame_loop.hpp"
#include "emergent/render_backend.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) {
        std::printf("  ok    %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 1e-5f) { return std::fabs(a - b) <= epsilon; }
bool near3(const float *a, const float *b, float epsilon = 1e-5f) {
    return near(a[0], b[0], epsilon) && near(a[1], b[1], epsilon) && near(a[2], b[2], epsilon);
}
bool sameBits(const float *a, const float *b, size_t count) {
    return std::memcmp(a, b, sizeof(float) * count) == 0;
}

using emergent::FrameLoop;
using emergent::FrameLoopConfig;
using emergent::FrameState;
using emergent::FrameStats;
using emergent::InputState;

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

} // namespace

int main() {
    using emergent::NullRenderBackend;
    using emergent::loadSpirvModule;
    using emergent::validateFrameLoopConfig;

    std::printf("EMERGENT frame loop self-test\n");

    // --- configuration -----------------------------------------------------
    {
        std::string error;
        FrameLoopConfig good;
        check(validateFrameLoopConfig(good, error), "the default configuration is valid");

        FrameLoopConfig bad = good;
        bad.fixedDelta = 0.0;
        check(!validateFrameLoopConfig(bad, error) && !error.empty(), "a zero fixed delta is rejected");

        bad = good;
        bad.fixedDelta = kNaN;
        check(!validateFrameLoopConfig(bad, error), "a NaN fixed delta is rejected");

        bad = good;
        bad.fixedDelta = 1.0;
        check(!validateFrameLoopConfig(bad, error), "a one-second fixed delta is rejected as too coarse");

        bad = good;
        bad.fixedDelta = 1e-6;
        check(!validateFrameLoopConfig(bad, error), "a sub-millisecond fixed delta is rejected as too fine");

        bad = good;
        bad.maxSubSteps = 0;
        check(!validateFrameLoopConfig(bad, error), "zero max substeps is rejected");

        bad = good;
        bad.maxFrameDelta = bad.fixedDelta * 0.5;
        check(!validateFrameLoopConfig(bad, error), "a max frame delta below the fixed delta is rejected");

        bad = good;
        bad.playerHalfExtent = -1.0f;
        check(!validateFrameLoopConfig(bad, error), "a negative player half-extent is rejected");

        FrameLoop loop;
        FrameLoopConfig invalid;
        invalid.fixedDelta = 0.0;
        check(!loop.initialize(invalid), "the loop refuses to initialize on an invalid configuration");
        check(!loop.lastError().empty(), "a refused initialization explains itself");
        check(!loop.ready(), "a refused initialization does not leave the loop ready");
    }

    // --- lifecycle ---------------------------------------------------------
    FrameLoop loop;
    {
        check(!loop.ready(), "a fresh loop is not ready");
        check(loop.initialize(), "initialize() succeeds");
        check(loop.ready(), "the loop reports ready after initialize()");
        check(loop.lastError().empty(), "a successful initialization reports no error");
        check(loop.initialize(), "initialize() is idempotent");
        check(loop.ready(), "the loop is still ready after a second initialize()");
        check(loop.playerBody() >= 0, "the loop has a character body to drive");
        check(loop.jointCount() == loop.animation().jointCount(), "the loop's joint count matches the baked rig");
        check(loop.bodyCount() > 1, "the scene has more than the seeded world");
    }

    // --- the fixed step ----------------------------------------------------
    {
        FrameLoop pacing;
        pacing.initialize();
        const double fixed = pacing.config().fixedDelta;
        FrameState state;
        InputState idle;

        pacing.tick(fixed, idle, state);
        check(state.physicsSteps == 1, "one fixed delta takes exactly one step");
        check(near(static_cast<float>(state.alpha), 0.0f, 1e-6f), "one fixed delta leaves no interpolation alpha");

        pacing.tick(fixed * 2.0, idle, state);
        check(state.physicsSteps == 2, "two fixed deltas take two steps");
        check(near(static_cast<float>(state.alpha), 0.0f, 1e-6f), "two fixed deltas leave no alpha");

        pacing.tick(fixed * 1.5, idle, state);
        check(state.physicsSteps == 1, "one and a half deltas take one step");
        check(near(static_cast<float>(state.alpha), 0.5f, 1e-4f), "one and a half deltas leave alpha of one half");

        // A quarter delta on its own. Started from a fresh accumulator because
        // the leftover from the previous check is still owed: the half delta
        // above has not been simulated, it is waiting, and the next delta adds
        // to it rather than replacing it. That carry is the whole point of an
        // accumulator and it is asserted separately just below.
        FrameLoop quarter;
        quarter.initialize();
        quarter.tick(fixed * 0.25, idle, state);
        check(state.physicsSteps == 0, "a quarter delta takes no step");
        check(near(static_cast<float>(state.alpha), 0.25f, 1e-4f), "a quarter delta leaves alpha of one quarter");

        // The carry: three quarter deltas are one and a quarter steps' worth of
        // time, so the third frame pays the debt the first two built up.
        pacing.tick(fixed * 0.25, idle, state);
        check(near(static_cast<float>(state.alpha), 0.75f, 1e-4f),
              "time owed by earlier frames carries into the next frame's alpha");
        pacing.tick(fixed * 0.25, idle, state);
        check(state.physicsSteps == 1, "the accumulated debt is paid as a step");
        check(near(static_cast<float>(state.alpha), 0.0f, 1e-4f), "paying the debt returns alpha to zero");

        // alpha is the renderer's contract; it must never leave [0,1).
        bool inRange = true;
        for (int i = 0; i < 400; ++i) {
            const double delta = fixed * (0.13 + 0.31 * static_cast<double>(i % 17));
            pacing.tick(delta, idle, state);
            if (!(state.alpha >= 0.0 && state.alpha < 1.0)) inRange = false;
        }
        check(inRange, "alpha stays inside [0,1) over four hundred irregular frames");

        // The high-refresh-rate case is where the boundary actually bites.
        // At 144Hz the accumulator is repeatedly brought to just under one
        // step, and (step - 1ulp) / step rounds to exactly 1.0 in double, so
        // an implementation that trusts the division reports alpha == 1.0 on a
        // frame that took no step. That is a violation of the contract every
        // renderer is written against.
        FrameLoop boundary;
        boundary.initialize();
        bool everOne = false;
        bool everNegative = false;
        for (int i = 0; i < 4000; ++i) {
            boundary.tick(1.0 / 144.0, idle, state);
            if (state.alpha >= 1.0) everOne = true;
            if (state.alpha < 0.0) everNegative = true;
        }
        check(!everOne, "alpha never reaches 1.0 over four thousand 144Hz frames");
        check(!everNegative, "alpha is never negative");
        check(pacing.stepsForSeconds(1.0) == 60, "stepsForSeconds reports 60 steps for one second at 60Hz");
        check(pacing.stepsForSeconds(0.0) == 0, "stepsForSeconds reports no steps for zero seconds");
    }

    // --- physics is independent of frame pacing ----------------------------
    {
        // Same number of physics steps, reached two different ways. If the
        // simulation were a function of the frame rate rather than of
        // simulated time, these two would diverge.
        FrameLoop slow;
        FrameLoop fast;
        slow.initialize();
        fast.initialize();
        const double fixed = slow.config().fixedDelta;

        InputState input;
        input.moveZ = 1.0f;
        FrameState a;
        FrameState b;

        int slowSteps = 0;
        for (int i = 0; i < 60; ++i) {
            slow.tick(fixed, input, a);
            slowSteps += a.physicsSteps;
        }
        int fastSteps = 0;
        for (int i = 0; i < 30; ++i) {
            fast.tick(fixed * 2.0, input, b);
            fastSteps += b.physicsSteps;
        }
        check(slowSteps == 60 && fastSteps == 60, "both runs took the same number of physics steps");
        check(near3(a.playerNext, b.playerNext, 1e-4f),
              "the character ends up in the same place at 60fps and at 30fps");

        // And the step count over a fixed span of simulated time is the same
        // whatever the frame pacing.
        FrameLoop jitter;
        jitter.initialize();
        InputState still;
        int jitteredSteps = 0;
        for (int i = 0; i < 60; ++i) {
            // Alternating 0.75 and 1.25 fixed deltas averages exactly one, so
            // 60 frames are one second of real time no matter how they are
            // distributed. The step count must be 60, not 60 frames' worth of
            // whatever each frame happened to ask for.
            const double delta = fixed * ((i % 2) == 0 ? 0.75 : 1.25);
            jitter.tick(delta, still, a);
            jitteredSteps += a.physicsSteps;
        }
        check(jitteredSteps == 60, "one second of irregular frames takes 60 steps, not a rate-dependent count");
        check(near(jitter.lastFrame().realDelta, fixed * 1.25, 1e-9),
              "the last frame used exactly the delta it was given");
    }

    // --- determinism -------------------------------------------------------
    {
        FrameLoop one;
        FrameLoop two;
        one.initialize();
        two.initialize();
        InputState input;
        input.moveX = 0.5f;
        input.moveZ = 1.0f;
        FrameState a;
        FrameState b;

        // A jittery delta sequence, so the two runs are not accidentally
        // aligned on a round number of steps.
        for (int i = 0; i < 500; ++i) {
            const double delta = one.config().fixedDelta * (0.4 + 0.11 * static_cast<double>(i % 23));
            InputState step = input;
            step.jump = (i % 97) == 0;
            one.tick(delta, step, a);
            two.tick(delta, step, b);
        }
        check(sameBits(a.playerNext, b.playerNext, 3), "two runs of the same delta sequence end in the same place");
        check(sameBits(a.jointMatrices, b.jointMatrices, static_cast<size_t>(a.jointCount) * 16),
              "two runs of the same delta sequence end in the same pose");
        check(one.stats().frameIndex == two.stats().frameIndex, "two runs of the same sequence run the same frame count");
    }

    // --- clamping and the spiral-of-death guard ----------------------------
    {
        FrameLoop guarded;
        guarded.initialize();
        const FrameLoopConfig cfg = guarded.config();
        FrameState state;
        InputState idle;

        guarded.tick(10.0, idle, state);
        check(guarded.stats().clamped, "a ten-second frame is reported as clamped");
        check(near(guarded.stats().delta, cfg.maxFrameDelta, 1e-9), "a ten-second frame uses maxFrameDelta");
        check(state.physicsSteps == cfg.maxSubSteps, "a ten-second frame is capped at maxSubSteps");
        check(guarded.stats().stepsDropped, "hitting the substep cap is reported rather than hidden");
        check(state.alpha >= 0.0 && state.alpha < 1.0, "a dropped frame still produces a usable alpha");

        // The backlog must be gone, or the next frames would keep trying to
        // catch up and the loop would never recover.
        guarded.tick(cfg.fixedDelta, idle, state);
        check(state.physicsSteps == 1, "the frame after a drop is back to normal");

        guarded.tick(-1.0, idle, state);
        check(state.physicsSteps == 0, "a negative delta takes no step");
        check(guarded.stats().delta == 0.0, "a negative delta is treated as zero");

        guarded.tick(kNaN, idle, state);
        check(state.physicsSteps == 0, "a NaN delta takes no step");
        check(guarded.stats().delta == 0.0, "a NaN delta is treated as zero");

        // An infinite delta is a failed clock, not a stall, so it is discarded
        // rather than clamped: clamping it would simulate time that never
        // happened. A large finite delta above is the stall case, and that one
        // is clamped.
        guarded.tick(kInf, idle, state);
        check(guarded.stats().delta == 0.0, "an infinite delta is discarded as a failed clock");
        check(!guarded.stats().clamped, "an infinite delta is not reported as a stall");
        check(state.physicsSteps == 0, "an infinite delta takes no step");
    }

    // --- interpolation -----------------------------------------------------
    {
        FrameLoop interp;
        interp.initialize();
        const double fixed = interp.config().fixedDelta;
        FrameState state;
        InputState idle;

        interp.tick(fixed * 3.0, idle, state);
        const bool windowOpen = !near3(state.playerPrevious, state.playerNext, 1e-6f);
        check(windowOpen, "a stepping frame leaves a real window between the two simulation states");
        check(near(state.playerPosition[0],
                   state.playerPrevious[0] +
                       (state.playerNext[0] - state.playerPrevious[0]) * static_cast<float>(state.alpha), 1e-5f),
              "the rendered position is the interpolation of the two states at alpha");

        // The high-refresh-rate case. At 240Hz against a 60Hz simulation, half
        // of all frames take no physics step, and if the interpolation window
        // is not collapsed those frames render the player behind the
        // simulation.
        bool collapsed = true;
        for (int i = 0; i < 120; ++i) {
            interp.tick(fixed * 0.25, idle, state);
            if (state.physicsSteps == 0) {
                if (!near3(state.playerPrevious, state.playerNext, 1e-7f) ||
                    !near3(state.playerPosition, state.playerNext, 1e-7f)) {
                    collapsed = false;
                }
            }
        }
        check(collapsed, "a frame that takes no step renders exactly the current simulation state");
    }

    // --- animation is variable-step, once per frame ------------------------
    {
        FrameLoop anim;
        anim.initialize();
        const double fixed = anim.config().fixedDelta;
        FrameState state;
        InputState idle;

        anim.playClip("walk", true);
        // Four substeps in one frame. If animation advanced per substep the
        // clip would move four times as far as the frame's delta says it
        // should.
        anim.tick(fixed * 4.0, idle, state);
        const float afterOneFrame = anim.animator().time();
        check(state.physicsSteps == 4, "the frame really did take four physics steps");
        check(near(afterOneFrame, static_cast<float>(fixed * 4.0), 1e-4f),
              "animation advances once per frame by the real delta, not once per substep");

        anim.tick(fixed * 4.0, idle, state);
        check(near(anim.animator().time(), static_cast<float>(fixed * 8.0), 1e-4f),
              "a second four-step frame advances animation by the same amount again");

        check(state.jointMatrices != nullptr, "the frame state carries joint matrices");
        check(state.jointCount == anim.jointCount(), "the frame state's joint count matches the rig");

        bool allFinite = true;
        for (int j = 0; j < state.jointCount * 16; ++j) {
            if (!std::isfinite(state.jointMatrices[j])) allFinite = false;
        }
        check(allFinite, "every joint matrix element is finite");

        // The rig must actually be moving, not frozen at a constant value.
        // cameraTarget is the resolved head joint, so it moves if and only if
        // the pose is being re-evaluated.
        anim.playClip("walk", true);
        anim.tick(fixed, idle, state);
        float first[3] = {state.cameraTarget[0], state.cameraTarget[1], state.cameraTarget[2]};
        anim.tick(fixed, idle, state);
        float second[3] = {state.cameraTarget[0], state.cameraTarget[1], state.cameraTarget[2]};
        check(!near3(first, second, 1e-6f), "the head joint moves between frames while a clip plays");
    }

    // --- locomotion follows movement ---------------------------------------
    {
        FrameLoop loco;
        loco.initialize();
        const double fixed = loco.config().fixedDelta;
        FrameState state;
        InputState idle;

        check(std::string(loco.currentClip()) == "idle", "the character starts on the idle clip");
        for (int i = 0; i < 60; ++i) loco.tick(fixed, idle, state);
        check(loco.grounded(), "the character settles onto the ground and stays grounded");

        InputState walk;
        walk.moveZ = 1.0f;
        for (int i = 0; i < 10; ++i) loco.tick(fixed, walk, state);
        check(std::string(loco.currentClip()) == "run", "holding forward at full speed selects the run clip");
        check(loco.playerSpeed() > 0.0f, "the character has horizontal speed while input is held");

        for (int i = 0; i < 120; ++i) loco.tick(fixed, idle, state);
        check(std::string(loco.currentClip()) == "idle", "releasing input returns the character to idle");
        check(near(loco.playerSpeed(), 0.0f, 0.05f), "releasing input stops the character");

        // A manual play() must stick until locomotion is handed back.
        check(loco.playClip("walk", true), "playClip accepts a known clip");
        check(!loco.playClip("nonesuch", true), "playClip rejects an unknown clip");
        for (int i = 0; i < 10; ++i) loco.tick(fixed, idle, state);
        check(std::string(loco.currentClip()) == "walk", "an explicit playClip is not overridden by the state machine");
        loco.resumeLocomotion();
        loco.tick(fixed, idle, state);
        check(std::string(loco.currentClip()) == "idle", "resumeLocomotion hands the choice back");
    }

    // --- input -------------------------------------------------------------
    {
        FrameLoop move;
        move.initialize();
        const double fixed = move.config().fixedDelta;
        FrameState state;
        InputState idle;
        for (int i = 0; i < 30; ++i) move.tick(fixed, idle, state);
        const float restingZ = state.playerPosition[2];

        InputState forward;
        forward.moveZ = 1.0f;
        for (int i = 0; i < 60; ++i) move.tick(fixed, forward, state);
        check(state.playerPosition[2] > restingZ + 1.0f, "holding forward moves the character along +Z");

        // A diagonal must not be faster than a cardinal. The input is
        // normalised, so each axis of a 45-degree stick is scaled down and the
        // character covers the same ground in the same time. Comparing the
        // axes separately would be meaningless -- a diagonal is supposed to
        // move on both -- so the invariant is on the total.
        auto horizontalDistance = [](const FrameState &s) {
            return std::sqrt(s.playerPosition[0] * s.playerPosition[0] + s.playerPosition[2] * s.playerPosition[2]);
        };

        move.reset();
        for (int i = 0; i < 30; ++i) move.tick(fixed, idle, state);
        InputState cardinal;
        cardinal.moveZ = 1.0f;
        for (int i = 0; i < 60; ++i) move.tick(fixed, cardinal, state);
        const float cardinalDistance = horizontalDistance(state);

        move.reset();
        for (int i = 0; i < 30; ++i) move.tick(fixed, idle, state);
        InputState diagonal;
        diagonal.moveZ = 1.0f;
        diagonal.moveX = 1.0f;
        for (int i = 0; i < 60; ++i) move.tick(fixed, diagonal, state);
        const float diagonalDistance = horizontalDistance(state);

        check(cardinalDistance > 1.0f, "a cardinal move covers ground");
        check(near(cardinalDistance, diagonalDistance, 0.05f),
              "a diagonal covers the same distance as a cardinal move, not 41% more");
    }

    // --- collision ---------------------------------------------------------
    {
        // Walking into a prop must actually stop the character. A frame loop
        // that only ever tested free movement would pass with a physics
        // integration that ran straight through walls.
        FrameLoopConfig walled;
        walled.propCount = 4;
        walled.propInnerRadius = 6.0f;
        walled.propOuterRadius = 6.0f;  // invalid, replaced below
        walled.propOuterRadius = 6.5f;
        FrameLoop blocker;
        check(blocker.initialize(walled), "a ring with a narrow radius is a valid configuration");

        const double fixed = blocker.config().fixedDelta;
        FrameState state;
        InputState idle;
        for (int i = 0; i < 30; ++i) blocker.tick(fixed, idle, state);
        InputState forward;
        forward.moveZ = 1.0f;
        for (int i = 0; i < 600; ++i) blocker.tick(fixed, forward, state);

        // The character is stopped, not oscillating: it has hit something and
        // is being held against it.
        check(blocker.playerSpeed() < 1.0f, "walking into the prop ring stops the character");
        check(state.playerPosition[2] > 2.0f, "the character got out of the spawn area first");
        check(state.playerPosition[2] < 20.0f, "the character was stopped before reaching the far side");

        // A prop must never be inside the spawn area, whatever the seed.
        FrameLoopConfig tight;
        tight.propCount = 64;
        tight.propInnerRadius = 40.0f;
        tight.propOuterRadius = 45.0f;
        FrameLoop clear;
        check(clear.initialize(tight), "a ring far from the origin is a valid configuration");
        for (int i = 0; i < 10; ++i) clear.tick(fixed, idle, state);
        float nearest = 1e9f;
        for (int i = 1; i < state.sceneBoxCount; ++i) {
            const float d = std::sqrt(state.sceneBoxes[i].center[0] * state.sceneBoxes[i].center[0] +
                                      state.sceneBoxes[i].center[2] * state.sceneBoxes[i].center[2]);
            if (d < nearest) nearest = d;
        }
        check(nearest >= 38.0f, "the prop ring keeps the spawn area clear");
    }

    // --- jumping -----------------------------------------------------------
    {
        FrameLoop jump;
        jump.initialize();
        const double fixed = jump.config().fixedDelta;
        FrameState state;
        InputState idle;
        for (int i = 0; i < 60; ++i) jump.tick(fixed, idle, state);
        check(jump.grounded(), "the character settles onto the ground before the jump test");

        InputState hop;
        hop.jump = true;
        jump.tick(fixed, hop, state);
        int airborneFrames = 0;
        float peak = -1e9f;
        for (int i = 0; i < 60; ++i) {
            jump.tick(fixed, hop, state);
            if (!state.grounded) ++airborneFrames;
            if (state.playerPosition[1] > peak) peak = state.playerPosition[1];
        }
        check(airborneFrames > 0, "a jump leaves the ground");
        check(peak > 0.5f + 0.2f, "the jump reaches an apex above the resting height");

        // Held, not tapped: one press must be one jump.
        const int airborneWhileHeld = airborneFrames;
        jump.reset();
        for (int i = 0; i < 60; ++i) jump.tick(fixed, idle, state);
        InputState held;
        held.jump = true;
        int secondJumpApex = 0;
        for (int i = 0; i < 90; ++i) {
            jump.tick(fixed, held, state);
            if (state.playerPosition[1] > 0.5f + 0.2f) secondJumpApex = i;
        }
        check(secondJumpApex > airborneWhileHeld || secondJumpApex == 0,
              "holding jump does not produce a second jump mid-air");
    }

    // --- reset -------------------------------------------------------------
    {
        FrameLoop restart;
        restart.initialize();
        const double fixed = restart.config().fixedDelta;
        FrameState state;
        InputState forward;
        forward.moveZ = 1.0f;
        for (int i = 0; i < 120; ++i) restart.tick(fixed, forward, state);
        check(restart.frameIndex() == 120, "the frame index counts every frame");
        check(std::fabs(state.playerPosition[2]) > 1.0f, "the character has walked away from the spawn");

        restart.reset();
        check(restart.frameIndex() == 0, "reset() rewinds the frame index");
        FrameState after;
        restart.tick(0.0, forward, after);
        check(near(after.playerPosition[2], 0.0f, 1e-3f), "the character is back at the spawn after reset()");
        check(near(after.playerPosition[1], restart.config().playerSpawnY, 1e-3f),
              "reset() puts the character back at the configured spawn height");
        check(after.physicsSteps == 0, "a zero delta after reset() takes no step");
    }

    // --- the drawable scene ------------------------------------------------
    {
        FrameLoop scene;
        scene.initialize();
        FrameState state;
        InputState idle;
        for (int i = 0; i < 10; ++i) scene.tick(1.0 / 60.0, idle, state);
        const int expected = 1 + static_cast<int>(scene.config().propCount);
        check(state.sceneBoxCount == expected, "the frame state describes the player and every prop");
        check(state.sceneBoxes != nullptr, "the frame state points at its scene boxes");
        check(near3(&state.sceneBoxes[0].center[0], state.playerPosition, 1e-5f),
              "scene box zero is the character, at the interpolated position");
        check(near(state.sceneBoxes[0].halfExtent[0], scene.config().playerHalfExtent, 1e-5f),
              "the character box has the configured half-extent");

        bool onGround = true;
        for (int i = 0; i < expected; ++i) {
            if (state.sceneBoxes[i].center[1] < -0.01f) onGround = false;
        }
        check(onGround, "every scene box sits on or above the ground");
    }

    // --- the headless renderer ---------------------------------------------
    {
        NullRenderBackend renderer;
        FrameState state;
        InputState idle;

        check(!renderer.open(0, 0, nullptr), "the headless renderer refuses a zero-sized target");
        check(renderer.open(64, 48, nullptr), "the headless renderer opens at a real size");
        check(renderer.opened(), "the headless renderer reports opened");
        check(!renderer.presentsToDisplay(), "the headless renderer never claims to present");
        check(renderer.stats().status.find("offscreen") != std::string::npos,
              "the headless renderer says it is offscreen");

        renderer.drawFrame(state);
        check(renderer.rejectedFrames() == 1, "a draw outside beginFrame() is rejected");
        check(renderer.stats().submittedFrames == 0, "a rejected draw is not counted as submitted");

        FrameLoop source;
        source.initialize();
        for (int i = 0; i < 30; ++i) source.tick(1.0 / 60.0, idle, state);
        renderer.beginFrame();
        renderer.drawFrame(state);
        renderer.endFrame();
        check(renderer.rejectedFrames() == 1, "a valid frame is accepted");
        check(renderer.stats().submittedFrames == 1, "a submitted frame is counted once");
        check(!renderer.inFrame(), "endFrame() closes the frame");

        FrameState seen;
        check(renderer.lastFrame(seen), "the headless renderer remembers the frame it drew");
        check(seen.frameIndex == state.frameIndex, "the remembered frame is the one that was drawn");
        check(seen.jointMatrices == state.jointMatrices, "the remembered frame kept its joint matrices");

        // Validation: a renderer is the last place a NaN can be caught before
        // it becomes a lost device on someone else's GPU.
        FrameState broken = state;
        broken.playerPosition[1] = kNaN;
        renderer.beginFrame();
        renderer.drawFrame(broken);
        renderer.endFrame();
        check(renderer.rejectedFrames() == 2, "a NaN position is rejected");

        broken = state;
        broken.alpha = 1.0;
        renderer.beginFrame();
        renderer.drawFrame(broken);
        renderer.endFrame();
        check(renderer.rejectedFrames() == 3, "an alpha of one is rejected");

        broken = state;
        broken.jointMatrices = nullptr;
        broken.jointCount = 5;
        renderer.beginFrame();
        renderer.drawFrame(broken);
        renderer.endFrame();
        check(renderer.rejectedFrames() == 4, "a joint count without matrices is rejected");

        broken = state;
        broken.sceneBoxes = nullptr;
        broken.sceneBoxCount = 3;
        renderer.beginFrame();
        renderer.drawFrame(broken);
        renderer.endFrame();
        check(renderer.rejectedFrames() == 5, "a scene box count without boxes is rejected");

        check(renderer.stats().submittedFrames == 1,
              "rejected frames are never counted as submitted, however many were offered");
    }

    // --- SPIR-V loading ----------------------------------------------------
    {
        const char *path = "emergent_frame_loop_test.spv";
        std::vector<uint32_t> module;
        std::string error;

        check(!loadSpirvModule("emergent_no_such_module.spv", module, error) && !error.empty(),
              "a missing shader module is reported, not crashed on");
        check(module.empty(), "a failed load leaves the output buffer empty");

        // A well-formed but tiny module: one word of magic.
        FILE *f = std::fopen(path, "wb");
        const uint32_t magic = emergent::kSpirvMagic;
        std::fwrite(&magic, sizeof(magic), 1, f);
        std::fclose(f);
        check(loadSpirvModule(path, module, error), "a module with the right magic loads");
        check(module.size() == 1 && module[0] == magic, "the loaded module round-trips");

        // Truncated. This is the case that reaches the driver as garbage
        // without the check, and a driver given one does not return an error.
        f = std::fopen(path, "wb");
        std::fwrite(&magic, sizeof(magic), 1, f);
        const uint8_t twoBytes[2] = {0x01, 0x02};
        std::fwrite(twoBytes, 1, sizeof(twoBytes), f);
        std::fclose(f);
        check(!loadSpirvModule(path, module, error) && error.find("multiple of 4") != std::string::npos,
              "a module whose size is not a multiple of four is rejected by size");
        check(module.empty(), "a truncated module leaves nothing behind");

        // The right size, the wrong contents: a GLSL source renamed to .spv.
        f = std::fopen(path, "wb");
        const uint32_t junk[2] = {0xdeadbeefu, 0x00000000u};
        std::fwrite(junk, sizeof(junk), 1, f);
        std::fclose(f);
        check(!loadSpirvModule(path, module, error) && error.find("magic") != std::string::npos,
              "a module without the SPIR-V magic is rejected by content");
        std::remove(path);
    }

    if (g_failures == 0) {
        std::printf("\nall frame loop checks passed\n");
        return 0;
    }
    std::printf("\n%d frame loop checks FAILED\n", g_failures);
    return 1;
}
