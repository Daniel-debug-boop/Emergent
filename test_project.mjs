import assert from 'node:assert/strict';
import fs from 'node:fs';
import { performance } from 'node:perf_hooks';
import { generateWorld, terrainHeight } from './world.mjs';

const seeds=[173927,173928,424242];
const timings=[];
for(const seed of seeds){
  const t0=performance.now();
  const w=generateWorld(seed);
  const ms=performance.now()-t0;
  timings.push({seed,ms:+ms.toFixed(3),buildings:w.buildings.length,trees:w.trees.length,businesses:w.businesses.length,npcs:w.npcs.length,cars:w.cars.length,roads:w.roads.length,districts:w.districts.length});
}
const source=fs.readFileSync('./game3d.js','utf8');
const index=fs.readFileSync('./index.html','utf8');
for(const token of ['webgl2','sceneProg','shadowProg','buildStaticScene','buildDynamicScene','updateSimulation','saveGame','loadGame']) assert(source.includes(token));
assert(index.includes('game3d.js') && index.includes('canvas'));
const sameA=generateWorld(173927), sameB=generateWorld(173927), other=generateWorld(173928);
assert.deepEqual(sameA.buildings,sameB.buildings);
assert.notDeepEqual(sameA.buildings,other.buildings);
assert.equal(sameA.npcs.length,520);assert.equal(sameA.cars.length,150);assert(sameA.businesses.length>100);
for(const [x,z] of [[0,0],[4800,4800],[9500,9500]]) assert(Number.isFinite(terrainHeight(x,z,173927)));
console.log(JSON.stringify({ok:true,timings}));
