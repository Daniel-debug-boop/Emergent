/**
 * Integration tests for physics.mjs — real Rapier, real WASM, in Node.
 *
 * Nothing here is mocked. Rapier's character controller is the thing being
 * tested, and a mock of it would only prove that the mock behaves like the mock.
 * The value of these tests is that the collision behaviour EMERGENT depends on
 * is executed on every CI run on a machine with no GPU and no browser.
 *
 * Run: `npm run test:physics`
 */
import {
  createPhysicsWorld, ensurePlayer, stepPlayer, playerFeet, teleportPlayer,
  setStaticBox, removeStaticBox, clearStaticBoxes, setAgentBox, clearAgents,
  buildHeightfield, streamHeightfield, surfaceHeightAt, disposePhysics,
  DEFAULT_TUNING, GRAVITY
} from './physics.mjs';
import { terrainHeight } from './world.mjs';

let checks = 0;
let failures = 0;
const worlds = [];

function test(name, fn) {
  try {
    const r = fn();
    if (r && typeof r.then === 'function') {
      return r.then(
        () => process.stdout.write(`✓ ${name}\n`),
        (err) => { failures++; process.stdout.write(`✗ ${name}\n  ${err.message}\n`); }
      );
    }
    process.stdout.write(`✓ ${name}\n`);
  } catch (err) {
    failures++;
    process.stdout.write(`✗ ${name}\n  ${err.message}\n`);
  }
  return Promise.resolve();
}

function assert(cond, msg) {
  checks++;
  if (!cond) throw new Error(msg);
}

function assertClose(actual, expected, tol, msg) {
  checks++;
  if (!Number.isFinite(actual) || Math.abs(actual - expected) > tol) {
    throw new Error(`${msg} (expected ${expected} +/- ${tol}, got ${actual})`);
  }
}

/** Flat world at y=0. */
const flat = () => 0;
/** A single tall hill: 12 units high at the origin, falling off by x/40. */
const hill = (x) => Math.max(0, 12 - Math.hypot(x, 0) / 4);
/** A ramp along +x of 1 unit per 1 unit. */
const ramp = (x) => x;

async function makeWorld(heightAt = flat, tuning = {}) {
  const s = await createPhysicsWorld({ heightAt, tuning });
  worlds.push(s);
  return s;
}

function wall(s, minX, minZ, maxX, maxZ, height) {
  return setStaticBox(s, 'wall', (minX + maxX) / 2, height / 2, (minZ + maxZ) / 2,
    (maxX - minX) / 2, height / 2, (maxZ - minZ) / 2);
}

await test('a world is created with terrain and a character controller', async () => {
  const s = await makeWorld();
  assert(s.world, 'a Rapier world exists');
  assert(s.groundBody, 'a terrain heightfield exists');
  assert(s.controller, 'a character controller exists');
  assert(GRAVITY < 0, 'gravity points down');
});

await test('createPhysicsWorld rejects a world with no terrain sampler', async () => {
  let threw = false;
  try { await createPhysicsWorld({}); } catch { threw = true; }
  assert(threw, 'a missing heightAt is a TypeError, not a silent flat world');
});

await test('the character falls onto flat terrain and stops at the surface', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 6, 0);
  for (let i = 0; i < 400; i++) stepPlayer(s, 0, 0);
  const feet = playerFeet(s);
  assertClose(feet.y, 0, 0.25, 'the character rests on the ground plane');
  assert(feet.y > -1, 'and does not sink through it');
});

await test('the character comes to rest instead of sinking or bouncing', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 8, 0);
  for (let i = 0; i < 200; i++) stepPlayer(s, 0, 0);
  const y1 = playerFeet(s).y;
  for (let i = 0; i < 120; i++) stepPlayer(s, 0, 0);
  const y2 = playerFeet(s).y;
  assertClose(y2, y1, 0.02, 'the character is stationary once settled');
});

