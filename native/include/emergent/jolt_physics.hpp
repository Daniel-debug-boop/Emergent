#pragma once
#include <memory>

namespace emergent {

/**
 * Rigid-body physics world backed by Jolt Physics.
 *
 * Opaque implementation: every Jolt type stays behind the pImpl so this header
 * carries no upstream include cost and the engine's build flags cannot leak
 * into callers.
 *
 * Lifetime is RAII. Destroying the world is equivalent to calling shutdown(),
 * and the order of teardown (bodies -> job system -> allocator -> factory) is
 * handled once, in one place.
 *
 * Not thread-safe. Drive it from the single simulation thread.
 */
class JoltPhysicsWorld {
public:
    JoltPhysicsWorld();
    ~JoltPhysicsWorld();

    JoltPhysicsWorld(const JoltPhysicsWorld&) = delete;
    JoltPhysicsWorld& operator=(const JoltPhysicsWorld&) = delete;

    /**
     * Register Jolt types and bring the simulation up.
     * Idempotent: calling it on a live world succeeds and changes nothing.
     * Returns false if the underlying system could not be initialised.
     */
    bool initialize();

    /** Tear the simulation down and release the Jolt factory. Idempotent. */
    void shutdown();

    /** True between a successful initialize() and shutdown(). */
    bool available() const;

    /**
     * Advance the simulation.
     * @param dt Seconds since the previous step. Values <= 0 are ignored so a
     *           paused frame cannot produce an undefined integration.
     */
    void step(float dt);

    /**
     * Add an axis-aligned box to the world.
     * @param halfExtent Half-size of the cube on each axis; must be > 0.
     * @param dynamic    false for static geometry, true for a simulated body.
     * @return false if the shape or body could not be created.
     */
    bool createBox(float x, float y, float z, float halfExtent, bool dynamic);

    /**
     * Add an axis-aligned box with independent extents.
     *
     * A ground plane, wall or ramp is a thin slab, not a cube, so a cube-only
     * API cannot express level geometry: a "floor" built as a 50-unit cube
     * swallows anything placed above it and ejects the body violently. Level
     * geometry must go through this entry point.
     *
     * @param hx,hy,hz  Half-sizes on each axis; all must be > 0.
     * @return false if the shape or body could not be created.
     */
    bool createBoxExtents(float x, float y, float z, float hx, float hy, float hz, bool dynamic);

    /**
     * Add a box and return a stable handle for it, or -1 on failure.
     *
     * Handles index a table inside the world and stay valid until shutdown, so
     * a caller can hold one across frames -- which is exactly what a frame
     * loop needs in order to read a transform and steer a body. createBox and
     * createBoxExtents are this call with the handle thrown away; the handles
     * start at 0 in creation order, so the ground seeded by initialize() is 0
     * and the body resting above it is 1.
     */
    int addBox(float x, float y, float z, float hx, float hy, float hz, bool dynamic);

    /** World-space centre of a handle, or false if the handle is unknown. */
    bool bodyPosition(int handle, float (&out)[3]) const;
    /** Linear velocity of a handle, or false if the handle is unknown. */
    bool bodyVelocity(int handle, float (&out)[3]) const;
    /**
     * Replaces the linear velocity outright and wakes the body if it had gone
     * to sleep. Setting velocity rather than accumulating impulses is what
     * makes input feel direct; the trade is that inertia is not simulated,
     * which is the right call for a character controller and the wrong one for
     * a thrown crate.
     */
    bool setBodyVelocity(int handle, float x, float y, float z);
    /**
     * Teleports a handle to a world-space centre, zeroing its velocity.
     *
     * This is a reset, not a move: it discards momentum and puts the body to
     * sleep-safe state, so it is what a level restart or a respawn needs and
     * what a character controller must never use mid-play (teleporting through
     * a wall is how players end up outside the level).
     */
    bool setBodyPosition(int handle, float x, float y, float z);
    /**
     * Handle of the first dynamic body added, or -1 if the world holds no
     * dynamic body. initialize() seeds one, so a freshly initialized world
     * returns 1.
     */
    int primaryDynamicBody() const;

    /** World-space Y of the first dynamic body, or 0 if there is none. */
    float firstDynamicY() const;

    /** Number of bodies currently in the world. */
    int bodyCount() const;

    /**
     * Maximum simulated bodies. Exceeding it is not fatal (Jolt grows on
     * demand) but staying under it avoids reallocations mid-simulation.
     */
    static constexpr unsigned kMaxBodies = 10240;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace emergent
