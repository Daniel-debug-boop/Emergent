/**
 * EMERGENT physics — Rapier world, terrain, buildings and the player.
 *
 * This is a thin, deliberately boring layer over Rapier
 * (`@dimforge/rapier3d-compat`). It owns no collision algorithm. Every hard
 * problem here — swept collision, autostep, snap-to-ground, slope limits,
 * character mass, pushing dynamic bodies — is solved by the library, and this
 * file's only job is to keep the library's world in step with EMERGENT's
 * procedural city and to hand the renderer a pose.
 *
 * It is a separate module from `game3d.js` so that it can be imported and
 * tested in Node with no DOM and no WebGL. Rapier is WASM, and WASM runs
 * perfectly well in Node, so the collision behaviour is genuinely executed by
 * the test suite rather than assumed.
 *
 * Terrain is a Rapier heightfield rebuilt around the player. That is the right
 * primitive for a heightfield world and it is exact: the same `terrainHeight`
 * that the renderer displaces vertices by is the function sampled into the
 * collider, so what the player collides with is what the player can see. An
 * approximation here would produce the worst class of bug in a game — walking
 * on top of a hill that is not there.
 */

import RAPIER from '@dimforge/rapier3d-compat';

let initialised = null;

/** Load the WASM module. Safe to call repeatedly; the work happens once. */
export function initPhysics() {
  if (!initialised) initialised = RAPIER.init();
  return initialised;
}

export const GRAVITY = -9.81 * 3.0; // scaled for this world's scale and speed

export const DEFAULT_TUNING = {
  // The character is large relative to a human because the world is: the city
  // is built from buildings tens of units across and the player crosses 190
  // units a second. These values are the ones the previous hand-rolled
  // collision used (a 1.2-unit pad around every building), so world geometry
  // that was previously passable is not suddenly solid.
  radius: 1.15,
  halfHeight: 1.3,
  // Character controller tuning. These are Rapier's, not ours.
  offset: 0.02,
  // Largest displacement handed to the controller in one call. See stepPlayer:
  // the controller's shape cast is only well behaved while the motion is small
  // compared with the shape, so a sprint is sub-stepped rather than stretched.
  maxStepDistance: 1.0,
  autostepMaxHeight: 0.9,
  autostepMinWidth: 0.25,
  snapToGroundDistance: 0.6,
  maxSlopeClimbDegrees: 52,
  minSlopeSlideDegrees: 38,
  characterMass: 90,
  // Terrain heightfield sampling, in world units. This is a correctness knob,
  // not a performance one: the collider is a bilinear surface sampled on this
  // grid, while the renderer displaces vertices with the height function
  // directly, so a coarse grid does not merely cost accuracy, it puts the
  // character on a different surface than the one on screen. Measured against
  // the shipped terrain, step 12 holds the two within 0.28 units and step 26
  // within 0.78 — the difference between standing on the ground and hovering
  // over it.
  heightfieldStep: 12,
  // Rows of the new heightfield generated per simulation step. A full patch is
  // 283 rows, so a rebuild is spread over half a second of frames while the
  // previous collider stays in the world. Building it synchronously costs 43ms
  // in one frame, four times every four seconds of walking.
  heightfieldRowsPerStep: 8,
  // The heightfield is rebuilt when the player leaves this radius from the
  // centre of the current patch.
  heightfieldRadius: 1700,
  walkSpeed: 190,
  sprintSpeed: 300,
  // Tuned so a jump clears about 3.4 units and lands in about 0.5s, which
  // reads as a jump at this world's scale.
  jumpSpeed: 17.5,
  // Downward velocity held while grounded. Small but non-zero: see the comment
  // in stepPlayer. Zeroing it entirely leaves the character hovering.
  groundPressure: 0.6,
  // Extra gravity while falling, the standard platformer trick for making a
  // descent feel responsive without changing the rise.
  fallGravityScale: 1.7,
};

let nextRigidId = 1;

/** A small registry so colliders can be removed by the caller's own key. */
function makeRegistry() {
  return new Map();
}

/**
 * Create the physics world.
 *
 * @param {object} [opts] Tuning overrides and the terrain sampler.
 * @param {(x:number,z:number)=>number} opts.heightAt Terrain height function.
 * @returns {Promise<object>} The world handle. Awaited because loading WASM is
 *          asynchronous, and a caller that skipped it would get a null world
 *          rather than an error.
 */
