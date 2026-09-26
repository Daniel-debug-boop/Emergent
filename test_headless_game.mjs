/**
 * EMERGENT runtime tests — the real game, running headlessly.
 *
 * These are not mocks of the engine: `game3d.js` is imported unmodified and its
 * actual frame loop, simulation, streaming and WebGL draw calls execute inside
 * Node against the validating GL surface in tools/headless_runtime.mjs. A
 * passing run therefore means the shipped runtime boots, renders and simulates
 * without a thrown exception, a validation error, or non-finite geometry.
 *
 * Run: `npm run test:runtime`
 */
import {
  setupHeadless, resetHarness, fireGlobal, pumpFrames, currentGL, overlayCtxOf
} from './tools/headless_runtime.mjs';

// ---------------------------------------------------------------------------
// Minimal test runner (no dependencies; the project ships no test framework)
// ---------------------------------------------------------------------------

const results = [];
let currentTest = null;

async function test(name, fn) {
  currentTest = { name, checks: 0 };
  results.push(currentTest);
  try {
    await fn();
  } catch (err) {
    currentTest.error = err.stack || String(err);
  }
}

function assert(condition, message) {
  currentTest.checks++;
  if (!condition) throw new Error(message);
}

function assertEqual(actual, expected, message) {
  assert(
    actual === expected,
    `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`
  );
}

function assertClose(actual, expected, tolerance, message) {
  assert(
    Number.isFinite(actual) && Math.abs(actual - expected) <= tolerance,
    `${message} (expected ${expected} +/- ${tolerance}, got ${actual})`
  );
}

/** Look up a business record from the live world. */
function world_business(game, id) {
  return game.world.businesses.find(b => b.id === id);
}

// ---------------------------------------------------------------------------
// Boot helper
// ---------------------------------------------------------------------------

let bootCounter = 0;

/**
 * Boot a fresh instance of the unmodified game module.
 * The module is side-effectful on import, so the URL is cache-busted to force a
 * real re-evaluation; its `./world.mjs` dependency stays cached and shared.
 */
async function boot({ query = '', frames = 0, width = 1280, height = 720 } = {}) {
  resetHarness();
  setupHeadless({ url: `http://localhost:8765/index.html${query}`, width, height });
  await import(`./game3d.js?boot=${bootCounter++}`);
  if (frames) await pumpFrames(frames);
  return { gl: currentGL(), game: globalThis.EMERGENT, doc: globalThis.document };
}