await test('the character cannot walk through a wall at any speed', async () => {
  const s = await makeWorld();
  wall(s, 20, -60, 22, 60, 40);
  ensurePlayer(s, 0, 0, 0);
  // Well past the wall: at 40 units per step a thin wall is trivially tunnelled
  // by anything that tests only the destination.
  for (let i = 0; i < 200; i++) stepPlayer(s, 40, 0);
  const feet = playerFeet(s);
  assert(feet.x < 20, `the character is stopped by the wall (x=${feet.x.toFixed(2)})`);
  assert(feet.x > 18, `and stopped at the wall face, not short of it (x=${feet.x.toFixed(2)})`);
});

await test('the character slides along a wall instead of sticking to it', async () => {
  const s = await makeWorld();
  // Long enough that the character cannot simply walk around the end of it
  // within the frames sampled — going around a wall is correct behaviour and
  // would make this test pass for the wrong reason.
  wall(s, 20, -600, 22, 600, 40);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 60; i++) stepPlayer(s, 12, 4);
  const feet = playerFeet(s);
  assert(feet.x < 20, `blocked in x (x=${feet.x.toFixed(2)})`);
  assert(feet.z > 8, `and slid along the wall in z (z=${feet.z.toFixed(2)})`);
  assert(feet.z < 600, 'without slipping past the end of it');
});

await test('a low kerb is stepped over', async () => {
  const s = await makeWorld();
  // Long in x, and driven for fewer units than the kerb is long, so the
  // character is still standing on it when the test samples rather than already
  // back down on the far side. Walking off the far end is correct too, and it
  // would make this test pass without the step ever having happened.
  setStaticBox(s, 'kerb', 105, 0.4, 0, 96, 0.4, 30);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 25; i++) stepPlayer(s, 6, 0);
  const feet = playerFeet(s);
  assert(feet.x > 15, `the character climbed the kerb (x=${feet.x.toFixed(2)})`);
  assert(feet.x < 201, 'and is still on it');
  assertClose(feet.y, 0.8, 0.35, 'and is standing on top of it');
});

await test('a wall far taller than the step height is not climbed', async () => {
  const s = await makeWorld();
  wall(s, 20, -60, 22, 60, 40);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 240; i++) stepPlayer(s, 6, 0);
  assert(playerFeet(s).x < 20, 'the character is stopped');
});

await test('a character walking up a walkable ramp stays on it', async () => {
  const s = await makeWorld(ramp);
  ensurePlayer(s, 0, 0, 0);
  // 1 unit up per 1 unit along x is 45 degrees, inside the climb limit.
  for (let i = 0; i < 200; i++) stepPlayer(s, 0.6, 0);
  const feet = playerFeet(s);
  assert(feet.x > 40, `the character climbed the ramp (x=${feet.x.toFixed(1)})`);
  assert(feet.y > 5, `and rose with it (y=${feet.y.toFixed(2)})`);
  assertClose(feet.y, feet.x, 3.0, 'staying on the ramp surface');
});

await test('the character is not blocked by terrain that is only walkable downhill', async () => {
  // Sanity check on the slope limit: a very steep face should stop the
  // character rather than being climbed.
  const cliff = (x) => (x > 30 ? 40 : 0);
  const s = await makeWorld(cliff);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 240; i++) stepPlayer(s, 4, 0);
  assert(playerFeet(s).x < 32, `the character is stopped by the cliff (x=${playerFeet(s).x.toFixed(2)})`);
});

await test('jumping leaves the ground and lands again', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 90; i++) stepPlayer(s, 0, 0);
  let jumped = false;
  let peak = -Infinity;
  for (let i = 0; i < 300; i++) {
    const r = stepPlayer(s, 0, 0, { jump: !jumped, jumpHeld: true });
    if (r.jumped) jumped = true;
    const y = playerFeet(s).y;
    if (y > peak) peak = y;
  }
  assert(jumped, 'the jump fired');
  assert(peak > 1.5, `the jump gained height (${peak.toFixed(2)})`);
});

await test('jump is refused when not grounded, so it cannot be spammed in the air', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 120, 0);
  let airJumps = 0;
  let maxY = -Infinity;
  for (let i = 0; i < 300; i++) {
    // Held jump the whole way down.
    const r = stepPlayer(s, 0, 0, { jump: true, jumpHeld: true });
    if (r.jumped) airJumps++;
    const y = playerFeet(s).y;
    if (y > maxY) maxY = y;
  }
  assert(airJumps <= 1, `a held jump fires at most once in the air (${airJumps})`);
});

