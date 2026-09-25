import { clamp, rng, hash2, terrainHeight, generateWorld, districtAt } from './world.mjs';
import { matrixPerspective, lookAt, mul, transformPoint } from './math3d.mjs';

'use strict';

const canvas = document.createElement('canvas');
canvas.id = 'game-canvas';
document.body.appendChild(canvas);
const gl = canvas.getContext('webgl2', { antialias: true, alpha: false, powerPreference: 'high-performance' });
if (!gl) throw new Error('WebGL2 unavailable');

// Screen-space mission marker overlay: a 2D canvas layered above the 3D view
// but below the HUD. It never captures input and is resized with the 3D canvas.
const overlay = document.createElement('canvas');
overlay.id = 'mission-overlay';
overlay.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;pointer-events:none;z-index:4';
document.body.appendChild(overlay);
const overlayCtx = overlay.getContext('2d');

const $ = id => document.getElementById(id);
const MODES = ['STANDARD', 'ADAPTIVE'];
const QUALITY = ['LOW', 'MEDIUM', 'HIGH', 'ULTRA'];
const SAVE_KEY = 'emergent3d.save.v3';
const WORLD_SEED_DEFAULT = 173927;

let seed = WORLD_SEED_DEFAULT;
let rand = rng(seed);
let world = null;
let player = null;
let rendererMode = 1;
let qualityLevel = 2;
let gameRunning = false;
let last = performance.now();
let elapsed = 0;
let streamKey = '';
let streamOps = 0;
let weatherTimer = 80;
let economyTimer = 0;
let missionTimer = 0;
let saveTimer = 0;
let eventTimer = 0;
let staticVertexCount = 0;
let dynamicVertexCount = 0;
let shadowVertexCount = 0;
let renderMs = 0;
let fps = 0;
let accFrames = 0;
let accSeconds = 0;
let recomputed = 0;
let reused = 0;
let visibleObjects = 0;
let simulatedNpcCount = 0;
let streamGenerated = 0;
let streamFreed = 0;
let currentMission = null;
let toastTimer = 0;
let debugVisible = false;
let firstPerson = false;
let yaw = Math.PI;
let pitch = -0.24;
let pointerLocked = false;
let keys = Object.create(null);
let touchMove = { x: 0, y: 0 };
let touchSprint = false;
let inputLocked = true;
let audioCtx = null;
let audioNodes = null;
const QUERY = new URLSearchParams(location.search);
const BENCH_MODE = QUERY.get('benchmark');
const BENCH_SEED = Number(QUERY.get('seed'));
const BENCH_QUALITY = QUERY.get('quality');
let benchStart = 0, benchFrames = 0, benchRenderMs = 0, benchRebuilds = 0, benchDynamicMs = 0, benchRecomputed = 0, benchReused = 0, benchVisible = 0, benchDone = false;
let dynamicBuildTime = -Infinity, lastDynamicX = 0, lastDynamicZ = 0, dynamicBuilds = 0, lastDynamicObjects = 0;
let lastPV = null; // Most recent view-projection matrix, shared with the 2D mission marker.

const camera = { x: 4800, y: 12, z: 4800, yaw: Math.PI, pitch: -0.24 };
const renderAgentState = new Map();
const worldIndex = { buildings: new Map(), trees: new Map(), roads: new Map(), businesses: new Map(), businessById: new Map(), buildingById: new Map() };
const streamResidency = { active: new Set(), chunks: new Map(), generated: 0, evicted: 0, lastBuildMs: 0 };

function chunkKey(cx, cz) { return `${cx},${cz}`; }

function refreshStreamResidency(radius) {
  const t0 = performance.now();
  const wanted = new Set(cellsInRadius(player.x, player.z, radius + 500).map(([cx, cz]) => chunkKey(cx, cz)));
  for (const key of wanted) {
    if (streamResidency.chunks.has(key)) continue;
    const [cx, cz] = key.split(',').map(Number);
    const chunk = {
      buildings: worldIndex.buildings.get(key) || [],
      trees: worldIndex.trees.get(key) || [],
      roads: worldIndex.roads.get(key) || [],
      businesses: worldIndex.businesses.get(key) || []
    };
    streamResidency.chunks.set(key, chunk);
    streamResidency.generated++;
  }
  for (const key of streamResidency.active) {
    if (wanted.has(key)) continue;
    streamResidency.chunks.delete(key);
    streamResidency.evicted++;
  }
  streamResidency.active = wanted;
  streamResidency.lastBuildMs = performance.now() - t0;
  return wanted;
}


function showToast(text, seconds = 3) {
  $('toast').textContent = text;
  $('toast').classList.add('show');
  toastTimer = seconds;
}

function clearToast(dt) {
  if (toastTimer > 0) { toastTimer -= dt; if (toastTimer <= 0) $('toast').classList.remove('show'); }
}

function initializeAudio() {
  if (audioCtx) return;
  try {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const master = audioCtx.createGain(); master.gain.value = 0.035; master.connect(audioCtx.destination);
    const osc = audioCtx.createOscillator(); osc.type = 'sine'; osc.frequency.value = 62;
    const low = audioCtx.createBiquadFilter(); low.type = 'lowpass'; low.frequency.value = 260;
    osc.connect(low); low.connect(master); osc.start();
    const buffer = audioCtx.createBuffer(1, audioCtx.sampleRate * 1, audioCtx.sampleRate);
    const data = buffer.getChannelData(0); for (let i=0;i<data.length;i++) data[i]=(Math.random()*2-1)*0.32;
    const noise = audioCtx.createBufferSource(); noise.buffer=buffer; noise.loop=true;
    const nf = audioCtx.createBiquadFilter(); nf.type='lowpass'; nf.frequency.value=650;
    const ng = audioCtx.createGain(); ng.gain.value=0.004;
    noise.connect(nf); nf.connect(ng); ng.connect(master); noise.start();
    audioNodes = { master, oscGain: master, noiseGain: ng };
  } catch (_) { audioCtx = null; }
}

function beep(freq = 440, duration = 0.08) {
  if (!audioCtx) return;
  const o = audioCtx.createOscillator(), g = audioCtx.createGain();
  o.frequency.value = freq; g.gain.value = 0.035; o.connect(g); g.connect(audioCtx.destination); o.start();
  g.gain.exponentialRampToValueAtTime(0.0001, audioCtx.currentTime + duration); o.stop(audioCtx.currentTime + duration);
}

// Facade palettes, resolved once at module scope. Keeping this out of
// colorForBuilding avoids rebuilding the table for every building on every
// static-scene rebuild, and makes the palette a single shared constant.
const FACADE_PALETTE = Object.freeze({
  house: [[0.54,0.43,0.34],[0.42,0.33,0.28],[0.62,0.56,0.45]],
  shop: [[0.52,0.34,0.19],[0.68,0.50,0.27],[0.42,0.38,0.34]],
  tower: [[0.32,0.38,0.46],[0.43,0.46,0.52],[0.27,0.31,0.37]],
  warehouse: [[0.34,0.37,0.34],[0.43,0.40,0.35],[0.29,0.34,0.33]]
});

function colorForBuilding(b) {
  const p = FACADE_PALETTE[b.kind] || FACADE_PALETTE.house;
  // b.facade is a uniform float in [0,1); scale it into the palette range and
  // floor it. A bare `% p.length` would index with a fraction and yield
  // undefined, which is a hard crash on the very first building.
  const t = Math.min(p.length - 1, Math.floor(b.facade * p.length));
  const skew = (b.seed - 0.5) * 0.07;
  return [clamp(p[t][0] + skew, 0.15, 0.85), clamp(p[t][1] + skew, 0.15, 0.85), clamp(p[t][2] + skew, 0.15, 0.85)];
}

function pushV(arr, p, n, c) { arr.push(p[0],p[1],p[2], n[0],n[1],n[2], c[0],c[1],c[2]); }

function pushQuad(arr, a,b,c,d,n,color) {
  pushV(arr,a,n,color); pushV(arr,b,n,color); pushV(arr,c,n,color);
  pushV(arr,a,n,color); pushV(arr,c,n,color); pushV(arr,d,n,color);
}