export async function createPhysicsWorld(opts = {}) {
  await initPhysics();
  const tuning = { ...DEFAULT_TUNING, ...(opts.tuning || {}) };
  if (typeof opts.heightAt !== 'function') {
    throw new TypeError('createPhysicsWorld needs a heightAt terrain sampler');
  }

  const world = new RAPIER.World({ x: 0, y: GRAVITY, z: 0 });
  // Fixed timestep. Rapier is deterministic for a fixed dt, which is what lets
  // the test suite assert exact outcomes rather than ranges.
  world.timestep = 1 / 60;

  const state = {
    RAPIER,
    world,
    tuning,
    heightAt: opts.heightAt,
    groundBody: null,
    groundCentre: { x: 0, z: 0 },
    groundRadius: 0,
    groundGrid: null,
    pending: null,
    player: null,
    playerCollider: null,
    controller: null,
    buildingColliders: makeRegistry(),
    dynamicBodies: makeRegistry(),
    stats: { heightfieldRebuilds: 0, buildingAdds: 0, buildingRemoves: 0, steps: 0 },
    // Rapier builds its broad phase and query structures during `world.step()`.
    // A collider created after the last step is invisible to the character
    // controller until then, so the very first move of a session — or the first
    // move after streaming adds a building — would sail straight through it.
    // Every mutation sets this, and stepPlayer flushes at most once per frame.
    dirty: true,
  };

  buildHeightfield(state, 0, 0, true);
  state.controller = world.createCharacterController(tuning.offset);
  state.controller.enableAutostep(tuning.autostepMaxHeight, tuning.autostepMinWidth, true);
  state.controller.enableSnapToGround(tuning.snapToGroundDistance);
  state.controller.setMaxSlopeClimbAngle((tuning.maxSlopeClimbDegrees * Math.PI) / 180);
  state.controller.setMinSlopeSlideAngle((tuning.minSlopeSlideDegrees * Math.PI) / 180);
  state.controller.setApplyImpulsesToDynamicBodies(true);
  state.controller.setCharacterMass(tuning.characterMass);
  return state;
}

/**
 * Rebuild the terrain heightfield centred on (cx, cz).
 *
 * Indexing, which is not documented in a form you can trust by reading: the
 * heights array is indexed `heights[xIndex * (nrows + 1) + zIndex]`, so **x
 * varies fastest**. That was measured rather than assumed, and the wrong
 * answer produces a terrain that is transposed about the diagonal — which
 * looks broadly plausible and is wrong everywhere.
 *
 * @param {object} s Physics world state.
 * @param {number} cx Centre x.
 * @param {number} cz Centre z.
 * @param {boolean} [force] Rebuild even if the player is still inside the patch.
 */
export function buildHeightfield(s, cx, cz, force = false) {
  if (!force && s.groundBody && Math.hypot(cx - s.groundCentre.x, cz - s.groundCentre.z) < s.tuning.heightfieldRadius * 0.5) {
    return false;
  }
  beginHeightfieldBuild(s, cx, cz);
  return advanceHeightfieldBuild(s, Infinity);
}

/**
 * Start generating a terrain patch centred on (cx, cz) without installing it.
 *
 * The old collider stays live throughout. Removing the ground first and
 * building the replacement afterwards would leave the character in free space
 * for the whole build, which on a slow frame is a fall.
 */
function beginHeightfieldBuild(s, cx, cz) {
  const r = s.tuning.heightfieldRadius;
  const step = s.tuning.heightfieldStep;
  // The patch extent is derived from the grid, not the other way round. A
  // heightfield is `n` cells across, spanning `n * step` and centred on its body
  // origin, so vertex j sits at `-n * step / 2 + j * step`. Sampling from
  // `cx - radius` instead shifts the whole terrain by however much `radius`
  // overshoots the grid, which is 3 world units at the default tuning — enough
  // to bury the character in the hillside on the first frame.
  const cols = Math.max(1, Math.round((r * 2) / step));
  s.pending = {
    cx, cz, cols, rows: cols, step,
    minX: cx - (cols * step) / 2,
    minZ: cz - (cols * step) / 2,
    heights: new Float32Array((cols + 1) * (cols + 1)),
    nextRow: 0
  };
  return s.pending;
}