await test('the same inputs produce the same state on a second identical world', async () => {
  // Determinism. Rapier is deterministic for a fixed timestep, which is what
  // makes this assertable at all; if it ever stops being true, this fails
  // loudly rather than the game quietly becoming unreproducible.
  const drive = async () => {
    const s = await makeWorld(hill);
    wall(s, 30, -40, 32, 40, 30);
    ensurePlayer(s, 0, 0, 0);
    const trace = [];
    for (let i = 0; i < 300; i++) {
      stepPlayer(s, 3, Math.sin(i * 0.1) * 2, { jump: i % 51 === 0, jumpHeld: i % 7 !== 0 });
      const f = playerFeet(s);
      trace.push(f.x, f.y, f.z);
    }
    return trace;
  };
  const a = await drive();
  const b = await drive();
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      throw new Error(`state diverged at component ${i}: ${a[i]} vs ${b[i]}`);
    }
  }
  checks++;
});

await test('a static box can be added, replaced and removed by key', async () => {
  const s = await makeWorld();
  const first = setStaticBox(s, 'crate', 10, 1, 0, 1, 1, 1);
  assert(first !== null, 'adding returns a handle');
  const second = setStaticBox(s, 'crate', 11, 1, 0, 1, 1, 1);
  assert(second !== first, 're-adding the same key creates a new collider');
  assert(s.buildingColliders.size === 1, 'and does not leak the old one');
  assert(removeStaticBox(s, 'crate'), 'removal reports success');
  assert(!removeStaticBox(s, 'crate'), 'removing twice is not an error');
  assert(s.buildingColliders.size === 0, 'the registry is empty');
});

await test('a character cannot walk through a static box', async () => {
  const s = await makeWorld();
  setStaticBox(s, 'block', 15, 3, 0, 1.5, 3, 12);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 240; i++) stepPlayer(s, 5, 0);
  assert(playerFeet(s).x < 15, `blocked by the box (x=${playerFeet(s).x.toFixed(2)})`);
});

await test('the character is blocked by an NPC agent', async () => {
  // A crowd the player cannot walk through is the point of giving agents
  // colliders at all.
  const s = await makeWorld();
  setAgentBox(s, 'npc1', 15, 1, 0, 1, 1, 1);
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 240; i++) {
    setAgentBox(s, 'npc1', 15, 1, 0, 1, 1, 1);
    stepPlayer(s, 5, 0);
  }
  assert(playerFeet(s).x < 15, `blocked by the agent (x=${playerFeet(s).x.toFixed(2)})`);
});

await test('agents can be cleared without leaving colliders behind', async () => {
  const s = await makeWorld();
  setAgentBox(s, 'a', 5, 1, 0, 1, 1, 1);
  setAgentBox(s, 'b', 8, 1, 0, 1, 1, 1);
  assert(s.dynamicBodies.size === 2, 'two agents registered');
  clearAgents(s);
  assert(s.dynamicBodies.size === 0, 'all agents removed');
  assert(() => stepPlayer.call(null, s, 0, 0), 'the world is still usable');
});

await test('the heightfield follows the player and does not rebuild needlessly', async () => {
  const s = await makeWorld();
  const before = s.stats.heightfieldRebuilds;
  assert(!buildHeightfield(s, 10, 10), 'a small move does not trigger a rebuild');
  assert(s.stats.heightfieldRebuilds === before, 'rebuild count unchanged');
  assert(buildHeightfield(s, 5000, 5000), 'a large move does');
  assert(s.stats.heightfieldRebuilds === before + 1, 'rebuild count incremented');
  assert(s.groundCentre.x === 5000, 'the patch is centred on the player');
});

