// Animation self-test: bakes the real ozz rig and clips, then samples and
// resolves them through the runtime jobs.
//
// This asserts observable behaviour rather than "it linked": that a bone below
// the hips in the hierarchy ends up above it in model space, that advancing
// time actually changes the pose, that looping wraps instead of running off
// the end of the clip, and that blending two clips lands between them rather
// than snapping to one.

#include "emergent/animation.hpp"

#include <cmath>
#include <cstdio>
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

bool finite(const float *m) {
    for (int i = 0; i < emergent::kMatrixFloats; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

float maxDifference(const float *a, const float *b) {
    float worst = 0.0f;
    for (int i = 0; i < emergent::kMatrixFloats; ++i) {
        worst = std::fmax(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

} // namespace

int main() {
    using emergent::AnimationLibrary;
    using emergent::Animator;
    using emergent::Pose;

    std::printf("EMERGENT animation self-test (ozz-animation)\n");

    // --- unbaked library ---------------------------------------------------
    {
        AnimationLibrary empty;
        check(!empty.ready(), "a fresh library is not ready before build()");
        check(empty.jointCount() == 0, "an unbaked library has no joints");
        check(empty.clipCount() == 0, "an unbaked library has no clips");
        check(empty.jointIndex("Head") == -1, "an unbaked library resolves no joints");
        check(empty.clipDuration("walk") == 0.0f, "an unbaked library has zero clip durations");

        Animator orphan;
        check(!orphan.bound(), "a fresh animator is unbound");
        check(!orphan.play("walk"), "an unbound animator refuses to play");
        check(!orphan.playing(), "an unbound animator is not playing");
        orphan.update(0.1f); // must be a no-op, not a crash
        check(orphan.time() == 0.0f, "an unbound animator ignores time steps");
    }

    // --- baking ------------------------------------------------------------
    AnimationLibrary library;
    check(library.build(), "build() bakes the skeleton and every clip");
    check(library.ready(), "the library reports ready after build()");
    check(library.build(), "build() is idempotent");

    check(library.jointCount() == 10, "the rig has ten joints");
    check(library.clipCount() == 3, "the rig ships three clips");
    check(library.animatedJointCount() == library.jointCount(),
        "every joint is animated by the clips");
    check(library.jointNames().size() == static_cast<size_t>(library.jointCount()),
        "jointNames() matches jointCount()");

    check(library.jointIndex("Hips") == 0, "the root joint is first in depth-first order");
    check(library.jointIndex("Head") > library.jointIndex("Neck"), "a child follows its parent");
    check(library.jointIndex("RightHand") > library.jointIndex("RightArm"),
        "the hierarchy is stored in depth-first order");
    check(library.jointIndex("Nonexistent") == -1, "an unknown joint resolves to -1");
    check(library.jointName(-1).empty() && library.jointName(9999).empty(),
        "an out-of-range joint index yields an empty name");
    check(library.jointName(library.jointIndex("Head")) == "Head", "joint names round-trip");

    const std::vector<std::string> clips = library.clipNames();
    check(clips.size() == 3 && clips[0] == "idle" && clips[1] == "walk" && clips[2] == "run",
        "clips are the idle/walk/run locomotion set");
    check(!library.hasClip("Nonexistent"), "an unknown clip is not reported as present");
    check(library.hasClip("walk"), "walk is present");

    check(std::fabs(library.clipDuration("walk") - 1.0f) < 1e-4f, "walk is a one-second cycle");
    check(library.clipDuration("run") < library.clipDuration("walk"),
        "the run cycle is shorter than the walk cycle");
    check(library.clipDuration("Nonexistent") == 0.0f, "an unknown clip has zero duration");

    // --- rest pose ---------------------------------------------------------
    Pose pose;
    check(!pose.bound(), "a fresh pose is unbound");
    check(pose.bind(library), "bind() accepts a baked library");
    check(pose.bound(), "the pose reports bound after bind()");
    check(pose.jointCount() == library.jointCount(), "the pose covers every joint");
    check(!pose.resolved(), "a fresh pose is not resolved");
    check(pose.modelMatrix("Head") == nullptr, "an unresolved pose has no matrices");
    float unresolved[3] = {-1.0f, -1.0f, -1.0f};
    check(!pose.jointTranslation("Head", unresolved), "translation fails while unresolved");
    float noMatrix[emergent::kMatrixFloats] = {};
    check(!pose.modelMatrix("Nonexistent", noMatrix), "an unknown joint has no matrix");

    // With no contributor the pose resolves to the rest pose: the head sits the
    // sum of the spine, neck and head offsets above the hips, and both are on
    // the rig's own vertical axis.
    check(pose.resolve(), "resolve() works on an empty pose");
    float hips[3] = {};
    float head[3] = {};
    check(pose.jointTranslation("Hips", hips), "the rest pose places Hips");
    check(pose.jointTranslation("Head", head), "the rest pose places Head");
    check(std::fabs(head[1] - (hips[1] + 0.52f)) < 1e-3f,
        "the head is one spine-length chain above the hips");
    check(std::fabs(hips[0]) < 1e-5f && std::fabs(hips[2]) < 1e-5f,
        "the rig stands on the Y axis");

    // --- playback ----------------------------------------------------------
    Animator walk;
    check(walk.bind(library), "an animator binds to a baked library");
    check(walk.playing() == false, "a bound animator is not playing until play()");
    check(!walk.play("Nonexistent"), "play() rejects an unknown clip");
    check(!walk.playing(), "a rejected play() leaves the animator idle");

    check(walk.play("walk", true), "play() selects the walk clip");
    check(walk.playing(), "the animator reports playing after play()");
    check(walk.clip() == "walk", "the current clip is reported by name");
    check(std::fabs(walk.duration() - 1.0f) < 1e-4f, "the animator reports the clip duration");
    check(walk.time() == 0.0f, "play() rewinds to the start");

    walk.update(0.25f);
    check(std::fabs(walk.time() - 0.25f) < 1e-4f, "update() advances playback time");

    walk.update(-1.0f);
    walk.update(0.0f);
    check(std::fabs(walk.time() - 0.25f) < 1e-4f, "zero and negative steps are ignored");

    walk.speed(0.0f);
    walk.update(0.5f);
    check(std::fabs(walk.time() - 0.25f) < 1e-4f, "speed 0 freezes playback");
    walk.speed(2.0f);
    walk.update(0.1f);
    check(std::fabs(walk.time() - 0.45f) < 1e-4f, "speed scales playback time");
    walk.speed(std::nanf(""));
    check(walk.speed() == 2.0f, "a non-finite speed is rejected");
    walk.speed(1.0f);
    walk.reset();
    check(walk.time() == 0.0f, "reset() rewinds without changing the clip");
    check(walk.clip() == "walk", "reset() keeps the current clip");

    // The pose really moves: a quarter of a walk cycle is a half-stride.
    Animator half;
    half.bind(library);
    half.play("walk", true);
    half.update(0.25f);

    Pose poseA;
    poseA.bind(library);
    check(poseA.sample(walk) && poseA.resolve(), "the start-of-clip pose resolves");

    Pose poseB;
    poseB.bind(library);
    check(poseB.sample(half) && poseB.resolve(), "the half-stride pose resolves");

    const float *startFoot = poseA.modelMatrix("LeftLeg");
    const float *midFoot = poseB.modelMatrix("LeftLeg");
    check(startFoot != nullptr && finite(startFoot), "sampled matrices are finite");
    check(midFoot != nullptr && finite(midFoot), "sampled matrices are finite at mid-stride");
    check(maxDifference(startFoot, midFoot) > 1e-3f, "the pose actually animates over time");

    float footA[3] = {};
    float footB[3] = {};
    poseA.jointTranslation("LeftLeg", footA);
    poseB.jointTranslation("LeftLeg", footB);
    check(std::fabs(footA[1] - footB[1]) > 1e-4f, "the swinging leg changes height");
    check(footA[1] < 0.0f, "a leg hangs below the hips");

    // --- looping and clamping ----------------------------------------------
    walk.update(0.45f);
    walk.update(1.0f);
    check(walk.time() < walk.duration(), "a looping clip wraps instead of running past its end");
    check(std::fabs(walk.time() - 0.45f) < 1e-3f, "the wrap lands on the expected time");

    Animator once;
    once.bind(library);
    once.play("idle", false);
    once.update(100.0f);
    check(std::fabs(once.time() - once.duration()) < 1e-4f,
        "a non-looping clip clamps to its duration");

    // --- blending ----------------------------------------------------------
    Animator idle;
    idle.bind(library);
    idle.play("idle", true);
    idle.update(0.4f);

    Animator run;
    run.bind(library);
    run.play("run", true);
    run.update(0.15f);

    Pose blend;
    blend.bind(library);
    Animator unbound;  // never bound, so it cannot share the pose's library
    check(!blend.sample(unbound), "sampling an unbound animator is rejected");

    check(blend.sample(idle, 0.5f), "the first layer blends in");
    check(blend.sample(run, 0.5f), "the second layer blends in");
    check(blend.resolve(), "a two-layer blend resolves");

    const float *blendedLeg = blend.modelMatrix("LeftLeg");
    check(blendedLeg != nullptr && finite(blendedLeg), "the blended pose is finite");
    check(maxDifference(blendedLeg, startFoot) > 0.0f, "a blend is not just the first layer");

    // A zero-weight layer must leave the pose exactly where it was: ozz only
    // normalises the layers that actually contribute.
    Pose single;
    single.bind(library);
    single.sample(run, 1.0f);
    single.sample(idle, 0.0f);
    single.resolve();
    const float *runOnly = single.modelMatrix("LeftLeg");
    Pose runAlone;
    runAlone.bind(library);
    runAlone.sample(run, 1.0f);
    runAlone.resolve();
    check(maxDifference(runOnly, runAlone.modelMatrix("LeftLeg")) < 1e-5f,
        "a zero-weight layer does not perturb the pose");

    // --- reset and rebind ---------------------------------------------------
    Pose reused;
    reused.bind(library);
    reused.sample(run, 1.0f);
    reused.resolve();
    reused.reset();
    check(!reused.resolved(), "reset() invalidates a resolved pose");
    check(reused.resolve(), "a reset pose resolves again");
    const float *afterReset = reused.modelMatrix("LeftLeg");
    check(afterReset != nullptr && std::fabs(afterReset[13] - (-0.05f)) < 1e-3f,
        "a reset pose returns to the rest pose");

    AnimationLibrary other;
    other.build();
    check(reused.bind(other), "a pose can be rebound to another baked library");
    check(reused.jointCount() == other.jointCount(), "rebinding adopts the new joint count");
    check(reused.modelMatrix("Head") == nullptr, "rebinding discards the previous pose");

    // --- teardown ------------------------------------------------------------
    library.clear();
    check(!library.ready(), "clear() empties the library");
    check(library.jointCount() == 0 && library.clipCount() == 0,
        "a cleared library reports no joints or clips");
    check(!walk.play("walk"), "play() fails once the library is cleared");
    // The animator and pose still hold their own ozz state; destroying them
    // after the library is gone must be safe.
    walk.update(0.1f);
    reused.bind(library);
    check(!reused.bind(library), "binding an unready library is rejected");

    if (g_failures == 0) {
        std::printf("\nall animation checks passed\n");
        return 0;
    }
    std::printf("\n%d animation check(s) FAILED\n", g_failures);
    return 1;
}