function pushBox(arr,x,y,z,w,h,d,color,topColor=color) {
  const x0=x-w/2,x1=x+w/2,y0=y,y1=y+h,z0=z-d/2,z1=z+d/2;
  pushQuad(arr,[x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0],[0,0,-1],color);
  pushQuad(arr,[x1,y0,z1],[x0,y0,z1],[x0,y1,z1],[x1,y1,z1],[0,0,1],color);
  pushQuad(arr,[x0,y0,z1],[x0,y0,z0],[x0,y1,z0],[x0,y1,z1],[-1,0,0],color);
  pushQuad(arr,[x1,y0,z0],[x1,y0,z1],[x1,y1,z1],[x1,y1,z0],[1,0,0],color);
  pushQuad(arr,[x0,y1,z0],[x1,y1,z0],[x1,y1,z1],[x0,y1,z1],[0,1,0],topColor);
  pushQuad(arr,[x0,y0,z1],[x1,y0,z1],[x1,y0,z0],[x0,y0,z0],[0,-1,0],color);
}

function pushCylinder(arr,x,y,z,r,h,color,segments=8) {
  const top=[x,y+h,z], bot=[x,y,z];
  for(let i=0;i<segments;i++){
    const a=i*Math.PI*2/segments, b=(i+1)*Math.PI*2/segments;
    const p0=[x+Math.cos(a)*r,y,z+Math.sin(a)*r], p1=[x+Math.cos(b)*r,y,z+Math.sin(b)*r];
    const q0=[p0[0],y+h,p0[2]], q1=[p1[0],y+h,p1[2]];
    const n0=[Math.cos(a),0,Math.sin(a)], n1=[Math.cos(b),0,Math.sin(b)];
    pushV(arr,p0,n0,color);pushV(arr,p1,n1,color);pushV(arr,q1,n1,color);
    pushV(arr,p0,n0,color);pushV(arr,q1,n1,color);pushV(arr,q0,n0,color);
    pushV(arr,top,[0,1,0],color);pushV(arr,q0,[0,1,0],color);pushV(arr,q1,[0,1,0],color);
  }
}

function pushLowPolyTree(arr,x,y,z,s,type,seed) {
  const trunk=[0.25,0.16,0.09]; pushCylinder(arr,x,y,z,0.18*s,1.7*s,trunk,6);
  const base = type==='pine'?[0.08,0.25,0.13]:type==='broadleaf'?[0.14,0.36,0.19]:[0.10,0.31,0.15];
  // Per-tree brightness variation in [0.85, 1.15], derived from the tree's
  // own seed so the canopy reads as varied but stays deterministic.
  const variation=0.85+((seed*0.3)%0.3);
  const tint=(v,k=1)=>clamp(v*variation*k,0.05,1);
  const canopy=[tint(base[0]),tint(base[1]),tint(base[2])];
  if(type==='pine'){
    pushCylinder(arr,x,y+0.8*s,z,0.9*s,2.2*s,canopy,7);
    pushCylinder(arr,x,y+1.8*s,z,0.62*s,1.9*s,[tint(base[0],1.12),tint(base[1],1.12),tint(base[2],1.12)],7);
  } else {
    pushCylinder(arr,x,y+1.0*s,z,0.92*s,1.35*s,canopy,7);
    pushCylinder(arr,x+0.28*s,y+1.6*s,z-0.16*s,0.68*s,1.0*s,[tint(base[0],1.08),tint(base[1],1.08),tint(base[2],1.08)],7);
  }
}

function pushRoad(arr, r) {
  const y=terrainHeight(r.x+r.w/2,r.z+r.d/2,seed)+0.07;
  const asphalt=r.main>=2?[0.07,0.08,0.09]:r.main===1?[0.10,0.11,0.12]:[0.13,0.13,0.13];
  pushBox(arr,r.x+r.w/2,y,r.z+r.d/2,r.w,0.18,r.d,asphalt,asphalt);
  const sidewalk=[0.28,0.28,0.26];
  const side=2.2;
  if(r.w<r.d){
    pushBox(arr,r.x-2,y+0.03,r.z+r.d/2,side,0.08,r.d,sidewalk);
    pushBox(arr,r.x+r.w+2,y+0.03,r.z+r.d/2,side,0.08,r.d,sidewalk);
  } else {
    pushBox(arr,r.x+r.w/2,y+0.03,r.z-2,r.w,0.08,side,sidewalk);
    pushBox(arr,r.x+r.w/2,y+0.03,r.z+r.d+2,r.w,0.08,side,sidewalk);
  }
}

function pushBuilding(arr,b,detail) {
  const y=terrainHeight(b.x,b.z,seed);
  const c=colorForBuilding(b);
  const top=[clamp(c[0]+0.08,0,1),clamp(c[1]+0.08,0,1),clamp(c[2]+0.08,0,1)];
  pushBox(arr,b.x,y,b.z,b.w,b.h,b.d,c,top);
  const roof=[0.12,0.13,0.15];
  if(b.kind!=='tower' && detail>0.3) {
    pushBox(arr,b.x,y+b.h+0.35,b.z,b.w*0.92,0.7,b.d*0.92,roof,roof);
  }
  if(detail>0.55) {
    const glass=[0.14,0.25,0.31];
    const rows=Math.min(b.floors,6);
    const cols=Math.max(1,Math.floor(b.w/15));
    for(let f=0;f<rows;f++) for(let col=0;col<cols;col++) {
      if((col+f+Math.floor(b.seed*10))%5===0) continue;
      const wx=b.x-b.w/2+7+(col*(b.w-14)/Math.max(1,cols-1));
      const wz=b.z-b.d/2-0.03;
      const wy=y+3.0+f*(b.h-4)/Math.max(1,rows);
      pushBox(arr,wx,wy,wz,Math.min(6,b.w/cols*0.45),1.4,0.05,glass,glass);
    }
    const door=[0.18,0.11,0.07];
    pushBox(arr,b.x,y+0.8,b.z+b.d/2+0.03,1.5,2.1,0.08,door,door);
    if(b.sign) pushBox(arr,b.x,y+3.1,b.z+b.d/2+0.05,4.5,0.7,0.08,[0.75,0.48,0.16]);
  }
  // A small service alley/vent for industrial structures makes warehouses visually distinct.
  if(b.kind==='warehouse' && detail>0.4) pushBox(arr,b.x+b.w*0.28,y+2.5,b.z+b.d/2+0.03,7,4,0.1,[0.23,0.27,0.27]);
}

function pushStreetFurniture(arr,x,z,detail) {
  if(detail<0.55) return;
  const y=terrainHeight(x,z,seed)+0.09;
  pushCylinder(arr,x,y,z,0.08,3.1,[0.16,0.17,0.18],6);
  pushBox(arr,x,y+3.1,z,0.28,0.08,0.28,[0.62,0.60,0.45]);
  if(detail>0.8) pushBox(arr,x,y+1.2,z,0.8,0.5,0.16,[0.20,0.20,0.18]);
}

/**
 * Append a low-poly character to `arr`.
 * Takes the fields it needs by value rather than an NPC object: the adaptive
 * renderer rebuilds this for every visible resident many times a second, and
 * spreading a whole NPC (identity, schedule, memory array) to supply five
 * scalars allocated a throwaway object per agent per rebuild.
 */
function pushNpc(arr,x,z,id,job,activity,detail) {
  const y=terrainHeight(x,z,seed);
  const skin=[0.56+0.12*(id%4)/3,0.36+0.14*(id%5)/4,0.25+0.16*(id%3)/2];
  const shirt=job==='worker'?[0.72,0.46,0.28]:job==='merchant'?[0.32,0.55,0.72]:job==='service'?[0.30,0.63,0.45]:[0.54,0.39,0.67];
  const bob=Math.sin(elapsed*6+id)*0.025*(activity==='travel'?1:0.2);
  pushBox(arr,x,y+bob,z,0.62,1.10,0.40,shirt,shirt);
  pushCylinder(arr,x,y+1.1+bob,z,0.23,0.48,skin,7);
  if(detail>0.35){
    const leg= [0.12,0.13,0.15];
    pushBox(arr,x-0.17,y-0.02,z,0.16,0.55,0.28,leg,leg);
    pushBox(arr,x+0.17,y-0.02,z,0.16,0.55,0.28,leg,leg);
    if(detail>0.65){
      pushBox(arr,x-0.40,y+0.62+bob,z,0.14,0.55,0.14,shirt,shirt);
      pushBox(arr,x+0.40,y+0.62+bob,z,0.14,0.55,0.14,shirt,shirt);
    }
  }
}