/**
 * Generate up to `rows` rows of the pending patch, installing it when finished.
 *
 * @param {object} s Physics world state.
 * @param {number} rows Row budget for this call, or Infinity to finish now.
 * @returns {boolean} True if the live heightfield was replaced by this call.
 */
export function advanceHeightfieldBuild(s, rows) {
  const p = s.pending;
  if (!p) return false;
  const end = Math.min(p.cols + 1, p.nextRow + rows);
  for (let j = p.nextRow; j < end; j++) {
    const x = p.minX + j * p.step;
    for (let i = 0; i <= p.rows; i++) {
      p.heights[j * (p.rows + 1) + i] = s.heightAt(x, p.minZ + i * p.step);
    }
  }
  p.nextRow = end;
  if (p.nextRow <= p.rows) return false;

  if (s.groundBody) {
    s.world.removeRigidBody(s.groundBody);
    s.groundBody = null;
  }
  const body = s.world.createRigidBody(
    RAPIER.RigidBodyDesc.fixed().setTranslation(p.cx, 0, p.cz)
  );
  s.world.createCollider(
    RAPIER.ColliderDesc.heightfield(p.rows, p.cols, p.heights, { x: p.cols * p.step, y: 1, z: p.rows * p.step }),
    body
  );
  s.groundBody = body;
  s.groundCentre = { x: p.cx, z: p.cz };
  s.groundRadius = s.tuning.heightfieldRadius;
  s.groundGrid = { minX: p.minX, minZ: p.minZ, cols: p.cols, rows: p.rows, step: p.step };
  s.pending = null;
  s.dirty = true;
  s.stats.heightfieldRebuilds++;
  return true;
}

/**
 * Keep the terrain under the player, incrementally.
 *
 * Called once per simulation step. A patch is started when the player leaves
 * half the patch radius and then generated a few rows at a time, which is why a
 * terrain rebuild costs a fraction of a millisecond per frame instead of 43ms
 * in one. The rebuild always completes long before the player can reach the
 * edge of the patch it is replacing, because the trigger distance is half the
 * radius and the whole build is a few hundred rows.
 *
 * @returns {boolean} True if a rebuild was started or is in progress.
 */
export function streamHeightfield(s) {
  if (s.pending) {
    advanceHeightfieldBuild(s, s.tuning.heightfieldRowsPerStep);
    return true;
  }
  if (!s.player) return false;
  const p = s.player.translation();
  const r = s.tuning.heightfieldRadius;
  if (Math.hypot(p.x - s.groundCentre.x, p.z - s.groundCentre.z) < r * 0.5) return false;
  beginHeightfieldBuild(s, p.x, p.z);
  advanceHeightfieldBuild(s, s.tuning.heightfieldRowsPerStep);
  return true;
}

/**
 * Height of the terrain *collider* at a world position, or null outside the patch.
 *
 * The collider is sampled on a grid, so its surface is very slightly different
 * from the height function that displaced the render mesh. Spawning a character
 * at the raw height function therefore drops it a fraction of a unit inside the
 * collider, and a body that starts interpenetrating a heightfield is pushed out
 * in an arbitrary direction — which looks exactly like collision that does not
 * work at all, and costs a frame of jitter to escape even when it does.
 *
 * Interpolating the same four samples the collider uses puts the character on
 * the surface it will actually be standing on.
 */
export function surfaceHeightAt(s, x, z) {
  const g = s.groundGrid;
  if (!g) return null;
  const gx = (x - g.minX) / g.step;
  const gz = (z - g.minZ) / g.step;
  if (gx < 0 || gz < 0 || gx > g.cols || gz > g.rows) return null;
  const i = Math.min(g.cols - 1, Math.floor(gx));
  const k = Math.min(g.rows - 1, Math.floor(gz));
  const fx = gx - i;
  const fz = gz - k;
  const at = (a, b) => s.heightAt(g.minX + a * g.step, g.minZ + b * g.step);
  const h00 = at(i, k), h10 = at(i + 1, k), h01 = at(i, k + 1), h11 = at(i + 1, k + 1);
  return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
}

/** Remove the terrain heightfield entirely (used by tests). */
export function clearHeightfield(s) {
  s.pending = null;
  if (s.groundBody) {
    s.world.removeRigidBody(s.groundBody);
    s.groundBody = null;
    s.dirty = true;
  }
}