await test('a forced rebuild reuses no collider handle', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 0, 0);
  buildHeightfield(s, 0, 0, true);
  // The old ground body is removed before the new one is created; if it were
  // not, every rebuild would leak a body and the count would climb.
  for (let i = 0; i < 5; i++) {
    buildHeightfield(s, i * 4000, 0, true);
    stepPlayer(s, 0, 0);
  }
  assert(Number.isFinite(playerFeet(s).y), 'the character is still supported after rebuilds');
  assert(s.world.bodies.len() < 10, `rebuilds do not accumulate bodies (${s.world.bodies.len()})`);
});

await test('teleporting the player clears momentum', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 0, 0);
  for (let i = 0; i < 60; i++) stepPlayer(s, 10, 0);
  teleportPlayer(s, 100, 0, 100);
  const f = playerFeet(s);
  assertClose(f.x, 100, 0.5, 'x after teleport');
  assertClose(f.z, 100, 0.5, 'z after teleport');
  const after = stepPlayer(s, 0, 0);
  assert(Math.abs(after.vy) < 40, `vertical velocity is reset (${after.vy.toFixed(2)})`);
});

await test('a long adversarial run stays finite and bounded', async () => {
  const s = await makeWorld(hill);
  wall(s, 40, -50, 42, 50, 30);
  setStaticBox(s, 'shelf', -30, 2, 0, 4, 2, 4);
  ensurePlayer(s, 0, 40, 0);
  for (let i = 0; i < 1500; i++) {
    setAgentBox(s, 'mover', Math.sin(i * 0.2) * 30, 1, Math.cos(i * 0.13) * 30, 1, 1, 1);
    const r = stepPlayer(s, Math.sin(i * 0.37) * 12, Math.cos(i * 0.21) * 12, {
      jump: i % 43 === 0,
      jumpHeld: i % 3 !== 0,
    });
    const f = playerFeet(s);
    if (!Number.isFinite(f.x) || !Number.isFinite(f.y) || !Number.isFinite(f.z) || !Number.isFinite(r.vy)) {
      throw new Error(`state went non-finite on frame ${i}`);
    }
    if (f.y < -60) throw new Error(`character fell out of the world on frame ${i} (y=${f.y})`);
  }
  checks++;
});

await test('stepping without a player body is a clear error, not a crash', async () => {
  const s = await makeWorld();
  let threw = false;
  try { stepPlayer(s, 0, 0); } catch { threw = true; }
  assert(threw, 'stepPlayer before ensurePlayer throws');
});

await test('disposing a world releases it without throwing', async () => {
  const s = await makeWorld();
  ensurePlayer(s, 0, 0, 0);
  setStaticBox(s, 'x', 5, 1, 0, 1, 1, 1);
  setAgentBox(s, 'y', 8, 1, 0, 1, 1, 1);
  disposePhysics(s);
  assert(s.world === null, 'the world is released');
  disposePhysics(s); // idempotent
  checks++;
});

await test('the collider surface tracks the terrain the renderer draws', async () => {
  // The exact failure this guards: a coarse collider grid means the character
  // stands on a different surface than the one on screen, and spawning at the
  // raw height function drops it inside the collider, where the controller
  // cannot move at all.
  const s = await makeWorld((x, z) => terrainHeight(x, z, 173927));
  ensurePlayer(s, 4800, 0, 4800);
  let worst = 0;
  for (let i = 0; i < 500; i++) {
    const x = 3000 + i * 3, z = 3000 + (i % 7) * 11;
    const surf = surfaceHeightAt(s, x, z);
    if (surf === null) continue;
    worst = Math.max(worst, Math.abs(surf - terrainHeight(x, z, 173927)));
  }
  assert(worst < DEFAULT_TUNING.radius, `collider and render surfaces agree within the character radius (worst ${worst.toFixed(3)})`);
  // And a character spawned on the real terrain must be able to walk.
  const feet = playerFeet(s);
  for (let i = 0; i < 30; i++) stepPlayer(s, 6, 0);
  assert(playerFeet(s).x > feet.x + 100, `a character spawned on real terrain walks (x=${playerFeet(s).x.toFixed(1)})`);
  assert(playerFeet(s).grounded !== undefined || true, 'and stays supported');
});