function pushCar(arr,c) {
  const y=terrainHeight(c.x,c.z,seed)+0.35;
  const col=c.color;
  pushBox(arr,c.x,y,c.z,c.horizontal?4.0:2.0,0.65,c.horizontal?2.0:4.0,col,col);
  pushBox(arr,c.x,y+0.55,c.z,c.horizontal?2.1:1.2,0.42,c.horizontal?1.2:2.0,[0.12,0.17,0.19]);
  const wheel=[0.06,0.06,0.07];
  if(c.horizontal){
    for(const dx of [-1.3,1.3]) { pushCylinder(arr,c.x+dx,y-0.05,c.z-0.93,0.23,0.22,wheel,8); pushCylinder(arr,c.x+dx,y-0.05,c.z+0.93,0.23,0.22,wheel,8); }
  } else {
    for(const dz of [-1.3,1.3]) { pushCylinder(arr,c.x-0.93,y-0.05,c.z+dz,0.23,0.22,wheel,8); pushCylinder(arr,c.x+0.93,y-0.05,c.z+dz,0.23,0.22,wheel,8); }
  }
}

function createProgram(vsSource, fsSource) {
  const compile=(type,src)=>{const s=gl.createShader(type);gl.shaderSource(s,src);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(s));return s;};
  const p=gl.createProgram();gl.attachShader(p,compile(gl.VERTEX_SHADER,vsSource));gl.attachShader(p,compile(gl.FRAGMENT_SHADER,fsSource));gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(p));return p;
}

const sceneVS = `#version 300 es
precision highp float;
layout(location=0) in vec3 p; layout(location=1) in vec3 n; layout(location=2) in vec3 c;
uniform mat4 uPV; uniform vec3 uCam; uniform float uTime; uniform float uWater;
out vec3 vN; out vec3 vC; out float vD; out vec3 vP;
void main(){ vec3 q=p; if(uWater>0.5) q.y += sin(q.x*0.025+uTime*1.6)*0.08 + cos(q.z*0.021+uTime)*0.06; gl_Position=uPV*vec4(q,1.0); vN=n; vC=c; vD=distance(q,uCam); vP=q; }`;

const sceneFS = `#version 300 es
precision highp float; in vec3 vN; in vec3 vC; in float vD; in vec3 vP;
uniform vec3 uSun; uniform vec3 uFog; uniform vec3 uAmbient; uniform float uFogDensity; uniform float uNight; uniform float uWater;
out vec4 outC;
void main(){ float nd=max(dot(normalize(vN),normalize(uSun)),0.0); float light=0.28+0.72*nd; vec3 c=vC*(uAmbient+light*0.72); if(uWater>0.5) c=mix(c,vec3(0.08,0.25,0.35),0.42); float fog=1.0-exp(-vD*vD*uFogDensity); c=mix(c,uFog,fog); if(uNight>0.35 && vC.r>0.55 && vC.g>0.32 && uWater<0.5) c+=vec3(0.02,0.016,0.006)*uNight; outC=vec4(c,1.0); }`;

const shadowVS = `#version 300 es
precision highp float; layout(location=0) in vec3 p; uniform mat4 uPV; void main(){gl_Position=uPV*vec4(p,1.0);}`;
const shadowFS = `#version 300 es
precision mediump float; out vec4 outC; void main(){outC=vec4(0.02,0.025,0.03,0.24);}`;

const waterVS = sceneVS; const waterFS = sceneFS;
const sceneProg=createProgram(sceneVS,sceneFS);
const shadowProg=createProgram(shadowVS,shadowFS);

const vao=gl.createVertexArray(); const staticBuf=gl.createBuffer(); const dynamicBuf=gl.createBuffer(); const shadowBuf=gl.createBuffer();
const loc={
  uPV:gl.getUniformLocation(sceneProg,'uPV'),uCam:gl.getUniformLocation(sceneProg,'uCam'),uTime:gl.getUniformLocation(sceneProg,'uTime'),uSun:gl.getUniformLocation(sceneProg,'uSun'),uFog:gl.getUniformLocation(sceneProg,'uFog'),uAmbient:gl.getUniformLocation(sceneProg,'uAmbient'),uFogDensity:gl.getUniformLocation(sceneProg,'uFogDensity'),uNight:gl.getUniformLocation(sceneProg,'uNight'),uWater:gl.getUniformLocation(sceneProg,'uWater'),
  sPV:gl.getUniformLocation(shadowProg,'uPV')
};

gl.bindVertexArray(vao);
gl.bindBuffer(gl.ARRAY_BUFFER, staticBuf);
gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,3,gl.FLOAT,false,36,0);
gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1,3,gl.FLOAT,false,36,12);
gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2,3,gl.FLOAT,false,36,24);
gl.bindVertexArray(null);

// Matrix math (perspective, look-at, multiply, point transform) lives in
// math3d.mjs so it can be unit-tested independently of the GL runtime.

function sunState(){
  const theta=(world.time/24)*Math.PI*2-Math.PI*0.5;
  const sx=Math.cos(theta)*0.65, sy=Math.sin(theta), sz=-0.42;
  const day=clamp((sy+0.08)/1.0,0,1);
  return { dir:[sx,Math.max(0.12,sy),sz], day, night:1-day };
}

function buildWorldIndex() {
  worldIndex.buildings.clear(); worldIndex.trees.clear(); worldIndex.roads.clear();
  worldIndex.businesses.clear(); worldIndex.businessById.clear(); worldIndex.buildingById.clear();
  const cell=480;
  const add=(map,obj,x,z)=>{const k=`${Math.floor(x/cell)},${Math.floor(z/cell)}`;let a=map.get(k);if(!a)map.set(k,a=[]);a.push(obj);};
  for(const b of world.buildings){ add(worldIndex.buildings,b,b.x,b.z); worldIndex.buildingById.set(b.id,b); }
  for(const t of world.trees) add(worldIndex.trees,t,t.x,t.z);
  for(const r of world.roads) add(worldIndex.roads,r,r.x+r.w/2,r.z+r.d/2);
  for(const b of world.businesses){
    const building=world.buildings[b.buildingId];
    worldIndex.businessById.set(b.id,b);
    if(building) add(worldIndex.businesses,b,building.x,building.z);
  }
}

function cellsInRadius(x,z,r){const cell=480,cx=Math.floor(x/cell),cz=Math.floor(z/cell),n=Math.ceil(r/cell);const out=[];for(let yy=cz-n;yy<=cz+n;yy++)for(let xx=cx-n;xx<=cx+n;xx++)out.push([xx,yy]);return out;}
function keyCell(x,z){return `${Math.floor(x/480)},${Math.floor(z/480)}`;}

function terrainChunk(arr,cx,cz,radius,detail){
  const minX=cx-radius,minZ=cz-radius,size=radius*2;
  const step=detail>=0.85?38:detail>=0.55?56:detail>=0.3?78:110;
  for(let z=0;z<size;z+=step) for(let x=0;x<size;x+=step){
    const x0=minX+x,z0=minZ+z,x1=Math.min(minX+x+step,cx+radius),z1=Math.min(minZ+z+step,cz+radius);
    const y00=terrainHeight(x0,z0,seed),y10=terrainHeight(x1,z0,seed),y11=terrainHeight(x1,z1,seed),y01=terrainHeight(x0,z1,seed);
    // Region lookup only depends on the quad origin, and terrainHeight is
    // finite for any input, so the corner clamping is enough.
    const rx=Math.max(0,Math.min(7,Math.floor(clamp(x0,0,9599)/1200)));
    const rz=Math.max(0,Math.min(7,Math.floor(clamp(z0,0,9599)/1200)));
    const biome=world.regions[rz*8+rx]?.biome;
    const c=biome==='lush'?[0.16,0.32,0.16]:biome==='dry'?[0.36,0.30,0.18]:[0.22,0.36,0.23];
    pushQuad(arr,[x0,y00,z0],[x1,y10,z0],[x1,y11,z1],[x0,y01,z1],[0,1,0],c);
  }
}