/**
 * Register a static box collider, replacing any previous one with the same key.
 *
 * Keyed rather than tracked in a list so the streaming layer can call this
 * unconditionally for every cell it visits and get idempotent behaviour for
 * free — a set-based sync that has to diff before it writes is a set-based sync
 * that will eventually be wrong.
 *
 * @param {object} s Physics world state.
 * @param {string|number} key Caller-owned identity.
 * @param {number} x Centre x.
 * @param {number} y Base y.
 * @param {number} z Centre z.
 * @param {number} hx Half-size x.
 * @param {number} hy Half-size y.
 * @param {number} hz Half-size z.
 * @returns {number|null} Collider handle.
 */
export function setStaticBox(s, key, x, y, z, hx, hy, hz) {
  removeStaticBox(s, key);
  const body = s.world.createRigidBody(RAPIER.RigidBodyDesc.fixed().setTranslation(x, y, z));
  const collider = s.world.createCollider(RAPIER.ColliderDesc.cuboid(hx, hy, hz), body);
  s.buildingColliders.set(key, { body, collider });
  s.stats.buildingAdds++;
  s.dirty = true;
  return collider.handle;
}

export function removeStaticBox(s, key) {
  const entry = s.buildingColliders.get(key);
  if (!entry) return false;
  s.world.removeRigidBody(entry.body);
  s.buildingColliders.delete(key);
  s.stats.buildingRemoves++;
  s.dirty = true;
  return true;
}

export function clearStaticBoxes(s) {
  for (const key of [...s.buildingColliders.keys()]) removeStaticBox(s, key);
}

/**
 * Create or update a kinematic agent (an NPC or a car).
 *
 * Kinematic rather than dynamic: NPCs follow scripted goals and cars follow
 * roads, so simulating them with forces would produce a simulation that is both
 * slower and less controllable. They still collide — that is the point, and it
 * is what makes the player unable to walk through a crowd.
 *
 * @param {object} s Physics world state.
 * @param {string|number} key Caller-owned identity.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} hx Half-size x.
 * @param {number} hy Half-size y.
 * @param {number} hz Half-size z.
 */
export function setAgentBox(s, key, x, y, z, hx, hy, hz) {
  let entry = s.dynamicBodies.get(key);
  if (!entry) {
    const body = s.world.createRigidBody(
      RAPIER.RigidBodyDesc.kinematicPositionBased().setTranslation(x, y, z)
    );
    const collider = s.world.createCollider(
      RAPIER.ColliderDesc.cuboid(hx, hy, hz).setFriction(0.4),
      body
    );
    entry = { body, collider };
    s.dynamicBodies.set(key, entry);
    s.dirty = true;
  } else {
    const t = entry.body.translation();
    entry.body.setNextKinematicTranslation({ x, y, z });
    void t;
  }
  return entry;
}

export function removeAgent(s, key) {
  const entry = s.dynamicBodies.get(key);
  if (!entry) return false;
  s.world.removeRigidBody(entry.body);
  s.dynamicBodies.delete(key);
  s.dirty = true;
  return true;
}

export function clearAgents(s) {
  for (const key of [...s.dynamicBodies.keys()]) removeAgent(s, key);
}

/**
 * Distance from a capsule collider's origin down to its lowest point.
 *
 * A Rapier capsule is `capsule(halfHeight, radius)`, and its total extent is
 * `2 * halfHeight + 2 * radius` — the hemispherical caps are *outside* the
 * half-height, not inside it. So a character whose feet should be at world y
 * has its body origin at `y + halfHeight + radius`.
 *
 * Getting this wrong by using halfHeight alone is not a rounding error: the
 * character spawns embedded in the ground, and a body that starts interpenetrating
 * a heightfield gets pushed out by contact resolution in an arbitrary direction,
 * which is indistinguishable from broken collision.
 */
export function capsuleFootOffset(tuning) {
  return tuning.halfHeight + tuning.radius;
}

/**
 * Make sure the live terrain patch actually covers a world position.
 *
 * The incremental stream only starts a rebuild once the player is already
 * inside the current patch, which is correct for travel and useless for a
 * spawn: a player created at the far corner of the world would spend the first
 * half-second of the build in free space, falling. Anything that *places* the
 * player — spawn, teleport, load — pays for a synchronous rebuild instead.
 */
function ensurePatchCovers(s, x, z) {
  const g = s.groundGrid;
  if (g && x >= g.minX && z >= g.minZ && x <= g.minX + g.cols * g.step && z <= g.minZ + g.rows * g.step) return false;
  s.pending = null;
  buildHeightfield(s, x, z, true);
  return true;
}

