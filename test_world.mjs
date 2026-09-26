import assert from 'node:assert/strict';
import { generateWorld, terrainHeight } from './world.mjs';
const a = generateWorld(173927);
const b = generateWorld(173927);
assert.deepEqual(a.regions, b.regions);
assert.deepEqual(a.roads, b.roads);
assert.deepEqual(a.buildings, b.buildings);
assert.deepEqual(a.businesses, b.businesses);
assert.deepEqual(a.npcs, b.npcs);
assert.deepEqual(a.cars, b.cars);
assert.notDeepEqual(a.buildings, generateWorld(173928).buildings);
assert(a.regions.length === 64);
assert(a.buildings.length > 200);
assert(a.npcs.length === 520);
assert(a.cars.length === 150);
assert(a.businesses.length > 20);
for (const p of [[0,0],[2400,2400],[4800,4800],[9200,9200]]) assert(Number.isFinite(terrainHeight(p[0],p[1],173927)));

// --- terrain coherence -----------------------------------------------------
//
// These guard a real defect that shipped and was invisible in a still frame:
// `fbm` used to sample the lattice hash directly, so `terrainHeight` returned
// uncorrelated values — white noise, not terrain. A coarse mesh hid it; a
// character controller walking across it could not be tuned at all, because
// there was no walkable slope to tune against.

const SEEDS = [173927, 173928, 424242];
for (const seed of SEEDS) {
  // Continuity: over a four-unit step the surface may not jump. A player moves
  // about three units per frame, so this is the largest change they can miss.
  let maxStep = 0;
  let at = null;
  for (let i = 0; i < 4000; i++) {
    const x = (i * 977) % 9400, z = (i * 613) % 9400;
    const d = Math.abs(terrainHeight(x, z, seed) - terrainHeight(x + 4, z, seed));
    if (d > maxStep) { maxStep = d; at = [x, z]; }
  }
  assert(maxStep < 1.0, `seed ${seed}: terrain is continuous, max change over 4 units is ${maxStep.toFixed(3)} at ${at}`);

  // Walkability: no slope anywhere steeper than the character can climb. The
  // controller's limit is 52 degrees, and terrain that touches it everywhere
  // is not terrain a player can cross.
  let maxSlopeDeg = 0;
  for (let i = 0; i < 4000; i++) {
    const x = (i * 1301) % 9400, z = (i * 787) % 9400, d = 4;
    const gx = (terrainHeight(x + d, z, seed) - terrainHeight(x - d, z, seed)) / (2 * d);
    const gz = (terrainHeight(x, z + d, seed) - terrainHeight(x, z - d, seed)) / (2 * d);
    maxSlopeDeg = Math.max(maxSlopeDeg, Math.atan(Math.hypot(gx, gz)) * 180 / Math.PI);
  }
  assert(maxSlopeDeg < 40, `seed ${seed}: terrain is walkable, max slope is ${maxSlopeDeg.toFixed(1)} degrees`);

  // It must still have relief. Flat-but-featureless is a different failure.
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < 2000; i++) {
    const h = terrainHeight((i * 331) % 9400, (i * 457) % 9400, seed);
    lo = Math.min(lo, h); hi = Math.max(hi, h);
  }
  assert(hi - lo > 4, `seed ${seed}: terrain has vertical relief (${(hi - lo).toFixed(1)} units)`);
}
console.log(JSON.stringify({ok:true,regions:a.regions.length,districts:a.districts.length,roads:a.roads.length,buildings:a.buildings.length,trees:a.trees.length,businesses:a.businesses.length,npcs:a.npcs.length,cars:a.cars.length}));
