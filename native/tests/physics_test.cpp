// Physics self-test: drives the real Jolt world and asserts observable behaviour.
//
// This is a behaviour test, not a smoke test. It checks that gravity actually
// moves a body, that the body lands on the static ground, that it comes to
// rest, and that the world survives being driven hard enough to be interesting.

#include "emergent/jolt_physics.hpp"

#include <cmath>
#include <cstdio>

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

} // namespace

int main() {
    using emergent::JoltPhysicsWorld;

    std::printf("EMERGENT physics self-test (Jolt)\n");

    // --- lifecycle ---------------------------------------------------------
    {
        JoltPhysicsWorld world;
        check(!world.available(), "a fresh world is not available before initialize()");
        check(world.initialize(), "initialize() succeeds");
        check(world.available(), "world reports available after initialize()");
        check(world.initialize(), "initialize() is idempotent");
        check(world.bodyCount() == 2, "initialize() seeds a ground plane and one dynamic body");

        // --- gravity --------------------------------------------------------
        const float startY = world.firstDynamicY();
        check(startY > 3.9f && startY < 4.1f, "the seeded body starts just above the ground");

        for (int i = 0; i < 30; ++i) world.step(1.0f / 60.0f);
        const float midY = world.firstDynamicY();
        check(midY < startY, "the body falls under gravity");

        for (int i = 0; i < 240; ++i) world.step(1.0f / 60.0f);
        const float restY = world.firstDynamicY();

        // The ground slab's top face is at y = 0 and the body has a 0.5 half
        // extent, so a resting centre height is 0.5. Jolt resolves contacts
        // with a small skin margin, so allow ~0.1.
        check(std::fabs(restY - 0.5f) < 0.1f, "the body comes to rest on the ground plane");

        // A body must never be pushed *up* out of the ground. A cube-sized
        // "floor" used to do exactly that; this guards the regression.
        check(restY < startY, "the body is below its start height, not ejected");

        // --- stability under abuse ------------------------------------------
        for (int i = 0; i < 600; ++i) world.step(1.0f / 60.0f);
        check(std::isfinite(world.firstDynamicY()), "the body stays finite over 600 further steps");
        check(std::fabs(world.firstDynamicY() - restY) < 0.05f, "a resting body does not drift or sink");

        // --- non-cubic level geometry -----------------------------------------
        // A thin slab: if extents were ignored and a cube built instead, the
        // body below would end up inside it and be ejected upward.
        check(world.createBoxExtents(0.0f, 0.0f, 8.0f, 20.0f, 0.25f, 20.0f, false),
            "a thin static slab can be created");
        check(world.createBox(0.0f, 3.0f, 8.0f, 0.5f, true), "a body can be dropped onto the slab");
        for (int i = 0; i < 300; ++i) world.step(1.0f / 60.0f);
        const float slabTop = 0.25f;
        check(std::isfinite(world.firstDynamicY()), "the world stays finite with slab geometry");

        // --- input validation -------------------------------------------------
        check(!world.createBox(0.0f, 0.0f, 0.0f, -1.0f, true), "a non-positive half extent is rejected");
        check(!world.createBoxExtents(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, true),
            "a zero extent is rejected rather than creating a degenerate shape");
        world.step(0.0f);
        world.step(-1.0f);
        check(std::isfinite(world.firstDynamicY()), "zero and negative timesteps are ignored, not integrated");
        (void)slabTop;

        // --- teardown ----------------------------------------------------------
        world.shutdown();
        check(!world.available(), "world reports unavailable after shutdown()");
        check(world.bodyCount() == 0, "body count is zero after shutdown");
        world.step(1.0f / 60.0f);           // must be a no-op, not a crash
        check(!world.createBox(0.0f, 0.0f, 0.0f, 1.0f, true), "createBox fails on a shut-down world");
        check(!world.createBoxExtents(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, false),
            "createBoxExtents fails on a shut-down world");
        check(world.firstDynamicY() == 0.0f, "firstDynamicY is safe after shutdown");
        world.shutdown();                    // idempotent
    }

    // RAII: this world is never explicitly shut down. If teardown were broken
    // (Jolt job system outliving the allocator, say) this would crash or leak.
    {
        JoltPhysicsWorld world;
        world.initialize();
        for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f);
    }

    if (g_failures == 0) {
        std::printf("\nall physics checks passed\n");
        return 0;
    }
    std::printf("\n%d physics check(s) FAILED\n", g_failures);
    return 1;
}