/** Create the player's kinematic body and capsule, if not already present. */
export function ensurePlayer(s, x, y, z) {
  if (s.player) return s.player;
  ensurePatchCovers(s, x, z);
  const t = s.tuning;
  const body = s.world.createRigidBody(
    RAPIER.RigidBodyDesc.kinematicPositionBased().setTranslation(x, y + capsuleFootOffset(t), z)
  );
  const collider = s.world.createCollider(
    RAPIER.ColliderDesc.capsule(t.halfHeight, t.radius),
    body
  );
  s.player = body;
  s.playerCollider = collider;
  s.vy = 0;
  s.wasGrounded = false;
  s.jumpHeld = false;
  s.coyote = 0;
  s.dirty = true;
  return body;
}

/**
 * Advance the player one step.
 *
 * The order is Rapier's contract, not a preference: a desired translation is
 * handed to the controller, the controller sweeps the collider against the
 * world and returns the movement it could actually achieve, and only that is
 * applied. Setting the position directly and letting the controller fix it up
 * afterwards is the common mistake, and it reintroduces exactly the tunnelling
 * the controller exists to prevent.
 *
 * @param {object} s Physics world state.
 * @param {number} wishX Desired world-space X displacement this frame.
 * @param {number} wishZ Desired world-space Z displacement this frame.
 * @param {{jump?:boolean, jumpHeld?:boolean, gravityScale?:number}} [input]
 * @returns {{x:number,y:number,z:number, grounded:boolean, hitWall:boolean, jumped:boolean, hitCeiling:boolean}}
 */