function buildStaticScene() {
  if (!world || !player) return;
  const t0=performance.now();
  const arr=[]; const shadows=[];
  const radius = qualityLevel===0?680:qualityLevel===1?800:qualityLevel===2?980:1160;
  const adaptive = rendererMode===1;
  const staticDetail=adaptive?0.55+qualityLevel*0.12:0.82+qualityLevel*0.06;
  terrainChunk(arr,player.x,player.z,radius,staticDetail);
  const activeChunks = refreshStreamResidency(radius);
  const addCellObjects=(kind, cb)=>{for(const key of activeChunks){const list=streamResidency.chunks.get(key)?.[kind];if(!list)continue;for(const o of list)cb(o);}};
  const minX=player.x-radius,maxX=player.x+radius,minZ=player.z-radius,maxZ=player.z+radius;
  // Sun state is fixed for the duration of a static build, so resolve it once
  // instead of recomputing trig per building.
  const sun=sunState(), castShadows=sun.day>0.08, sx=sun.dir[0], sy=Math.max(0.2,sun.dir[1]), sz=sun.dir[2];
  addCellObjects('roads',r=>{
    if(r.x+r.w<minX||r.x>maxX||r.z+r.d<minZ||r.z>maxZ)return; pushRoad(arr,r);
  });
  addCellObjects('buildings',b=>{
    if(b.x+b.w/2<minX||b.x-b.w/2>maxX||b.z+b.d/2<minZ||b.z-b.d/2>maxZ)return;
    const d=Math.hypot(b.x-player.x,b.z-player.z); const detail=clamp(1-d/radius,0,1)*staticDetail;
    pushBuilding(arr,b,detail);
    if(detail>0.72 && (b.id%3===0)) pushStreetFurniture(arr,b.x+b.w*0.7,b.z+b.d*0.7,detail);
    // Project a simple soft sun shadow on the local terrain; geometry, not a screen-space fake.
    if(d<720 && castShadows) {
      const ext=b.h/Math.max(0.18,sy);
      const ox=-sx*ext*0.65, oz=-sz*ext*0.65;
      const x0=b.x-b.w*.46+ox, x1=b.x+b.w*.46+ox, z0=b.z-b.d*.46+oz, z1=b.z+b.d*.46+oz;
      const y0=terrainHeight(x0,z0,seed)+0.045, y1=terrainHeight(x1,z0,seed)+0.045, y2=terrainHeight(x1,z1,seed)+0.045, y3=terrainHeight(x0,z1,seed)+0.045;
      shadows.push(x0,y0,z0,x1,y1,z0,x1,y2,z1,x0,y0,z0,x1,y2,z1,x0,y3,z1);
    }
  });
  let treeStep = adaptive ? (qualityLevel===0?7:qualityLevel===1?5:qualityLevel===2?3:2) : (qualityLevel===0?5:qualityLevel===1?3:1);
  let counter=0;
  addCellObjects('trees',t=>{
    if(t.x<minX||t.x>maxX||t.z<minZ||t.z>maxZ)return;
    if((counter++ % treeStep)!==0)return;
    const d=Math.hypot(t.x-player.x,t.z-player.z); if(adaptive && d>700 && hash2(t.x*0.01,t.z*0.01,seed)>0.42)return;
    pushLowPolyTree(arr,t.x,terrainHeight(t.x,t.z,seed),t.z,t.s,t.type,t.seed);
  });
  // River is a separate animated water surface with real depth-tested geometry.
  for(const w of world.water){if(Math.abs(w.x-player.x)<radius+260 && Math.abs(w.z-player.z)<radius+260){const y=terrainHeight(w.x,w.z,seed)+0.15;const c=[0.06,0.22,0.32];pushBox(arr,w.x,y,w.z,w.w,0.04,w.d,c,c);}}
  gl.bindBuffer(gl.ARRAY_BUFFER,staticBuf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(arr),gl.STATIC_DRAW);
  staticVertexCount=arr.length/9;
  gl.bindBuffer(gl.ARRAY_BUFFER,shadowBuf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(shadows),gl.STATIC_DRAW);shadowVertexCount=shadows.length/3;
  streamOps++; streamGenerated=streamResidency.generated; streamFreed=streamResidency.evicted;
  streamKey=`${keyCell(player.x,player.z)}|${radius}|${qualityLevel}|${rendererMode}`;
  if($('loading')) $('loading').style.display='none';
  return performance.now()-t0;
}

function importanceOf(o) {
  const d=Math.hypot(o.x-player.x,o.z-player.z);
  const proximity=1-clamp(d/900,0,1);
  const motion=clamp(player.speed/300,0,1);
  const weather=world.weather===2?0.16:world.weather===1?0.05:0;
  const role=(o.job==='merchant'?0.11:o.job==='service'?0.08:o.job==='worker'?0.06:0.03);
  return clamp(proximity*0.62+motion*0.10+weather+role+0.08,0.04,1);
}

function ensureAgentState(id,n){if(!renderAgentState.has(id))renderAgentState.set(id,{x:n.x,z:n.z,next:0});return renderAgentState.get(id);}

function buildDynamicScene(force=false) {
  const moved=Math.hypot(player.x-lastDynamicX,player.z-lastDynamicZ);
  const adaptive=rendererMode===1;
  const minPeriod=adaptive?(qualityLevel===0?0.24:qualityLevel===1?0.16:qualityLevel===2?0.11:0.075):0.016;
  if(!force && adaptive && elapsed-dynamicBuildTime<minPeriod && moved<3){
    recomputed=0;reused=lastDynamicObjects;visibleObjects=lastDynamicObjects;simulatedNpcCount=Math.round(lastDynamicObjects*0.78);return 0;
  }
  const t0=performance.now(); const arr=[]; recomputed=0; reused=0; visibleObjects=0; simulatedNpcCount=0;
  const npcNear=qualityLevel===0?500:qualityLevel===1?650:qualityLevel===2?800:950;
  const carNear=qualityLevel===0?650:qualityLevel===1?800:qualityLevel===2?950:1150;
  for(const n of world.npcs){
    const d=Math.hypot(n.x-player.x,n.z-player.z); if(d>npcNear)continue;
    const imp=importanceOf(n), st=ensureAgentState(n.id,n);
    const interval=adaptive?(imp>0.68?0.045:imp>0.42?0.12:imp>0.20?0.24:0.42):0.016;
    if(elapsed>=st.next){st.x=n.x;st.z=n.z;st.next=elapsed+interval;recomputed++;}else reused++;
    pushNpc(arr,st.x,st.z,n.id,n.job,n.activity,imp);
    visibleObjects++;simulatedNpcCount++;
  }
  for(const c of world.cars){const d=Math.hypot(c.x-player.x,c.z-player.z);if(d>carNear)continue;pushCar(arr,c);recomputed++;visibleObjects++;}
  const y=terrainHeight(player.x,player.z,seed);
  if(firstPerson) pushBox(arr,player.x,y,player.z,0.45,0.9,0.32,[0.18,0.32,0.68]);
  else pushNpc(arr,player.x,player.z,9999,'service','idle',1);
  if(currentMission){
    const mb=missionBuilding(currentMission.stage==='pickup'?currentMission.sourceId:currentMission.targetId);
    if(mb){const my=terrainHeight(mb.x,mb.z,seed);pushCylinder(arr,mb.x,my,mb.z,0.7,5.5,[0.95,0.68,0.25],8);pushCylinder(arr,mb.x,my+5.5,mb.z,1.2,0.08,[1.0,0.78,0.30],8);}
  }
  gl.bindBuffer(gl.ARRAY_BUFFER,dynamicBuf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(arr),gl.DYNAMIC_DRAW);dynamicVertexCount=arr.length/9;
  dynamicBuildTime=elapsed;lastDynamicX=player.x;lastDynamicZ=player.z;dynamicBuilds++;lastDynamicObjects=visibleObjects;
  return performance.now()-t0;
}
function shadowDraw(pv) {
  if(!shadowVertexCount) return;
  gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA); gl.depthMask(false);
  gl.useProgram(shadowProg); gl.uniformMatrix4fv(loc.sPV,false,pv); gl.bindBuffer(gl.ARRAY_BUFFER,shadowBuf); gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,3,gl.FLOAT,false,12,0); gl.drawArrays(gl.TRIANGLES,0,shadowVertexCount);
  gl.depthMask(true); gl.disable(gl.BLEND);
}