/** Assert the WebGL contract held for the frames that were drawn. */
function assertGLHealthy(gl, label) {
  assert(gl, `${label}: expected a WebGL2 context`);
  assertEqual(gl.stats.errors.length, 0, `${label}: WebGL validation errors: ${gl.stats.errors[0]}`);
  assertEqual(gl.stats.nanUploads, 0, `${label}: non-finite vertex data was uploaded`);
  assertEqual(
    [...gl.unmodelled].length, 0,
    `${label}: GL entry points not modelled by the harness: ${[...gl.unmodelled].join(', ')}`
  );
  assert(gl.stats.drawCalls > 0, `${label}: no draw calls were issued`);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

await test('boots, renders and reports a clean WebGL frame', async () => {
  const { gl } = await boot({ frames: 90 });
  assertGLHealthy(gl, 'boot');
  const game = globalThis.EMERGENT;
  assert(game.running, 'game should be running after boot');
  assert(game.world && game.world.buildings.length > 0, 'world should be generated');
  assert(game.stats.staticVertexCount > 1000, 'static geometry should be built');
  assert(game.stats.dynamicVertexCount > 0, 'dynamic geometry should be built');
  assert(gl.stats.drawnVertices > 10000, 'renderer should have drawn real geometry');
  assert(game.stats.staticChunks > 1, `static geometry should be split into cells (got ${game.stats.staticChunks})`);
  assert(game.stats.visibleChunks > 0, 'at least one static cell should be visible');
  assert(game.stats.culledChunks > 0, `frustum culling should reject cells (got ${game.stats.culledChunks})`);
});

await test('static cells are culled by the frustum, not merely drawn', async () => {
  // The performance claim, measured rather than asserted from a screenshot.
  //
  // What matters is not that culling exists but that it *removes work*. If
  // every cell were submitted this would still pass a "culledChunks > 0"
  // check as long as one far cell fell outside, so the assertion here is on
  // submitted vertices: the GPU must receive a fraction of what was uploaded,
  // and turning the camera must change which fraction.
  const { gl } = await boot({ frames: 120 });
  assertGLHealthy(gl, 'culling');
  const game = globalThis.EMERGENT;
  const s = game.stats;

  assert(s.staticChunks > 4, `enough cells to cull (${s.staticChunks})`);
  assert(s.culledChunks > 0, 'some cells fall outside the frustum');
  assert(s.visibleChunks > 0, 'some cells are inside the frustum');
  assert(
    s.submittedStaticVertices < s.staticVertexCount,
    `culling must submit less than the whole buffer (${Math.round(s.submittedStaticVertices)} of ${Math.round(s.staticVertexCount)})`
  );
  // A 60-degree vertical field of view over a 1000-unit radius cannot
  // legitimately contain every cell. If it does, the frustum is wrong in the
  // permissive direction and nothing is being saved.
  assert(
    s.visibleChunks < s.staticChunks,
    `not every cell can be on screen at once (${s.visibleChunks}/${s.staticChunks})`
  );

  // Turning the camera 180 degrees must change the visible set. A culler that
  // ignored its matrix and always returned true would pass every check above,
  // so this is the assertion that gives the rest their meaning.
  // The game starts at yaw = PI, so 0 is the opposite direction.
  const before = new Set(s.visibleChunkKeys);
  const beforeSubmitted = s.submittedStaticVertices;
  game.look(0, -0.24);
  await pumpFrames(30);
  const after = game.stats;
  const afterSet = new Set(after.visibleChunkKeys);
  assert(after.visibleChunks > 0, 'cells are still visible after turning around');
  const shared = [...afterSet].filter(k => before.has(k)).length;
  assert(
    shared < afterSet.size,
    `turning around must reveal different cells (${shared} of ${afterSet.size} cells unchanged)`
  );
  assert(
    after.submittedStaticVertices !== beforeSubmitted || afterSet.size !== before.size,
    'turning the camera must change what is submitted to the GPU'
  );
});

await test('survives a long run without NaN geometry or lost frames', async () => {
  const { gl } = await boot({ frames: 600 });
  assertGLHealthy(gl, 'long run');
  // Draw calls are now one per visible static cell plus the shadow pass and
  // the dynamic pass, so the count is a function of the frustum. What has to
  // hold is the shape: more than the two non-culled passes, and never zero.
  const game = globalThis.EMERGENT;
  const perFrame = gl.stats.drawCalls / 600;
  assert(perFrame >= 2, `at least the shadow and dynamic passes run every frame (${perFrame.toFixed(2)} draws/frame)`);
  assertEqual(
    Math.round(gl.stats.drawCalls),
    Math.round(600 * (game.stats.visibleChunks + 2)),
    'draw calls should equal visible cells plus shadow and dynamic, each frame'
  );
  // The sim ran ~20s of virtual time: clock, weather, economy, traffic and NPCs
  // all advanced. Nothing may become non-finite while they do.
  const w = globalThis.EMERGENT.world;
  assert(Number.isFinite(w.time) && Number.isFinite(w.economy) && Number.isFinite(w.heat),
    'world clock/economy/heat must stay finite');
  for (const b of w.businesses) {
    if (!Number.isFinite(b.stock) || !Number.isFinite(b.price)) {
      throw new Error(`business ${b.id} has non-finite stock/price`);
    }
  }
  for (const n of w.npcs) {
    if (!Number.isFinite(n.x) || !Number.isFinite(n.z) || !Number.isFinite(n.energy)) {
      throw new Error(`npc ${n.id} has non-finite state`);
    }
  }
  for (const c of w.cars) {
    if (!Number.isFinite(c.x) || !Number.isFinite(c.z) || !Number.isFinite(c.speed)) {
      throw new Error(`car ${c.id} has non-finite state`);
    }
  }
});

await test('the player moves through keyboard input and the world reacts', async () => {
  const { gl } = await boot({ frames: 10 });
  const game = globalThis.EMERGENT;
  const before = { x: game.player.x, z: game.player.z };
  fireGlobal('keydown', { key: 'w' });
  await pumpFrames(120);
  fireGlobal('keyup', { key: 'w' });
  const after = { x: game.player.x, z: game.player.z };
  const moved = Math.hypot(after.x - before.x, after.z - before.z);
  assert(moved > 1, `player should have walked (moved ${moved.toFixed(2)}m)`);
  assertGLHealthy(gl, 'walking');
});

await test('crossing the world streams chunks in and out', async () => {
  const { gl } = await boot({ frames: 10 });
  const game = globalThis.EMERGENT;
  const generatedBefore = game.stats.streamGenerated;
  const residentBefore = game.stats.residentChunks;
  // Teleport across the map repeatedly: each jump forces a full residency
  // refresh, which must generate new chunks and evict the old ones.
  for (let i = 0; i < 6; i++) {
    game.teleport(600 + i * 1300, 600 + (i % 3) * 1200);
    await pumpFrames(2);
  }
  const after = game.stats;
  assert(after.streamGenerated > generatedBefore, 'teleporting should generate stream chunks');
  assert(after.streamFreed > 0, 'teleporting should evict chunks that left the radius');
  assert(after.residentChunks > 0 && after.residentChunks < 400,
    `resident chunk set should be bounded, got ${after.residentChunks}`);
  assertGLHealthy(gl, 'streaming');
});

await test('quality levels change the streaming radius and rebuild cleanly', async () => {
  const { gl } = await boot({ frames: 10 });
  const game = globalThis.EMERGENT;
  const seen = new Set();
  for (let i = 0; i < 4; i++) {
    seen.add(game.streamRadius);
    fireGlobal('keydown', { key: 'q' });
    await pumpFrames(4);
  }
  assertEqual(seen.size, 4, 'each quality level should have a distinct stream radius');
  assertEqual(game.renderer.quality, 'HIGH', 'four presses from HIGH should wrap back to HIGH');
  assertGLHealthy(gl, 'quality cycle');
});

await test('renderer mode toggles between standard and adaptive', async () => {
  const { gl } = await boot({ frames: 20 });
  const game = globalThis.EMERGENT;
  const adaptive = game.renderer.mode;
  fireGlobal('keydown', { key: 't' });
  await pumpFrames(5);
  const other = game.renderer.mode;
  assert(adaptive !== other, 'T should switch renderer mode');
  // Adaptive mode reuses per-NPC state instead of rebuilding every frame.
  await pumpFrames(180);
  const stats = game.stats;
  if (other === 'ADAPTIVE') {
    assert(stats.reused > 0, 'adaptive mode should reuse cached per-NPC state');
  } else {
    assert(stats.recomputed > 0, 'standard mode should recompute every visible agent');
  }
  assertGLHealthy(gl, 'renderer toggle');
});

await test('a delivery mission is offered, picked up and delivered', async () => {
  const { gl } = await boot({ frames: 20 });
  const game = globalThis.EMERGENT;

  // A mission is generated from world state after a short delay.
  for (let i = 0; i < 12 && !game.mission; i++) await pumpFrames(30);
  const mission = game.mission;
  assert(mission, 'the world should offer a delivery mission');
  assertEqual(mission.stage, 'pickup', 'a fresh mission starts at the pickup stage');
  assert(mission.reward > 0, 'a mission should pay something');

  // Walk to the pickup building and interact.
  const pickup = game.missionBuildingFor(mission);
  assert(pickup, 'mission should reference a real building');
  const sourceStockBefore = world_business(game, mission.sourceId).stock;
  game.teleport(pickup.x, pickup.z + 4);
  await pumpFrames(4);
  const moneyBefore = game.player.money;
  fireGlobal('keydown', { key: 'e' });
  await pumpFrames(4);
  assertEqual(game.mission.stage, 'delivery', 'interacting at the source starts delivery');
  assertEqual(game.player.money, moneyBefore, 'pickup should not pay out yet');
  assert(world_business(game, mission.sourceId).stock < sourceStockBefore,
    'pickup should remove crates from the source business');

  // Deliver to the target.
  const target = game.missionBuildingFor(game.mission);
  assert(target, 'delivery mission should reference a target building');
  const targetStockBefore = world_business(game, mission.targetId).stock;
  game.teleport(target.x, target.z + 4);
  await pumpFrames(4);
  fireGlobal('keydown', { key: 'e' });
  await pumpFrames(4);
  assertEqual(game.mission, null, 'completing a delivery clears the mission');
  assertEqual(game.player.missionsCompleted, 1, 'delivery should count as a completed mission');
  assert(game.player.money > moneyBefore, 'delivery should pay the reward');
  assert(world_business(game, mission.targetId).stock > targetStockBefore,
    'delivery should restock the target business');
  assertGLHealthy(gl, 'mission flow');
});

await test('save and load round-trips simulation state', async () => {
  const { gl } = await boot({ frames: 20 });
  const game = globalThis.EMERGENT;
  for (let i = 0; i < 12 && !game.mission; i++) await pumpFrames(30);
  await pumpFrames(60);

  const before = {
    money: game.player.money,
    x: game.player.x,
    z: game.player.z,
    discoveries: game.player.discoveries,
    time: game.world.time,
    weather: game.world.weather,
    businessStock: game.world.businesses.slice(0, 20).map(b => b.stock)
  };
  game.save();
  const saved = globalThis.localStorage.getItem('emergent3d.save.v3');
  assert(saved, 'saveGame should write to localStorage');
  const parsed = JSON.parse(saved);
  assertEqual(parsed.seed, game.world.seed, 'a save must record the world it belongs to');

  // Diverge the live state, then load the save back and require restoration.
  game.teleport(before.x + 400, before.z + 400);
  game.player.money = 1;
  game.world.businesses[0].stock = 0;
  await pumpFrames(30);
  assert(game.load(), 'load should restore a save for the current world seed');

  assertClose(game.player.x, before.x, 1e-6, 'player position should be restored');
  assertClose(game.player.z, before.z, 1e-6, 'player position should be restored');
  assertEqual(game.player.money, before.money, 'money should be restored');
  assertEqual(game.player.discoveries, before.discoveries, 'discoveries should be restored');
  assertClose(game.world.time, before.time, 1e-6, 'world clock should be restored');
  assertEqual(game.world.weather, before.weather, 'weather should be restored');
  const restored = game.world.businesses.slice(0, 20).map(b => b.stock);
  assertEqual(JSON.stringify(restored), JSON.stringify(before.businessStock),
    'business stock should be restored');

  // A save from a different world must never be applied to this one.
  globalThis.localStorage.setItem('emergent3d.save.v3', JSON.stringify({ ...parsed, seed: parsed.seed + 1 }));
  assert(!game.load(), 'load must refuse a save belonging to a different world seed');
  assertGLHealthy(gl, 'save/load');
});

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

const failed = results.filter(r => r.error);
const totalChecks = results.reduce((n, r) => n + r.checks, 0);
for (const r of results) {
  const status = r.error ? 'FAIL' : `ok (${r.checks} checks)`;
  process.stdout.write(`${r.error ? '✗' : '✓'} ${r.name} — ${status}\n`);
  if (r.error) process.stdout.write(`  ${r.error.split('\n').slice(0, 4).join('\n  ')}\n`);
}
process.stdout.write(
  `\n${results.length - failed.length}/${results.length} runtime tests passed, ${totalChecks} assertions\n`
);
process.exit(failed.length ? 1 : 0);