export function stepPlayer(s, wishX, wishZ, input = {}) {
  const t = s.tuning;
  const body = s.player;
  if (!body) throw new Error('stepPlayer called before ensurePlayer');
  if (s.dirty) {
    // One throwaway step to publish the colliders added since the last frame.
    // It is a real step, so a dynamic body would integrate once here as well,
    // which is what it needed anyway to be visible to the sweep.
    s.dirty = false;
    s.world.step();
  }
  const pos = body.translation();
  const dt = s.world.timestep;

  // Rapier has no built-in jump, and it should not have one: whether a jump is
  // available depends on coyote time and input buffering, which are game rules
  // rather than collision. So the vertical component is integrated here and
  // handed to the controller as a desired move, exactly as the horizontal is.
  const grounded = s.wasGrounded;
  let jumped = false;
  let vy = s.vy || 0;

  // A jump is a press, not a state. Gating on the level of the jump key means a
  // held key re-fires the instant the character lands, which is how platformers
  // acquire the notorious "bunny hop" — and it makes a held jump impossible to
  // distinguish from mashing one. The edge is the rule; Rapier has no opinion
  // either way, and neither should this layer.
  const pressed = !!input.jump && !s.jumpHeld;
  s.jumpHeld = !!input.jump;

  if (pressed && (grounded || (s.coyote || 0) > 0)) {
    vy = t.jumpSpeed;
    s.coyote = 0;
    jumped = true;
  } else {
    const scale = vy > 0 ? 1 : (input.gravityScale || t.fallGravityScale);
    vy -= -s.world.gravity.y * scale * dt;
    // Downward pressure, not free fall, while grounded.
    //
    // Letting gravity integrate without a floor is wrong in both directions at
    // once: the velocity grows without bound while the controller holds the
    // character on the surface, so stepping off a ledge drops like a stone
    // from a standing start — but zeroing it outright removes the only thing
    // asking the character to stay in contact, and snap-to-ground then leaves
    // the character hovering a visible fraction of a unit above the ground. A
    // small constant negative velocity supplies continuous contact pressure
    // that the controller resolves against the surface, and bounds the
    // integration at the same time.
    if (grounded && vy < 0) vy = -t.groundPressure;
  }
  s.coyote = grounded ? 0 : Math.max(0, (s.coyote || 0) - dt);

  // Keep the terrain under the player. Nothing else does this: a heightfield is
  // a fixed-size patch, and a sprinting player crosses 8000 units in a minute,
  // so without this the character walks off the edge of the world and every
  // collision test below it becomes meaningless. The rebuild is incremental, so
  // this is a few rows of sampling rather than a stall.
  streamHeightfield(s);

  // Sub-step the move.
  //
  // Rapier's controller sweeps the collider, so it cannot tunnel through a wall
  // the way a point-in-solid test does — but the sweep is only well behaved while
  // the requested motion is small next to the shape. Handing it forty units when
  // the capsule has a radius of 1.15 does not produce a fast character; it
  // produces a degenerate cast that reports a ground contact at zero distance
  // and then refuses to move at all, so a fast character stands still against
  // thin geometry and a slightly slower one walks straight through it. Neither
  // is acceptable, so a large requested move is split into several sweeps and
  // the results are summed, which is what every production character controller
  // does and what keeps the anti-tunnelling guarantee at any speed.
  const subSteps = Math.max(1, Math.ceil(Math.max(Math.abs(wishX), Math.abs(wishZ), Math.abs(vy * dt)) / t.maxStepDistance));
  let cx = pos.x, cy = pos.y, cz = pos.z;
  let nowGrounded = false;
  let hitWall = false;
  let hitCeiling = false;
  for (let i = 0; i < subSteps; i++) {
    // The controller sweeps from wherever the collider actually is, so the body
    // has to be advanced between sweeps. Re-casting from the same place N times
    // is not sub-stepping, it is the same sweep counted N times — which is
    // precisely how a "sub-stepped" character walks through walls.
    //
    // Moving the body is not enough on its own: a collider's world position is
    // cached in the collider set and only refreshed by the step, so without
    // this call the sweep silently continues to start from the frame's opening
    // position no matter where the body was moved to.
    body.setTranslation({ x: cx, y: cy, z: cz }, true);
    s.world.propagateModifiedBodyPositionsToColliders();
    s.controller.computeColliderMovement(s.playerCollider, {
      x: wishX / subSteps,
      y: (vy * dt) / subSteps,
      z: wishZ / subSteps,
    });
    const part = s.controller.computedMovement();
    cx += part.x;
    cy += part.y;
    cz += part.z;
    nowGrounded = s.controller.computedGrounded();
    // A contact is a wall only if it faces sideways. The character rests on the
    // ground every single frame, so counting every contact as a wall makes
    // hitWall permanently true and the signal worthless.
    const n = s.controller.numComputedCollisions();
    for (let c = 0; c < n; c++) {
      const ny = s.controller.computedCollision(c).normal1.y;
      if (ny < -0.7) hitCeiling = true;
      else if (ny < 0.7) hitWall = true;
    }
  }
  const movement = { x: cx - pos.x, y: cy - pos.y, z: cz - pos.z };

  body.setNextKinematicTranslation({
    x: pos.x + movement.x,
    y: pos.y + movement.y,
    z: pos.z + movement.z,
  });
  s.world.step();
  s.stats.steps++;

  // `computedGrounded()` is the single source of truth for "the surface stopped
  // the character". An earlier version inferred it by comparing the requested
  // vertical move against the achieved one, which is true in ordinary free fall
  // as well — so it zeroed gravity every frame and the character fell at a
  // constant crawl.
  s.vy = vy;
  s.wasGrounded = nowGrounded;

  const after = body.translation();
  return {
    x: after.x,
    y: after.y - capsuleFootOffset(t),
    z: after.z,
    grounded: nowGrounded,
    hitWall,
    hitCeiling,
    jumped,
    vy,
  };
}

/** Current feet position of the player. */
export function playerFeet(s) {
  if (!s.player) return { x: 0, y: 0, z: 0 };
  const t = s.player.translation();
  return { x: t.x, y: t.y - capsuleFootOffset(s.tuning), z: t.z };
}

/** Teleport the player, clearing momentum. Used by respawn and by tests. */
export function teleportPlayer(s, x, y, z) {
  if (!s.player) return;
  ensurePatchCovers(s, x, z);
  const o = capsuleFootOffset(s.tuning);
  s.player.setTranslation({ x, y: y + o, z }, true);
  s.player.setNextKinematicTranslation({ x, y: y + o, z });
  s.coyote = 0;
  s.vy = 0;
  s.wasGrounded = false;
  s.jumpHeld = false;
}

export function disposePhysics(s) {
  if (!s || !s.world) return;
  clearAgents(s);
  clearStaticBoxes(s);
  clearHeightfield(s);
  if (s.player) s.world.removeRigidBody(s.player);
  s.world.free();
  s.player = null;
  s.world = null;
}

export { RAPIER };