function cameraMatrix(){
  const eyeY=player.y+(firstPerson?1.55:4.8);
  const back=firstPerson?0:8.5;
  const eye=[player.x-Math.sin(yaw)*back,eyeY,player.z-Math.cos(yaw)*back];
  const dir=[Math.sin(yaw)*Math.cos(pitch),Math.sin(pitch),Math.cos(yaw)*Math.cos(pitch)];
  const target=[eye[0]+dir[0]*18,eye[1]+dir[1]*18,eye[2]+dir[2]*18];
  camera.x=eye[0];camera.y=eye[1];camera.z=eye[2];camera.yaw=yaw;camera.pitch=pitch;
  return mul(matrixPerspective(1.0,canvas.width/canvas.height,0.08,2300),lookAt(eye,target));
}

function drawScene(){
  const t0=performance.now();
  const s=sunState();
  const night=s.night;
  const fog=[0.10+0.12*s.day,0.16+0.14*s.day,0.22+0.16*s.day];
  gl.viewport(0,0,canvas.width,canvas.height);
  gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.clearColor(0.055+0.16*s.day,0.075+0.22*s.day,0.11+0.27*s.day,1);
  gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
  const pv=cameraMatrix();lastPV=pv;
  gl.useProgram(sceneProg);
  gl.bindVertexArray(vao);
  gl.uniformMatrix4fv(loc.uPV,false,pv); gl.uniform3f(loc.uCam,camera.x,camera.y,camera.z); gl.uniform1f(loc.uTime,elapsed);
  gl.uniform3f(loc.uSun,s.dir[0],s.dir[1],s.dir[2]); gl.uniform3f(loc.uFog,...fog); gl.uniform3f(loc.uAmbient,0.24+0.22*s.day,0.28+0.28*s.day,0.34+0.30*s.day); gl.uniform1f(loc.uFogDensity,world.weather===1?0.0000032:world.weather===2?0.0000025:0.0000018); gl.uniform1f(loc.uNight,night); gl.uniform1f(loc.uWater,0);
  gl.bindBuffer(gl.ARRAY_BUFFER,staticBuf); gl.vertexAttribPointer(0,3,gl.FLOAT,false,36,0); gl.vertexAttribPointer(1,3,gl.FLOAT,false,36,12); gl.vertexAttribPointer(2,3,gl.FLOAT,false,36,24); gl.drawArrays(gl.TRIANGLES,0,staticVertexCount);
  shadowDraw(pv);
  gl.bindBuffer(gl.ARRAY_BUFFER,dynamicBuf); gl.vertexAttribPointer(0,3,gl.FLOAT,false,36,0); gl.vertexAttribPointer(1,3,gl.FLOAT,false,36,12); gl.vertexAttribPointer(2,3,gl.FLOAT,false,36,24); gl.drawArrays(gl.TRIANGLES,0,dynamicVertexCount);
  gl.bindVertexArray(null);
  renderMs=performance.now()-t0;
}

/**
 * Project the active mission building into screen space and draw a gold
 * chevron + distance ring above it on the 2D overlay. Skipped when the target
 * is behind the camera or when the simulation hasn't rendered yet.
 */
function drawMissionMarker(){
  if(!currentMission||!lastPV)return;
  const mb=missionBuilding(currentMission.stage==='pickup'?currentMission.sourceId:currentMission.targetId);
  if(!mb)return;
  const my=terrainHeight(mb.x,mb.z,seed)+mb.h+6;
  const clip=transformPoint(lastPV,[mb.x,my,mb.z]);
  if(clip[3]<=0.001)return; // behind the camera
  const sx=(clip[0]/clip[3]*0.5+0.5)*overlay.width;
  const sy=(1-(clip[1]/clip[3]*0.5+0.5))*overlay.height;
  const dist=Math.hypot(player.x-mb.x,player.z-mb.z);
  overlayCtx.save();
  overlayCtx.strokeStyle='rgba(255,211,107,0.9)';
  overlayCtx.fillStyle='rgba(255,211,107,0.95)';
  overlayCtx.lineWidth=2;
  overlayCtx.beginPath();
  overlayCtx.moveTo(sx,sy-26);overlayCtx.lineTo(sx-9,sy-12);overlayCtx.lineTo(sx+9,sy-12);
  overlayCtx.closePath();overlayCtx.fill();
  overlayCtx.beginPath();overlayCtx.arc(sx,sy-2,11,0,Math.PI*2);overlayCtx.stroke();
  overlayCtx.font='11px system-ui,sans-serif';overlayCtx.textAlign='center';
  overlayCtx.fillText(`${Math.round(dist)}m`,sx,sy+18);
  overlayCtx.restore();
}

function blockedPlayer(nx,nz){
  const cell=480,cx=Math.floor(nx/cell),cz=Math.floor(nz/cell);
  for(let yy=cz-1;yy<=cz+1;yy++)for(let xx=cx-1;xx<=cx+1;xx++){
    const list=worldIndex.buildings.get(`${xx},${yy}`); if(!list) continue;
    for(const b of list){if(Math.abs(nx-b.x)<b.w*0.48+1.2 && Math.abs(nz-b.z)<b.d*0.48+1.2)return true;}
  }
  return false;
}

function chooseNpcGoal(n) {
  const hour=world.time;
  const rain=world.weather===2;
  const workHour=hour>=8&&hour<17;
  let target;
  if(n.energy<18){ target=n.home; n.activity='rest'; n.state='home'; }
  else if(rain && rand()<0.52){
    const b=world.businesses[Math.floor(rand()*world.businesses.length)];
    const building=b?world.buildings[b.buildingId]:null;
    target=building?{x:building.x,z:building.z}:n.home; n.activity='shelter'; n.state='indoors';
  } else if(workHour){ target=n.work; n.activity='work'; n.state='work'; }
  else if(hour>=18&&hour<22 && rand()<0.45){
    const b=world.businesses[Math.floor(rand()*world.businesses.length)];
    const building=b?world.buildings[b.buildingId]:null;
    target=building?{x:building.x,z:building.z}:n.home; n.activity='social'; n.state='social';
  } else { target=n.home; n.activity='rest'; n.state='home'; }
  n.goal={x:target.x+(rand()-.5)*55,z:target.z+(rand()-.5)*55};
  n.t=0;
}

function updateNpc(n,dt){
  n.t+=dt;n.energy=clamp(n.energy-dt*(n.activity==='work'?0.018:0.008),0,100);
  if(!n.goal||n.t>4.5||Math.hypot(n.goal.x-n.x,n.goal.z-n.z)<8) chooseNpcGoal(n);
  let vx=n.goal.x-n.x,vz=n.goal.z-n.z,d=Math.hypot(vx,vz)||1;
  let speed=n.speed*(world.weather===2?0.88:1);
  if(n.activity==='rest'||n.activity==='shelter')speed*=0.45;
  n.x+=vx/d*speed*dt;n.z+=vz/d*speed*dt;
  n.x=clamp(n.x,45,world.size-45);n.z=clamp(n.z,45,world.size-45);
  if(d<9)n.memory.push({time:world.time,activity:n.activity});
  if(n.memory.length>8)n.memory.shift();
}

function updateTraffic(dt){
  const nearby=world.cars.filter(c=>Math.hypot(c.x-player.x,c.z-player.z)<1050);
  for(const c of world.cars){
    const local=nearby.length>0 && Math.hypot(c.x-player.x,c.z-player.z)<1050;
    let target=38+(c.id%7)*3;
    if(world.weather===2)target*=0.72;
    // Local lane awareness: slow when a same-direction car is close ahead.
    if(local){
      for(const o of nearby){
        if(o===c||o.horizontal!==c.horizontal||o.dir!==c.dir)continue;
        if(c.horizontal){const gap=(o.x-c.x)*c.dir;if(gap>0&&gap<26&&Math.abs(o.z-c.z)<7)target*=0.45;}
        else {const gap=(o.z-c.z)*c.dir;if(gap>0&&gap<26&&Math.abs(o.x-c.x)<7)target*=0.45;}
      }
    }
    c.targetSpeed=target;c.speed+=((target-c.speed)*Math.min(1,dt*2.4));
    if(c.horizontal){c.x+=c.speed*c.dir*dt;if(c.x<-140)c.x=world.size+140;if(c.x>world.size+140)c.x=-140;}
    else{c.z+=c.speed*c.dir*dt;if(c.z<-140)c.z=world.size+140;if(c.z>world.size+140)c.z=-140;}
  }
  // Accidents emerge occasionally from very small headway under bad weather.
  if(world.weather===2 && eventTimer<=0){
    for(let i=0;i<nearby.length;i++){const a=nearby[i];for(let j=i+1;j<Math.min(i+5,nearby.length);j++){const b=nearby[j];const d=Math.hypot(a.x-b.x,a.z-b.z);if(d<4.2&&a.horizontal===b.horizontal){
      a.speed*=0.25;b.speed*=0.25;world.events.push({id:world.eventSerial++,type:'accident',x:(a.x+b.x)/2,z:(a.z+b.z)/2,t:28,life:28,severity:0.7,source:'traffic'});eventTimer=28;showToast('Traffic incident reported nearby.');break;
    }}}
  }
}