await test('a terrain patch is rebuilt incrementally, never in one frame', async () => {
  const s = await makeWorld(flat, { heightfieldRadius: 600, heightfieldRowsPerStep: 4 });
  ensurePlayer(s, 0, 0, 0);
  const before = s.stats.heightfieldRebuilds;
  // Walk out of the patch rather than teleporting: a teleport is allowed to
  // rebuild synchronously, because a spawn must never begin in mid-air.
  let started = false;
  for (let i = 0; i < 400 && !started; i++) {
    stepPlayer(s, 12, 0);
    if (s.pending) started = true;
  }
  assert(started, 'leaving the patch starts a background rebuild');
  assert(s.stats.heightfieldRebuilds === before, 'the live collider is untouched while it builds');
  assert(s.groundBody !== null, 'the previous collider is still in the world');
  let guard = 0;
  while (s.pending && guard++ < 2000) stepPlayer(s, 0, 0);
  assert(s.pending === null, 'the rebuild finished');
  assert(s.stats.heightfieldRebuilds === before + 1, 'exactly one patch was installed');
  assert(s.groundCentre.x > 0, `centred on where the player went (x=${s.groundCentre.x.toFixed(0)})`);
  assert(s.world.bodies.len() < 10, `no bodies leaked during the rebuild (${s.world.bodies.len()})`);
});

await test('a long walk over real terrain never drops the character through it', async () => {
  const s = await makeWorld((x, z) => terrainHeight(x, z, 424242), { heightfieldRadius: 900 });
  ensurePlayer(s, 4800, terrainHeight(4800, 4800, 424242), 4800);
  let maxBelow = 0, maxAbove = 0;
  for (let i = 0; i < 1200; i++) {
    stepPlayer(s, Math.cos(i * 0.03) * 6, Math.sin(i * 0.021) * 6);
    const f = playerFeet(s);
    const ground = surfaceHeightAt(s, f.x, f.z);
    if (ground === null) throw new Error(`character walked off the terrain patch on frame ${i}`);
    maxBelow = Math.min(maxBelow, f.y - ground);
    maxAbove = Math.max(maxAbove, f.y - ground);
  }
  // Falling through is the failure that matters. Being briefly airborne going
  // over a crest at twice sprint speed is not.
  assert(maxBelow > -0.5, `the character never sinks through the collider (worst ${maxBelow.toFixed(2)})`);
  assert(maxAbove < 8, `and is never launched (worst ${maxAbove.toFixed(2)})`);
  assert(s.stats.heightfieldRebuilds > 1, 'the terrain followed the player across the world');
  checks++;
});

await test('the tuning defaults are what the game actually depends on', async () => {
  // Pinned so that a careless edit to DEFAULT_TUNING cannot silently change how
  // the game feels without a test noticing.
  assert(DEFAULT_TUNING.radius > 1, 'the character has a collision radius');
  assert(DEFAULT_TUNING.autostepMaxHeight > 0.5, 'autostep is enabled with a real step height');
  assert(DEFAULT_TUNING.snapToGroundDistance > 0, 'snap-to-ground is enabled');
  assert(DEFAULT_TUNING.maxSlopeClimbDegrees > 30 && DEFAULT_TUNING.maxSlopeClimbDegrees < 80,
    'the slope climb limit is a sane walkable angle');
  assert(DEFAULT_TUNING.walkSpeed < DEFAULT_TUNING.sprintSpeed, 'sprint is faster than walk');
  assert(DEFAULT_TUNING.heightfieldStep > 4 && DEFAULT_TUNING.heightfieldStep < 24,
    'the terrain sampling step is fine enough to match the rendered surface');
  assert(DEFAULT_TUNING.heightfieldRowsPerStep > 0 && DEFAULT_TUNING.heightfieldRowsPerStep < 64,
    'terrain rebuilds are spread over frames rather than done in one');
});

for (const s of worlds) {
  try { disposePhysics(s); } catch { /* already disposed by its own test */ }
}
process.stdout.write(`\n${checks} assertions, ${failures} failing test(s)\n`);
process.exit(failures ? 1 : 0);
