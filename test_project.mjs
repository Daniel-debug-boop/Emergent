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
// The material system has to be *wired into the shader*, not merely uploaded.
// The whole material pipeline once ran green against a fragment shader that
// read only position, normal and colour, so the tokens the shader needs are
// asserted here at the top of the project test where a regression is cheapest
// to see. test_shaders.mjs proves the same thing structurally; this is the
// coarse tripwire that names the specific wiring.
for(const token of ['sampler2DArray uAlbedoTex','sampler2DArray uNormalTex','sampler2DArray uArmTex',
  'layout(location=3) in float m','uniform4fv(loc.uMatA0','uniform1i(loc.uAlbedoTex','uMaterialsReady'])
  assert(source.includes(token),`game3d.js is missing ${token} — the material system is not reachable from the shader`);
assert(index.includes('game3d.js') && index.includes('canvas'));
const sameA=generateWorld(173927), sameB=generateWorld(173927), other=generateWorld(173928);
assert.deepEqual(sameA.buildings,sameB.buildings);
assert.notDeepEqual(sameA.buildings,other.buildings);
assert.equal(sameA.npcs.length,520);assert.equal(sameA.cars.length,150);assert(sameA.businesses.length>100);
for(const [x,z] of [[0,0],[4800,4800],[9500,9500]]) assert(Number.isFinite(terrainHeight(x,z,173927)));
console.log(JSON.stringify({ok:true,timings}));