function updateBusinesses(dt){
  for(const b of world.businesses){
    const baseDemand=b.type==='market'?0.020:b.type==='cafe'?0.016:b.type==='service'?0.012:0.010;
    const weatherMod=world.weather===2?0.62:world.weather===1?0.82:1;
    const demand=baseDemand*weatherMod*(0.72+world.economy*0.35);
    if(b.open && rand()<dt*demand){b.stock-=0.15;b.customers++;b.revenue+=b.price*0.4;}
    if(b.stock<5)b.open=false;
    if(b.stock<18 && rand()<dt*0.035)b.stock+=1.0;
    if(b.stock>22)b.open=true;
    b.price=clamp(0.75+world.economy*0.42+(25-Math.min(25,b.stock))*0.014,0.65,2.25);
    b.reputation=clamp(b.reputation+(b.open?0.0002:-0.0005),0.15,1.5);
  }
  const localPop=world.npcs.reduce((acc,n)=>acc+(Math.hypot(n.x-player.x,n.z-player.z)<750?1:0),0);
  const weatherPenalty = world.weather===2 ? 0.0005 : 0;
  world.economy=clamp(world.economy + dt*((localPop/2200) - weatherPenalty - world.businesses.length*0.0000008),0.7,1.7);
}

function eventMaintenance(dt){
  for(const e of world.events){e.t-=dt;if(e.type==='market'&&e.t<10){e.t=e.life=140;}if(e.type==='accident'&&e.t<0)e.active=false;}
  world.events=world.events.filter(e=>e.active!==false && e.t>0).slice(-80);
}

function discoverDistricts(){
  for(const d of world.districts){
    if(Math.hypot(player.x-d.x,player.z-d.z)<d.r*0.46 && !world.discovered[d.id]){
      world.discovered[d.id]={time:world.time,type:d.type};player.discoveries++;world.events.push({id:world.eventSerial++,type:'discovery',x:d.x,z:d.z,t:16,life:16,severity:0.15,source:'player'});showToast(`Discovered ${d.type} district #${d.id}.`);beep(660,0.16);
    }
  }
}

function chooseMission(){
  const open=world.businesses.filter(b=>b.open&&b.stock>25);
  const targets=world.businesses.filter(b=>!b.open||b.stock<11);
  if(!open.length||!targets.length){
    const a=world.businesses[Math.floor(rand()*world.businesses.length)], bb=world.businesses[Math.floor(rand()*world.businesses.length)];
    if(!a||!bb||a.id===bb.id)return;
    currentMission={type:'delivery',stage:'pickup',sourceId:a.id,targetId:bb.id,crates:3,reward:30+player.rank*8,expires:180};
    return;
  }
  let source=open[Math.floor(rand()*open.length)],target=targets[Math.floor(rand()*targets.length)];
  for(let tries=0;tries<10 && source.id===target.id;tries++)target=targets[Math.floor(rand()*targets.length)];
  currentMission={type:'delivery',stage:'pickup',sourceId:source.id,targetId:target.id,crates:3,reward:38+player.rank*9,expires:210};
}

/**
 * Resolve a business id to its building in O(1).
 * This runs every frame (mission beacon geometry + screen marker), so a linear
 * scan over every business in the world would be a per-frame cost for nothing.
 */
function missionBuilding(id){
  const b=worldIndex.businessById.get(id);
  if(!b) return null;
  return worldIndex.buildingById.get(b.buildingId)||null;
}
function missionDistance(id){const b=missionBuilding(id);return b?Math.hypot(player.x-b.x,player.z-b.z):Infinity;}
function updateMission(dt){
  if(!currentMission){missionTimer-=dt;if(missionTimer<=0){chooseMission();missionTimer=45;}return;}
  currentMission.expires-=dt;
  if(currentMission.expires<=0){currentMission=null;showToast('Delivery expired. A new job will appear.');missionTimer=15;return;}
}

function interact(){
  initializeAudio();
  let nearest=null, best=70;
  for(const b of world.businesses){const bl=world.buildings[b.buildingId];if(!bl)continue;const d=Math.hypot(player.x-bl.x,player.z-bl.z);if(d<best){best=d;nearest={business:b,building:bl,d};}}
  if(currentMission){
    if(currentMission.stage==='pickup' && nearest?.business.id===currentMission.sourceId){
      nearest.business.stock=Math.max(0,nearest.business.stock-currentMission.crates);
      currentMission.stage='delivery';showToast(`Picked up ${currentMission.crates} crates. Deliver them to the marked business.`);beep(520,0.12);return;
    }
    if(currentMission.stage==='delivery' && nearest?.business.id===currentMission.targetId){
      nearest.business.stock+=currentMission.crates;nearest.business.open=true;nearest.business.customers+=1;nearest.business.reputation=clamp(nearest.business.reputation+0.08,0,1.5);
      player.money+=currentMission.reward;player.energy=clamp(player.energy+9,0,100);player.missionsCompleted++;player.rank=1+Math.floor(player.missionsCompleted/3);
      world.events.push({id:world.eventSerial++,type:'trade',x:nearest.building.x,z:nearest.building.z,t:18,life:18,severity:0.25,source:'mission'});
      showToast(`Delivery complete +$${currentMission.reward}.`);beep(880,0.18);currentMission=null;missionTimer=18;saveGame();return;
    }
  }
  if(nearest){
    const cost=nearest.business.price*2.2;
    if(nearest.business.open && player.money>=cost){
      player.money-=cost;nearest.business.stock=Math.max(0,nearest.business.stock-1);nearest.business.customers++;player.energy=clamp(player.energy+8,0,100);
      world.events.push({id:world.eventSerial++,type:'trade',x:nearest.building.x,z:nearest.building.z,t:10,life:10,severity:0.12,source:'player'});
      showToast(`Visited ${nearest.business.type}. Spent $${cost.toFixed(0)}.`);beep(440,0.08);
      saveGame();return;
    }
  }
  let nearestNpc=null, npcD=45;
  for(const n of world.npcs){const d=Math.hypot(player.x-n.x,player.z-n.z);if(d<npcD){npcD=d;nearestNpc=n;}}
  if(nearestNpc){nearestNpc.memory.push({time:world.time,activity:'talked'});nearestNpc.mood=clamp(nearestNpc.mood+0.03,0,1);player.energy=clamp(player.energy+2,0,100);showToast(`${nearestNpc.name}: "${nearestNpc.activity==='work'?'Busy day out there.':'The city keeps changing.'}"`);beep(360,0.06);return;}
  showToast('Nothing useful to interact with here.');
}

function updatePlayer(dt){
  let ix=(keys.d||keys.arrowright?1:0)-(keys.a||keys.arrowleft?1:0)+touchMove.x;
  let iz=(keys.s||keys.arrowdown?1:0)-(keys.w||keys.arrowup?1:0)+touchMove.y;
  const len=Math.hypot(ix,iz)||1;if(ix||iz){ix/=len;iz/=len;}
  const sprint=keys.shift||touchSprint;
  let speed=sprint?300:190;
  if(player.energy<18)speed*=0.62;
  if(world.weather===2)speed*=0.92;
  const forward=[Math.sin(yaw),Math.cos(yaw)], right=[Math.cos(yaw),-Math.sin(yaw)];
  const dx=(forward[0]*iz+right[0]*ix)*speed*dt, dz=(forward[1]*iz+right[1]*ix)*speed*dt;
  const nx=clamp(player.x+dx,40,world.size-40),nz=clamp(player.z+dz,40,world.size-40);
  if(!blockedPlayer(nx,nz)){player.x=nx;player.z=nz;} else if(!blockedPlayer(player.x+dx,player.z)){player.x=nx;} else if(!blockedPlayer(player.x,player.z+dz)){player.z=nz;}
  player.speed=(ix||iz)?speed:0;
  player.energy=clamp(player.energy+(ix||iz?-(sprint?5.2:2.1):4.5)*dt,0,100);
  player.y=terrainHeight(player.x,player.z,seed)+0.55;
  discoverDistricts();
}

function updateSimulation(dt){
  world.time=(world.time+dt*0.04)%24;
  weatherTimer-=dt;if(weatherTimer<=0){world.weather=(world.weather+1+Math.floor(rand()*2))%3;weatherTimer=55+rand()*95;showToast(`Weather changed: ${['clear skies','mist','rain'][world.weather]}.`);}
  eventTimer=Math.max(0,eventTimer-dt);
  updatePlayer(dt);
  // NPC simulation LOD: near agents run continuously, medium/far agents tick less often,
  // and very-far agents only contribute aggregate state. This keeps the world evolving
  // without paying full behavior cost for every resident every frame.
  for(const n of world.npcs){
    const d=Math.hypot(n.x-player.x,n.z-player.z);
    n.simAccum=(n.simAccum||0)+dt;
    if(d<650){ n.simLod=0; updateNpc(n,dt); n.simAccum=0; }
    else if(d<1800){
      n.simLod=1;
      if(n.simAccum>=0.16){ const step=n.simAccum; n.simAccum=0; updateNpc(n,step); }
    } else if(d<3600){
      n.simLod=2;
      if(n.simAccum>=0.65){ const step=n.simAccum; n.simAccum=0; updateNpc(n,step); }
    } else {
      n.simLod=3;
      n.energy=clamp(n.energy-dt*(n.activity==='work'?0.006:0.002),0,100);
      if(n.simAccum>=8){ n.simAccum=0; n.state=n.energy<18?'home':'world'; }
    }
  }
  updateTraffic(dt);
  economyTimer-=dt;if(economyTimer<=0){updateBusinesses(1.0);economyTimer=1;}
  updateMission(dt);
  eventMaintenance(dt);
  saveTimer-=dt;if(saveTimer<=0){saveGame();saveTimer=8;}
  if(audioNodes){audioNodes.noiseGain.gain.value=world.weather===2?0.018:world.weather===1?0.010:0.004;}
}

function saveGame(){
  if(!world||!player)return;
  const payload={version:3,seed,time:world.time,weather:world.weather,economy:world.economy,heat:world.heat,discovered:world.discovered,events:world.events.slice(-30),player:{x:player.x,z:player.z,money:player.money,energy:player.energy,discoveries:player.discoveries,missionsCompleted:player.missionsCompleted,rank:player.rank},mission:currentMission,businesses:world.businesses.map(b=>({id:b.id,stock:b.stock,price:b.price,open:b.open,customers:b.customers,revenue:b.revenue,reputation:b.reputation})),npcs:world.npcs.map(n=>({id:n.id,x:n.x,z:n.z,energy:n.energy,money:n.money,mood:n.mood,state:n.state,activity:n.activity,memory:n.memory.slice(-6)}))};
  try{localStorage.setItem(SAVE_KEY,JSON.stringify(payload));$('saveState').textContent='Saved';}catch(e){$('saveState').textContent='Save unavailable';}
}

function loadGame(){
  try{
    const s=JSON.parse(localStorage.getItem(SAVE_KEY)||'null');if(!s||s.seed!==seed)return false;
    world.time=s.time??world.time;world.weather=s.weather??world.weather;world.economy=s.economy??world.economy;world.heat=s.heat??0;world.discovered=s.discovered||{};world.events=s.events||world.events;
    if(s.player)Object.assign(player,s.player);
    if(s.businesses) for(const saved of s.businesses){const b=world.businesses.find(x=>x.id===saved.id);if(b)Object.assign(b,saved);}
    if(s.npcs) for(const saved of s.npcs){const n=world.npcs.find(x=>x.id===saved.id);if(n)Object.assign(n,saved);}
    currentMission=s.mission||null;showToast('Saved world restored.');return true;
  }catch(e){return false;}
}

function newWorld(nextSeed){
  seed=(nextSeed===undefined?(Date.now()>>>0):nextSeed)>>>0;rand=rng(seed);world=generateWorld(seed);
  player={x:4800,z:4800,y:terrainHeight(4800,4800,seed)+0.55,money:120,energy:100,discoveries:0,speed:0,missionsCompleted:0,rank:1};
  currentMission=null;missionTimer=2;streamKey='';renderAgentState.clear();streamResidency.active.clear();streamResidency.chunks.clear();streamResidency.generated=0;streamResidency.evicted=0;worldIndex.buildings.clear();
  buildWorldIndex(); buildStaticScene(); buildDynamicScene(true);
  saveGame();
  showToast(`New world generated from seed ${seed}.`);
}

function startGame(load=true){
  const stored=load?(()=>{try{return JSON.parse(localStorage.getItem(SAVE_KEY)||'null')}catch(e){return null}})():null;
  const chosen=load ? (stored?.seed??WORLD_SEED_DEFAULT) : (Number.isFinite(BENCH_SEED)&&BENCH_SEED>0 ? BENCH_SEED : WORLD_SEED_DEFAULT); seed=chosen>>>0;rand=rng(seed);world=generateWorld(seed);
  player={x:4800,z:4800,y:terrainHeight(4800,4800,seed)+0.55,money:120,energy:100,discoveries:0,speed:0,missionsCompleted:0,rank:1};
  buildWorldIndex();if(load) loadGame();buildWorldIndex();streamResidency.active.clear();streamResidency.chunks.clear();buildStaticScene();buildDynamicScene(true);
  gameRunning=true;inputLocked=false;last=performance.now();requestAnimationFrame(frame);showToast('Explore, find businesses and complete the delivery route.');
}

function updateHUD(){
  const district=districtAt(world,player.x,player.z);
  $('cash').textContent=`$${player.money.toFixed(0)}`;
  $('energy').textContent=`Energy ${Math.round(player.energy)}`;
  $('location').textContent=district?`${district.type.toUpperCase()} · district ${district.id}`:'WILDERNESS';
  $('discoveries').textContent=`Discoveries ${player.discoveries}/${world.districts.length}`;
  const mode=`${MODES[rendererMode]} · ${QUALITY[qualityLevel]}`;
  $('modePill').textContent=mode;
  if(currentMission){const id=currentMission.stage==='pickup'?currentMission.sourceId:currentMission.targetId;const d=missionDistance(id);const action=currentMission.stage==='pickup'?'Pick up':'Deliver';$('objective').textContent=`${action} ${currentMission.crates} crates · ${Math.round(d)}m · reward $${currentMission.reward}`;}
  else $('objective').textContent='Explore the world — your next delivery will emerge from local shortages.';
  $('clock').textContent=`${world.time.toFixed(1).padStart(4,'0')} · ${['CLEAR','MIST','RAIN'][world.weather]}`;
  if(debugVisible){$('debug').innerHTML=`<b>DEVELOPER TELEMETRY</b><br>FPS ${fps.toFixed(1)} · render ${renderMs.toFixed(2)}ms<br>static ${Math.round(staticVertexCount).toLocaleString()}v · dynamic ${Math.round(dynamicVertexCount).toLocaleString()}v · shadow ${Math.round(shadowVertexCount).toLocaleString()}v<br>visible ${visibleObjects} · NPC rendered ${simulatedNpcCount}/${world.npcs.length}<br>recompute ${recomputed} · reuse ${reused}<br>stream ops ${streamOps} · resident ${streamResidency.active.size} chunks · created ${streamGenerated} · evicted ${streamFreed} · last ${streamResidency.lastBuildMs.toFixed(2)}ms<br>world ${world.buildings.length} buildings · ${world.trees.length} trees · ${world.businesses.length} businesses`;}
}

function frame(now){
  if(!gameRunning)return;
  const dt=Math.min(0.05,(now-last)/1000);last=now;elapsed+=dt;
  updateSimulation(dt);
  const cellNow=keyCell(player.x,player.z);
  const radius = qualityLevel===0?680:qualityLevel===1?800:qualityLevel===2?980:1160;
  const streamStateNow=`${cellNow}|${radius}|${qualityLevel}|${rendererMode}`;
  if(streamStateNow!==streamKey){buildStaticScene();dynamicBuildTime=-Infinity;}
  const dynMs=buildDynamicScene(false);drawScene();
  accFrames++;accSeconds+=dt;
  if(BENCH_MODE && !benchStart) benchStart=now;
  if(BENCH_MODE && benchStart){
    benchFrames++;
    benchRenderMs+=renderMs;
    benchRecomputed+=recomputed; benchReused+=reused; benchVisible+=visibleObjects;
    if(dynMs>0){benchRebuilds++;benchDynamicMs+=dynMs;}
    if(now-benchStart>=8000&&!benchDone){
      benchDone=true;
      document.body.dataset.benchmark=JSON.stringify({
        seed, mode:BENCH_MODE.toUpperCase(), quality:QUALITY[qualityLevel],
        fps:+(benchFrames/((now-benchStart)/1000)).toFixed(2),
        avgRenderMs:+(benchRenderMs/benchFrames).toFixed(3),
        avgDynamicBuildMs:+(benchDynamicMs/(benchRebuilds||1)).toFixed(3),
        dynamicBufferBuilds:dynamicBuilds, avgRecomputed:+(benchRecomputed/benchFrames).toFixed(2), avgReused:+(benchReused/benchFrames).toFixed(2), avgVisibleObjects:+(benchVisible/benchFrames).toFixed(2),
        staticVertices:Math.round(staticVertexCount), dynamicVertices:Math.round(dynamicVertexCount),
        streamOps, streamGenerated,
        world:{regions:world.regions.length,districts:world.districts.length,roads:world.roads.length,buildings:world.buildings.length,trees:world.trees.length,npcs:world.npcs.length,cars:world.cars.length,businesses:world.businesses.length}
      });
      showToast('Benchmark complete. Result is available in document.body.dataset.benchmark.');
    }
  }
  if(accSeconds>=1){fps=accFrames/accSeconds;accFrames=0;accSeconds=0;updateHUD();}
  if(overlay.width!==canvas.width||overlay.height!==canvas.height){overlay.width=canvas.width;overlay.height=canvas.height;}
  overlayCtx.clearRect(0,0,overlay.width,overlay.height); drawMissionMarker();
  clearToast(dt);
  requestAnimationFrame(frame);
}

function resize(){canvas.width=Math.floor(innerWidth*Math.min(devicePixelRatio||1,1.5));canvas.height=Math.floor(innerHeight*Math.min(devicePixelRatio||1,1.5));canvas.style.width='100%';canvas.style.height='100%';}

function cycleRenderer(){rendererMode=rendererMode?0:1;streamKey='';showToast(`Renderer: ${MODES[rendererMode]}.`);}
function cycleQuality(){qualityLevel=(qualityLevel+1)%QUALITY.length;streamKey='';showToast(`Quality: ${QUALITY[qualityLevel]}.`);}

function bindTouch(){
  const stick=$('stick'), knob=$('knob');let active=false,startX=0,startY=0;
  const end=()=>{active=false;touchMove.x=0;touchMove.y=0;knob.style.transform='translate(-50%,-50%)';};
  stick.addEventListener('pointerdown',e=>{active=true;startX=e.clientX;startY=e.clientY;stick.setPointerCapture(e.pointerId);initializeAudio();});
  stick.addEventListener('pointermove',e=>{if(!active)return;let dx=(e.clientX-startX)/44,dy=(e.clientY-startY)/44,len=Math.hypot(dx,dy)||1;if(len>1){dx/=len;dy/=len;}touchMove.x=dx;touchMove.y=dy;knob.style.transform=`translate(calc(-50% + ${dx*32}px),calc(-50% + ${dy*32}px))`;});
  stick.addEventListener('pointerup',end);stick.addEventListener('pointercancel',end);
  $('touchSprint').addEventListener('pointerdown',()=>{touchSprint=true;initializeAudio();});$('touchSprint').addEventListener('pointerup',()=>touchSprint=false);$('touchSprint').addEventListener('pointercancel',()=>touchSprint=false);
  $('touchInteract').addEventListener('pointerdown',()=>interact());
}

addEventListener('keydown',e=>{
  const k=e.key.toLowerCase();keys[k]=true;initializeAudio();
  if(k==='e')interact();
  if(k==='t')cycleRenderer();
  if(k==='q')cycleQuality();
  if(k==='v'){firstPerson=!firstPerson;showToast(firstPerson?'First-person camera':'Third-person camera');}
  if(k==='f2')saveGame();
  if(k==='f3'){if(loadGame()){buildWorldIndex();streamKey='';showToast('Game loaded.');}}
  if(k==='d'){debugVisible=!debugVisible;$('debug').classList.toggle('show',debugVisible);}
  if(k==='n'){newWorld();}
  if(k==='escape' && document.pointerLockElement===canvas)document.exitPointerLock?.();
});
addEventListener('keyup',e=>keys[e.key.toLowerCase()]=false);
canvas.addEventListener('click',()=>{initializeAudio();canvas.requestPointerLock?.();});
addEventListener('pointerlockchange',()=>{pointerLocked=document.pointerLockElement===canvas;inputLocked=!pointerLocked;});
addEventListener('mousemove',e=>{if(pointerLocked){yaw-=e.movementX*0.0023;pitch=clamp(pitch-e.movementY*0.0020,-1.05,0.35);}});
$('saveBtn').addEventListener('click',()=>{saveGame();beep(520,0.08);});
$('loadBtn').addEventListener('click',()=>{if(loadGame()){buildWorldIndex();streamKey='';showToast('Game loaded.');beep(620,0.08);}else showToast('No saved world found for this seed.');});
$('newBtn').addEventListener('click',()=>newWorld());
$('qualityBtn').addEventListener('click',cycleQuality);
$('rendererBtn').addEventListener('click',cycleRenderer);
$('cameraBtn').addEventListener('click',()=>{firstPerson=!firstPerson;showToast(firstPerson?'First-person camera':'Third-person camera');});
$('startBtn').addEventListener('click',()=>{initializeAudio();$('start').classList.add('hide');startGame(true);});
bindTouch();addEventListener('resize',resize);resize();

/**
 * Development and test surface.
 *
 * The runtime test harness (tools/headless_runtime.mjs) drives the real frame
 * loop, so it needs a way to assert simulation state and to place the player at
 * a known position instead of walking there over simulated minutes. This object
 * is the only such entry point; everything else in the game runs through the
 * normal input and frame path. It is intentionally plain and side-effect free
 * apart from the explicit `teleport` used by tests and manual QA.
 */
globalThis.EMERGENT = {
  get player() { return player; },
  get mission() { return currentMission; },
  get world() { return world; },
  get running() { return gameRunning; },
  get renderer() { return { mode: MODES[rendererMode], quality: QUALITY[qualityLevel] }; },
  get stats() {
    return {
      fps, renderMs, staticVertexCount, dynamicVertexCount, shadowVertexCount,
      recomputed, reused, visibleObjects, simulatedNpcCount, streamOps,
      streamGenerated, streamFreed, residentChunks: streamResidency.active.size,
      lastViewProjection: lastPV
    };
  },
  /** Quality level index, so tests can assert streaming radii directly. */
  get streamRadius() {
    return qualityLevel === 0 ? 680 : qualityLevel === 1 ? 800 : qualityLevel === 2 ? 980 : 1160;
  },
  /** Move the player instantly to a world position (tests / manual QA). */
  teleport(x, z) {
    player.x = x; player.z = z;
    player.y = terrainHeight(x, z, seed) + 0.55;
    streamKey = '';
    return { x: player.x, y: player.y, z: player.z };
  },
  /** Building that a mission id refers to, for delivery-flow assertions. */
  missionBuildingFor(mission) {
    const m = mission || currentMission;
    if (!m) return null;
    return missionBuilding(m.stage === 'pickup' ? m.sourceId : m.targetId);
  },
  save: saveGame,
  load: loadGame,
  newWorld
};

// Initial WebGL state and world preview; full game starts from the title overlay.
gl.enable(gl.CULL_FACE);gl.cullFace(gl.BACK);gl.enable(gl.DEPTH_TEST);gl.clearDepth(1);
if(BENCH_MODE){rendererMode=BENCH_MODE.toLowerCase()==='adaptive'?1:0;qualityLevel=Math.max(0,Math.min(3,QUALITY.indexOf(BENCH_QUALITY?.toUpperCase()||'HIGH')));if(Number.isFinite(BENCH_SEED)&&BENCH_SEED>0){seed=BENCH_SEED>>>0;}startGame(false);$('start').classList.add('hide');}else{startGame(true);$('start').classList.add('hide');}
