import { clamp, rng, hash2, terrainHeight, generateWorld, districtAt } from './world.mjs';
import { matrixPerspective, lookAt, mul, transformPoint } from './math3d.mjs';
import { extractFrustumPlanes, aabbVisible } from './culling.mjs';
import { MATERIAL_DESCRIPTOR } from './assets/textures/materials.mjs';
import { buildMaterialTable, MAX_MATERIALS } from './materials.mjs';
import { createMaterialTextures, bindMaterialTextures as bindTextures, mipLevels, UNITS } from './textures.mjs';
import { createGeometryKit, VERTEX_FLOATS, VERTEX_BYTES } from './geometry.mjs';
import { buildBuilding, buildRoad, buildTree, buildBush, dressStreet, dressBuilding, buildCrossing, buildCar, buildCarProxy, buildCharacter, buildCharacterProxy } from './city.mjs';
import {
  createPhysicsWorld, ensurePlayer, stepPlayer, playerFeet, setStaticBox,
  removeStaticBox, setAgentBox, removeAgent, buildHeightfield, teleportPlayer,
  surfaceHeightAt, DEFAULT_TUNING
} from './physics.mjs';
import {
  currentStage, advanceMission, objectiveText, stageDistance, createJob
} from './missions.mjs';
import {
  createInput, isDown, wasPressed, moveAxes, endFrame, addMouseDelta, setKey,
  isCapturing, setTouchStick, setButton, setStick, deserialiseBindings,
  serialiseBindings
} from './input.mjs';

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
/**
 * Rapier world, created once at boot and kept for the life of the session.
 *
 * `null` until the async constructor resolves, which is why every consumer has
 * to cope with a null check rather than assuming it exists. Movement does not
 * fall back to a hand-rolled collision path: a world that cannot be simulated
 * must not silently become a world where the player walks through buildings.
 */
let physics = null;
// The simulation world is built during module evaluation, before a single frame
// can run. A character controller that is not ready on the first frame is a
// character that clips through geometry on the first frame, and making this
// lazy would mean every consumer needed a null check on the hot path.
//
// It lives here rather than in the boot tail because `seed` has to be
// initialised before the terrain sampler closes over it.
try {
  physics = await createPhysicsWorld({ heightAt: (x, z) => terrainHeight(x, z, seed) });
} catch (err) {
  // Unrecoverable, and deliberately loud: a city whose collision cannot be
  // simulated is not a degraded experience, it is a broken one, and a silent
  // fallback to a hand-rolled point test is exactly the failure this replaced.
  const message = `EMERGENT could not start: physics initialisation failed (${err && err.message ? err.message : err}).`;
  document.body.appendChild(Object.assign(document.createElement('pre'), {
    textContent: message,
    style: 'position:fixed;inset:0;z-index:99;margin:0;padding:32px;background:#071018;color:#ff9c8a;font:14px/1.6 ui-monospace,monospace;white-space:pre-wrap'
  }));
  throw err;
}
// Fixed simulation step. Rapier's world timestep and the movement accumulator
// both use this, so the two can never drift apart.
const PHYSICS_DT = 1 / 60;
// Catch-up ceiling. Six steps is 100ms of simulation, which covers every frame
// rate down to 10fps; past that the game is already unplayable and spiralling
// the solver is what turns a hitch into a freeze.
const MAX_PHYSICS_STEPS = 6;
let physicsAccumulator = 0;
// Last-resort record of what each building collider was built from, so a static
// rebuild only touches geometry that actually changed.
const colliderCache = new Map();
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
// Static geometry is uploaded once as a single buffer, but laid out as one
// contiguous run per 480-unit cell. Each run keeps its own bounds, so the draw
// loop can reject a cell with a frustum test and submit a sub-range instead of
// the whole city. Before this, the entire streaming radius went to the GPU
// every frame regardless of where the camera pointed.
let staticChunks = [];
let visibleChunkKeys = [];
let visibleChunks = 0;
let culledChunks = 0;
let submittedStaticVertices = 0;
const frustumPlanes = new Float32Array(24);
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
// Set by interact() and consumed by the next mission update. Keeping the press
// in one place means the stage rules decide whether it counts, rather than
// every call site that happens to be near a target.
let pendingInteract = false;
let pendingInteractId = undefined;
let toastTimer = 0;
let debugVisible = false;
let firstPerson = false;
let yaw = Math.PI;
let pitch = -0.24;
let pointerLocked = false;
/**
 * Input state.
 *
 * Registered before the legacy listener further down this file, which is why
 * the handlers below call `stopImmediatePropagation`: two handlers for one
 * event is how a key press ends up firing an action twice, once from the
 * binding table and once from a hard-coded `if (k === 'e')`. The legacy handler
 * is dead code kept only until this file's tail is next edited; nothing reaches
 * it.
 */
const input = createInput();

/** One place that turns a press into an effect. */
function dispatchInputActions(){
  // Touch and the on-screen RUN button are not keyboard events, so they are
  // folded into the same action state rather than being special-cased at every
  // call site.
  setTouchStick(input,touchMove.x,-touchMove.y,(touchMove.x!==0||touchMove.y!==0));
  pollGamepad();
  if(input.mouse.dx||input.mouse.dy){
    yaw-=input.mouse.dx*0.0023;
    pitch=clamp(pitch-input.mouse.dy*0.0020,-1.05,0.35);
  }
  if(wasPressed(input,'interact'))interact();
  if(wasPressed(input,'toggleRenderer'))cycleRenderer();
  if(wasPressed(input,'toggleQuality'))cycleQuality();
  if(wasPressed(input,'toggleCamera')){firstPerson=!firstPerson;showToast(firstPerson?'First-person camera':'Third-person camera');}
  if(wasPressed(input,'toggleTelemetry')){debugVisible=!debugVisible;$('debug').classList.toggle('show',debugVisible);}
  if(wasPressed(input,'save'))saveGame();
  if(wasPressed(input,'load')){if(loadGame()){buildWorldIndex();streamKey='';showToast('Game loaded.');}}
  if(wasPressed(input,'newWorld'))newWorld();
  if(wasPressed(input,'releasePointer')&&document.pointerLockElement===canvas)document.exitPointerLock?.();
}

/**
 * Read the first connected gamepad, if there is one.
 *
 * Polled rather than event-driven because the Gamepad API only exposes state,
 * never events: there is no "button pressed" to subscribe to. A disconnected
 * pad is simply not polled, and its held actions are released so the player does
 * not keep walking after unplugging it.
 */
function pollGamepad(){
  const pads=typeof navigator!=='undefined'&&navigator.getGamepads?navigator.getGamepads():null;
  if(!pads)return;
  let pad=null;
  for(const p of pads)if(p&&p.connected){pad=p;break;}
  if(!pad){
    for(const action of input.down)if(['jump','interact','sprint'].includes(action))input.down.delete(action);
    input.stick.x=0;input.stick.y=0;
    return;
  }
  setStick(input,pad.axes[0]||0,pad.axes[1]||0);
  const buttons=pad.buttons||[];
  for(let i=0;i<buttons.length;i++)setButton(input,i,!!(buttons[i]&&buttons[i].pressed));
}

let touchMove = { x: 0, y: 0 };
let touchSprint = false;

// Keyboard and mouse, bound before the legacy handlers at the end of this file
// so that those never see an event. See the note on `input` above.
/**
 * `KeyboardEvent.code` for the keys a fallback is worth having.
 *
 * `code` is the physical key and is what bindings are written against, because
 * `key` is whatever character the layout produces. Every real keyboard event
 * carries it. Synthetic events, some remote-desktop bridges and some assistive
 * tools do not, and a game that silently stops responding to those is worse
 * than one that guesses: this covers the movement cluster and nothing else, so
 * an unknown key is ignored rather than mis-bound.
 */
const KEY_CODE_FALLBACK={'w':'KeyW','a':'KeyA','s':'KeyS','d':'KeyD','e':'KeyE','q':'KeyQ','t':'KeyT','v':'KeyV','n':'KeyN',' ':'Space','arrowup':'ArrowUp','arrowdown':'ArrowDown','arrowleft':'ArrowLeft','arrowright':'ArrowRight','shift':'ShiftLeft','escape':'Escape','enter':'Enter'};
const eventCode=e=>e.code||KEY_CODE_FALLBACK[String(e.key||'').toLowerCase()]||'';

addEventListener('keydown',e=>{
  setKey(input,eventCode(e),true);
  initializeAudio();
  // Space and the arrows scroll the page out from under the player.
  const code=eventCode(e);
  if(isCapturing(input)||code==='Space'||code.startsWith('Arrow'))e.preventDefault();
  e.stopImmediatePropagation();
});
addEventListener('keyup',e=>{setKey(input,eventCode(e),false);e.stopImmediatePropagation();});
addEventListener('mousemove',e=>{if(pointerLocked)addMouseDelta(input,e.movementX||0,e.movementY||0);});
// Camera heights, measured from the player's feet. The pre-physics code kept the
// body 0.55 above the terrain and hung the camera a further 1.55 (first person)
// or 4.8 (chase) above that; these reproduce exactly the same framing now that
// the simulation reports the feet position directly.
const EYE_ABOVE_FEET = 2.10;
const CHASE_ABOVE_FEET = 5.35;
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

/** The shadow pass is position-only: three floats per vertex. */
const SHADOW_FLOATS = 3;
const SHADOW_BYTES = SHADOW_FLOATS * 4;

/**
 * The material table and the geometry kit.
 *
 * Built once at module load. `materials.mjs` merges the baked texture set with
 * the untextured solids into one index space; `geometry.mjs` binds every
 * emitter to it. Nothing below this point names a material by anything other
 * than its id string, and every one of those strings is checked at the point of
 * use.
 */
const MATERIALS = buildMaterialTable(MATERIAL_DESCRIPTOR);
const G = createGeometryKit(MATERIALS);

/**
 * Which texture the terrain uses, by biome.
 *
 * Three ground materials rather than one, because a single ground texture over
 * a whole city is the most obvious way a large world looks synthetic. The
 * biome is a coarse 1200-unit grid from the world generator, so the boundaries
 * fall on grid lines and the transition can be hidden by scattering.
 */
/**
 * GPU-side material state.
 *
 * Starts not-ready: the first frames render with flat vertex colours, which is
 * what the renderer did before materials existed and is a correct — if plain —
 * fallback. The PBR path takes over when the last array is uploaded. A player
 * never sees a loading screen for it, and a failure to load one wall's texture
 * degrades one wall rather than preventing the game from starting.
 */
const materialState = { ready: false, failed: [], loaded: 0, total: 0, textures: {} };
let materialUpload = null;

/** Start the upload. Called once, at boot, and it never blocks. */
function startMaterialUpload() {
  materialUpload = createMaterialTextures(gl, MATERIAL_DESCRIPTOR, (p) => {
    materialState.loaded = p.loaded;
    materialState.total = p.total;
    materialState.failed = p.failed;
  });
  materialUpload.promise.then((s) => {
    materialState.ready = s.ready;
    materialState.failed = s.failed;
    if (s.failed.length) {
      // Reported rather than swallowed. A silently missing material is
      // indistinguishable from a bug in the material table, and this is the one
      // line that tells them apart.
      console.warn(`EMERGENT: ${s.failed.length} of ${s.total} material tiles failed to load:`);
      for (const f of s.failed.slice(0, 8)) console.warn('  ' + f);
    }
  }).catch((err) => {
    console.warn('EMERGENT: material upload failed, falling back to untextured rendering —', err);
  });
}

function bindMaterialTextures() {
  if (!materialState.ready) return;
  bindTextures(gl, materialState);
}

function groundMaterial(biome){
  if (biome === 'lush') return 'terrain_grass';
  if (biome === 'dry') return 'terrain_dirt';
  return hash2(player ? player.x * 0.004 : 0, player ? player.z * 0.004 : 0, seed) > 0.5 ? 'terrain_grass' : 'terrain_dirt';
}

function createProgram(vsSource, fsSource) {
  const compile=(type,src)=>{const s=gl.createShader(type);gl.shaderSource(s,src);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(s));return s;};
  const p=gl.createProgram();gl.attachShader(p,compile(gl.VERTEX_SHADER,vsSource));gl.attachShader(p,compile(gl.FRAGMENT_SHADER,fsSource));gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(p));return p;
}

// One shader for every surface in the game. It has no idea what a wall or a
// car is: it reads the material index off the vertex, resolves it through the
// uploaded material table, and shades whatever comes out. That is the whole
// point of the table — a new surface is a new row, not a new shader.
//
// The material table is a pair of vec4 arrays rather than a data texture,
// because it is a few hundred bytes and a texture would cost a sampler and a
// cache line to read the same numbers. The array size is baked in from
// MAX_MATERIALS so the two can never disagree.
const MAT_UNIFORMS = MAX_MATERIALS;

const sceneVS = `#version 300 es
precision highp float;
layout(location=0) in vec3 p; layout(location=1) in vec3 n; layout(location=2) in vec3 c; layout(location=3) in float m; layout(location=4) in vec2 uv;
uniform mat4 uPV; uniform vec3 uCam; uniform float uTime; uniform float uWater;
uniform vec4 uMatA[${MAT_UNIFORMS}]; uniform vec4 uMatB[${MAT_UNIFORMS}];
out vec3 vN; out vec3 vC; out float vD; out vec3 vP; out vec2 vUV; out float vMat; out float vT; out float vUseUV;
void main(){
  vec3 q=p; if(uWater>0.5) q.y += sin(q.x*0.025+uTime*1.6)*0.08 + cos(q.z*0.021+uTime)*0.06;
  gl_Position=uPV*vec4(q,1.0);
  vN=n; vC=c; vD=distance(q,uCam); vP=q; vMat=m;
  int mi=int(m+0.5);
  float mode=uMatB[mi].x;
  // Box mapping. The plane comes from the material's recorded role rather than
  // from the normal, so a road tiles as ground and a facade tiles as a wall
  // even where a chamfer or a pitched roof made the normal ambiguous. World
  // locked, so a brick wall's bricks stay put while the player walks past.
  //
  // MAP_MODE.UV is the exception: an imported mesh brings its own coordinates
  // because no world-axis projection can put a wood grain along a tapered leg.
  // The flag is interpolated so the branch happens once per vertex, not once
  // per fragment.
  vUseUV=step(3.5,mode);
  vec2 proj = p.xz;
  if(mode>0.5 && mode<1.5) proj=p.zy; else if(mode>1.5 && mode<2.5) proj=p.xy;
  vUV=mix(proj*uMatA[mi].y, uv, vUseUV);
  vT=uMatB[mi].w;
}`;

const sceneFS = `#version 300 es
precision highp float;
in vec3 vN; in vec3 vC; in float vD; in vec3 vP; in vec2 vUV; in float vMat; in float vT; in float vUseUV;
uniform vec3 uSun; uniform vec3 uFog; uniform vec3 uAmbient; uniform float uFogDensity; uniform float uNight; uniform float uWater;
uniform vec3 uSunColor; uniform vec3 uSkyColor; uniform vec3 uGroundColor;
uniform vec4 uMatA[${MAT_UNIFORMS}]; uniform vec4 uMatB[${MAT_UNIFORMS}];
uniform sampler2DArray uAlbedoTex; uniform sampler2DArray uNormalTex; uniform sampler2DArray uArmTex;
uniform float uMaterialsReady;
out vec4 outC;
const float PI=3.14159265;
float d_ggx(float NoH,float a){float a2=a*a;float d=NoH*NoH*(a2-1.0)+1.0;return a2/max(PI*d*d,1e-7);}
float g_smith(float NoV,float NoL,float a){float k=a*0.5;return (NoV/(NoV*(1.0-k)+k))*(NoL/(NoL*(1.0-k)+k));}
vec3 fres(float u,vec3 f0){return f0+(1.0-f0)*pow(clamp(1.0-u,0.0,1.0),5.0);}
void main(){
  int mi=int(vMat+0.5);
  vec4 A=uMatA[mi]; vec4 B=uMatB[mi];
  // Face the normal at the eye. Without this, a box corner built from two
  // opposing faces shades both of them identically and every solid reads flat.
  vec3 Ng=normalize(vN);
  vec3 V=normalize(uCam-vP);
  vec3 N=dot(Ng,V)<0.0?-Ng:Ng;
  vec3 L=normalize(uSun);
  vec3 albedo=vC; float rough=A.z; float metal=A.w; float ao=1.0; float emis=B.z;
  if(vT>0.5 && uMaterialsReady>0.5){
    int layer=int(A.x);
    vec4 alb=texture(uAlbedoTex,vec3(vUV,float(layer)));
    vec4 arm=texture(uArmTex,vec3(vUV,float(layer)));
    vec3 nt=texture(uNormalTex,vec3(vUV,float(layer))).xyz*2.0-1.0;
    vec3 T,Bt;
    if(vUseUV>0.5){
      // Screen-space derivatives of position and UV give the tangent frame for
      // an arbitrary mesh without storing tangents, which is the standard
      // technique and costs two derivatives instead of four floats a vertex.
      // Degenerate when a triangle is edge-on or has a mirrored UV island, so it
      // falls back to an arbitrary perpendicular rather than a NaN.
      vec3 dp1=dFdx(vP), dp2=dFdy(vP);
      vec2 du1=dFdx(vUV), du2=dFdy(vUV);
      float det=du1.x*du2.y-du1.y*du2.x;
      vec3 Tt=abs(det)>1e-12?(dp1*du2.y-dp2*du1.y)/det:vec3(0.0);
      T=normalize(Tt-N*dot(N,Tt));
      if(dot(T,T)<0.5) T=normalize(cross(abs(N.y)>0.7?vec3(0.0,0.0,1.0):vec3(0.0,1.0,0.0),N));
    } else {
      // A box-projected surface's tangent is one world axis, derived from the
      // same plane the coordinate came from. No tangents in the vertex format.
      T=normalize(cross(abs(N.y)>0.7?vec3(0.0,0.0,1.0):vec3(0.0,1.0,0.0),N));
    }
    Bt=cross(N,T);
    nt.xy*=B.y;
    albedo*=alb.rgb;
    ao=arm.r; rough=clamp(arm.g,0.04,1.0); metal=arm.b;
    N=normalize(mat3(T,Bt,N)*nt);
  }
  vec3 f0=mix(vec3(0.04),albedo,metal);
  vec3 diffC=albedo*(1.0-metal);
  float NoV=max(dot(N,V),1e-4);
  vec3 direct=vec3(0.0);
  float NoL=dot(N,L);
  if(NoL>0.0){
    vec3 H=normalize(L+V);
    float NoH=max(dot(N,H),0.0); float VoH=max(dot(V,H),0.0);
    float a=rough*rough;
    vec3 spec=fres(VoH,f0)*(d_ggx(NoH,a)*g_smith(NoV,NoL,a)/max(4.0*NoV*NoL,1e-5));
    direct=(diffC/PI+spec)*uSunColor*NoL;
  }
  // Hemispherical ambient: a two-colour sky/ground irradiance. Not an IBL, but
  // it is what keeps a shaded wall off flat black and gives a rough surface
  // something to reflect, and it costs two uniforms.
  vec3 amb=mix(uGroundColor,uSkyColor,N.y*0.5+0.5);
  vec3 R=reflect(-V,N);
  vec3 env=mix(uGroundColor,uSkyColor,R.y*0.5+0.5);
  vec3 ambient=diffC*amb*ao+env*fres(NoV,f0)*(1.0-rough*0.8)*ao*mix(0.30,1.0,metal);
  vec3 c=direct+ambient+albedo*emis*(0.25+uNight*0.95);
  if(uWater>0.5) c=mix(c,vec3(0.08,0.25,0.35),0.42);
  float fog=1.0-exp(-vD*vD*uFogDensity); c=mix(c,uFog,fog);
  outC=vec4(c,1.0);
}`;

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
  uSunColor:gl.getUniformLocation(sceneProg,'uSunColor'),uSkyColor:gl.getUniformLocation(sceneProg,'uSkyColor'),uGroundColor:gl.getUniformLocation(sceneProg,'uGroundColor'),
  uMatA:gl.getUniformLocation(sceneProg,'uMatA'),uMatB:gl.getUniformLocation(sceneProg,'uMatB'),
  uAlbedoTex:gl.getUniformLocation(sceneProg,'uAlbedoTex'),uNormalTex:gl.getUniformLocation(sceneProg,'uNormalTex'),uArmTex:gl.getUniformLocation(sceneProg,'uArmTex'),
  uMaterialsReady:gl.getUniformLocation(sceneProg,'uMaterialsReady'),
  // GLSL array uniforms must be bound by name with either [0] or the bare name.
  // A bare lookup for an array can return null on some drivers, which fails
  // silently as "no material ever changed", so the [0] form is used explicitly.
  uMatA0:gl.getUniformLocation(sceneProg,'uMatA[0]'),uMatB0:gl.getUniformLocation(sceneProg,'uMatB[0]'),
  sPV:gl.getUniformLocation(shadowProg,'uPV')
};

// position(3) normal(3) colour(3) material(1) — 10 floats, 40 bytes. The
// material index is a float rather than an integer attribute because a fourth
// integer stream would need its own divisor setup for a value that is only
// ever used as an array index, and the cost of the float is one byte per
// vertex in a buffer the CPU rebuilds every frame anyway.
const STRIDE = VERTEX_BYTES;

/**
 * Point the four vertex attributes at the currently bound array buffer.
 *
 * The stride is shared by the static, dynamic and shadow buffers because they
 * all use the same vertex layout, so it is written once here rather than three
 * times inline. The shadow buffer is position-only and gets a separate
 * three-float layout.
 */
function setAttributePointers(){
  gl.vertexAttribPointer(0,3,gl.FLOAT,false,VERTEX_BYTES,0);
  gl.vertexAttribPointer(1,3,gl.FLOAT,false,VERTEX_BYTES,12);
  gl.vertexAttribPointer(2,3,gl.FLOAT,false,VERTEX_BYTES,24);
  gl.vertexAttribPointer(3,1,gl.FLOAT,false,VERTEX_BYTES,36);
  gl.vertexAttribPointer(4,2,gl.FLOAT,false,VERTEX_BYTES,40);
}
gl.bindVertexArray(vao);
gl.bindBuffer(gl.ARRAY_BUFFER, staticBuf);
gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,3,gl.FLOAT,false,STRIDE,0);
gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1,3,gl.FLOAT,false,STRIDE,12);
gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2,3,gl.FLOAT,false,STRIDE,24);
gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3,1,gl.FLOAT,false,STRIDE,36);
gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4,2,gl.FLOAT,false,STRIDE,40);
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

function terrainChunk(builder,cx,cz,radius,detail){
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
    // Each quad is routed by its own centre, so a quad straddling a cell
    // boundary lands wholly in one cell. That keeps every cell's run
    // contiguous, which is what makes culling a sub-range draw.
    G.quad(builder.target((x0+x1)/2,(z0+z1)/2),[x0,y00,z0],[x1,y10,z0],[x1,y11,z1],[x0,y01,z1],[0,1,0],c,G.M(groundMaterial(biome)));
  }
}

const CHUNK_SIZE = 480;

/**
 * Accumulates geometry into per-cell vertex runs.
 *
 * The key property is that a cell's vertices end up contiguous in the final
 * buffer, so culling is a `drawArrays(first, count)` sub-range rather than a
 * buffer rebind. One upload, many draws, no VAO churn.
 */
function makeChunkBuilder() {
  const cells = new Map();
  return {
    cells,
    /** The vertex array for the cell containing (x, z), created on demand. */
    target(x, z) {
      const key = `${Math.floor(x / CHUNK_SIZE)},${Math.floor(z / CHUNK_SIZE)}`;
      let arr = cells.get(key);
      if (!arr) { arr = []; cells.set(key, arr); }
      return arr;
    }
  };
}

/**
 * Axis-aligned bounds of a run of vertices.
 *
 * Walks the shared vertex stride rather than a literal, because the stride
 * changed from 9 to 10 when materials arrived and a hard-coded 9 here silently
 * produced fractional run boundaries — which the frustum culler then used to
 * draw ranges that start and end inside a vertex.
 */
function boundsOf(vertices, first, count) {
  let minX = Infinity, minY = Infinity, minZ = Infinity;
  let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i < count; i++) {
    const o = (first + i) * VERTEX_FLOATS;
    const x = vertices[o], y = vertices[o + 1], z = vertices[o + 2];
    if (x < minX) minX = x; if (y < minY) minY = y; if (z < minZ) minZ = z;
    if (x > maxX) maxX = x; if (y > maxY) maxY = y; if (z > maxZ) maxZ = z;
  }
  if (count === 0) return { min: [0, 0, 0], max: [0, 0, 0] };
  return { min: [minX, minY, minZ], max: [maxX, maxY, maxZ] };
}

/** Concatenate the builder's cells into one buffer, recording each cell's run. */
function flattenCells(cells) {
  let total = 0;
  for (const arr of cells.values()) total += arr.length;
  const vertices = new Float32Array(total);
  const runs = [];
  let offset = 0;
  for (const [key, arr] of cells) {
    const count = arr.length / VERTEX_FLOATS;
    if (count === 0) continue;
    vertices.set(arr, offset * VERTEX_FLOATS);
    const bounds = boundsOf(vertices, offset, count);
    runs.push({ key, first: offset, count, min: bounds.min, max: bounds.max });
    offset += count;
  }
  return { vertices, runs };
}

let staticBuildMs=0, dynamicBuildMs=0, staticBytes=0;

function buildStaticScene() {
  if (!world || !player) return;
  const t0=performance.now();
  const builder = makeChunkBuilder();
  const shadowBuilder = makeChunkBuilder();
  const radius = qualityLevel===0?680:qualityLevel===1?800:qualityLevel===2?980:1160;
  const adaptive = rendererMode===1;
  const staticDetail=adaptive?0.55+qualityLevel*0.12:0.82+qualityLevel*0.06;
  // The distance over which detail is spent, as a fraction of what is actually
  // streamed.
  //
  // Streaming radius and detail radius are different things and conflating them
  // is what makes procedural cities either empty at 300 m or unaffordable. The
  // stream exists so the world continues past the horizon; the detail radius
  // exists so that a building the player can actually read has its windows, its
  // cornice and its roof plant, and one 800 m away is a correctly materialled
  // mass with the right silhouette. The exponent front-loads the falloff, which
  // puts the budget where a player is looking instead of spreading it evenly
  // over ground they will never walk on.
  const detailRadius=(260+qualityLevel*180)*(adaptive?0.6:1);
  terrainChunk(builder,player.x,player.z,radius,staticDetail);
  const activeChunks = refreshStreamResidency(radius);
  syncBuildingColliders(activeChunks);
  const addCellObjects=(kind, cb)=>{for(const key of activeChunks){const list=streamResidency.chunks.get(key)?.[kind];if(!list)continue;for(const o of list)cb(o);}};
  const minX=player.x-radius,maxX=player.x+radius,minZ=player.z-radius,maxZ=player.z+radius;
  // Sun state is fixed for the duration of a static build, so resolve it once
  // instead of recomputing trig per building.
  const sun=sunState(), castShadows=sun.day>0.08, sx=sun.dir[0], sy=Math.max(0.2,sun.dir[1]), sz=sun.dir[2];
  // Signs and windows are lit at night, and the static scene is rebuilt on the
  // day/night cycle, so this is resolved once per build rather than per object.
  const isNight=sun.day<0.34;
  addCellObjects('roads',r=>{
    if(r.x+r.w<minX||r.x>maxX||r.z+r.d<minZ||r.z>maxZ)return;
    // Roads get their own detail budget against the same radius, so a street
    // the player is on has its markings, kerbs and lamps and one across the
    // district is still a correctly surfaced carriageway.
    buildRoad(G,builder.target(r.x,r.z),r,clamp(1-Math.hypot(r.x+r.w/2-player.x,r.z+r.d/2-player.z)/detailRadius,0,1)*staticDetail,seed);
  });
  addCellObjects('buildings',b=>{
    if(b.x+b.w/2<minX||b.x-b.w/2>maxX||b.z+b.d/2<minZ||b.z-b.d/2>maxZ)return;
    const d=Math.hypot(b.x-player.x,b.z-player.z); const detail=clamp(1-d/detailRadius,0,1)*staticDetail;
    const arr=builder.target(b.x,b.z);
    buildBuilding(G,arr,b,detail,seed,isNight);
    if(detail>0.34) dressBuilding(G,arr,b,detail,seed,isNight);
    // Project a simple soft sun shadow on the local terrain; geometry, not a screen-space fake.
    if(d<720 && castShadows) {
      const ext=b.h/Math.max(0.18,sy);
      const ox=-sx*ext*0.65, oz=-sz*ext*0.65;
      const x0=b.x-b.w*.46+ox, x1=b.x+b.w*.46+ox, z0=b.z-b.d*.46+oz, z1=b.z+b.d*.46+oz;
      const y0=terrainHeight(x0,z0,seed)+0.045, y1=terrainHeight(x1,z0,seed)+0.045, y2=terrainHeight(x1,z1,seed)+0.045, y3=terrainHeight(x0,z1,seed)+0.045;
      // Routed by the shadow's own centre, not the building's: the offset can
      // be tens of units and would otherwise land the quad in the neighbouring
      // cell, where it would then be culled with the wrong bounds.
      const sh=shadowBuilder.target(b.x+ox,b.z+oz);
      sh.push(x0,y0,z0,x1,y1,z0,x1,y2,z1,x0,y0,z0,x1,y2,z1,x0,y3,z1);
    }
  });
  let treeStep = adaptive ? (qualityLevel===0?7:qualityLevel===1?5:qualityLevel===2?3:2) : (qualityLevel===0?5:qualityLevel===1?3:1);
  let counter=0;
  addCellObjects('trees',t=>{
    if(t.x<minX||t.x>maxX||t.z<minZ||t.z>maxZ)return;
    if((counter++ % treeStep)!==0)return;
    const d=Math.hypot(t.x-player.x,t.z-player.z); if(adaptive && d>700 && hash2(t.x*0.01,t.z*0.01,seed)>0.42)return;
    buildTree(G,builder.target(t.x,t.z),t,clamp(1-d/detailRadius,0,1)*staticDetail,seed);
  });
  // River is a separate animated water surface with real depth-tested geometry.
  for(const w of world.water){if(Math.abs(w.x-player.x)<radius+260 && Math.abs(w.z-player.z)<radius+260){const y=terrainHeight(w.x,w.z,seed)+0.15;const c=[0.06,0.22,0.32];G.box(builder.target(w.x,w.z),w.x,y,w.z,w.w,0.04,w.d,c,G.M('terrain_mud'));}}

  const staticData = flattenCells(builder.cells);
  const shadowData = flattenCells(shadowBuilder.cells);
  gl.bindBuffer(gl.ARRAY_BUFFER,staticBuf);gl.bufferData(gl.ARRAY_BUFFER,staticData.vertices,gl.STATIC_DRAW);
  staticVertexCount=staticData.vertices.length/VERTEX_FLOATS;
  staticChunks=staticData.runs;
  gl.bindBuffer(gl.ARRAY_BUFFER,shadowBuf);gl.bufferData(gl.ARRAY_BUFFER,shadowData.vertices,gl.STATIC_DRAW);shadowVertexCount=shadowData.vertices.length/VERTEX_FLOATS;
  staticBytes=staticData.vertices.byteLength;
  streamOps++; streamGenerated=streamResidency.generated; streamFreed=streamResidency.evicted;
  streamKey=`${keyCell(player.x,player.z)}|${radius}|${qualityLevel}|${rendererMode}`;
  if($('loading')) $('loading').style.display='none';
  staticBuildMs=performance.now()-t0;
  return staticBuildMs;
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
    // Visual LOD. A character is under ten pixels tall at sixty metres and a
    // car under twenty at a hundred and fifty, so both drop detail well before
    // they drop out of the world.
    if(d<70) buildCharacter(G,arr,st.x,st.z,undefined,n.id,n.job,n.activity,elapsed*2+n.id*0.7,seed,0);
    else if(d<260) buildCharacter(G,arr,st.x,st.z,undefined,n.id,n.job,n.activity,elapsed*2+n.id*0.7,seed,1);
    else buildCharacterProxy(G,arr,st.x,st.z,undefined,n.id,n.job,n.activity,elapsed*2+n.id*0.7,seed);
    visibleObjects++;simulatedNpcCount++;
  }
  for(const c of world.cars){
    const d=Math.hypot(c.x-player.x,c.z-player.z);if(d>carNear)continue;
    if(d<160) buildCar(G,arr,c,seed,0);
    else if(d<520) buildCar(G,arr,c,seed,1);
    else buildCarProxy(G,arr,c,seed);
    recomputed++;visibleObjects++;
  }
  const y=terrainHeight(player.x,player.z,seed);
  if(firstPerson) G.box(arr,player.x,y,player.z,0.45,0.9,0.32,[0.18,0.32,0.68],G.M('fabric'));
  else buildCharacter(G,arr,player.x,player.z,y,9999,'service','idle',elapsed*2,seed);
  if(currentMission){
    const stage=currentStage(currentMission);
    if(stage&&Number.isFinite(stage.x)){const my=terrainHeight(stage.x,stage.z,seed);G.cylinder(arr,stage.x,my,stage.z,0.7,5.5,[0.95,0.68,0.25],G.M('lamp'),8);G.cylinder(arr,stage.x,my+5.5,stage.z,1.2,0.08,[1.0,0.78,0.30],G.M('lamp'),8);}
  }
  gl.bindBuffer(gl.ARRAY_BUFFER,dynamicBuf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(arr),gl.DYNAMIC_DRAW);dynamicVertexCount=arr.length/VERTEX_FLOATS;
  dynamicBuildTime=elapsed;lastDynamicX=player.x;lastDynamicZ=player.z;dynamicBuilds++;lastDynamicObjects=visibleObjects;
  dynamicBuildMs=performance.now()-t0;
  return dynamicBuildMs;
}
function shadowDraw(pv) {
  if(!shadowVertexCount) return;
  gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA); gl.depthMask(false);
  gl.useProgram(shadowProg); gl.uniformMatrix4fv(loc.sPV,false,pv); gl.bindBuffer(gl.ARRAY_BUFFER,shadowBuf); gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0,3,gl.FLOAT,false,12,0); gl.drawArrays(gl.TRIANGLES,0,shadowVertexCount);
  gl.depthMask(true); gl.disable(gl.BLEND);
}

function cameraMatrix(){
  const eyeY=player.y+(firstPerson?EYE_ABOVE_FEET:CHASE_ABOVE_FEET);
  // EYE_ABOVE_FEET / CHASE_ABOVE_FEET replace the old body-offset + eye-offset pair.
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
  // Direct light is warm at low sun and neutral at noon; ambient is the sky
  // above and the bounced ground below. Without the split, every surface in the
  // world is lit by one colour and reads as a single flat wash.
  const sunWarm=Math.max(0,1-sunState().day*1.35);
  gl.uniform3f(loc.uSunColor,(0.95+0.55*sunWarm)*s.day,(0.94+0.12*sunWarm)*s.day,(0.90-0.16*sunWarm)*s.day);
  gl.uniform3f(loc.uSkyColor,(0.30+0.34*s.day)*(1-night*0.72),(0.38+0.40*s.day)*(1-night*0.70),(0.52+0.44*s.day)*(1-night*0.60));
  gl.uniform3f(loc.uGroundColor,(0.16+0.12*s.day)*(1-night*0.80),(0.15+0.11*s.day)*(1-night*0.80),(0.13+0.09*s.day)*(1-night*0.82));
  gl.uniform4fv(loc.uMatA0,MATERIALS.a); gl.uniform4fv(loc.uMatB0,MATERIALS.b);
  gl.uniform1f(loc.uMaterialsReady,materialState.ready?1:0);
  // The sampler units are set once, not per frame: a unit assignment is a
  // property of the program, and setting it every frame is a way for a
  // rebind elsewhere to quietly point the shader at the wrong array.
  bindMaterialTextures();
  gl.uniform1i(loc.uAlbedoTex,UNITS.albedo);
  gl.uniform1i(loc.uNormalTex,UNITS.normal);
  gl.uniform1i(loc.uArmTex,UNITS.arm);
  gl.bindBuffer(gl.ARRAY_BUFFER,staticBuf); setAttributePointers();
  // Frustum-cull the static cells. Each run is a contiguous slice of the one
  // static buffer, so a rejected cell costs a plane test and nothing else: no
  // rebind, no re-upload, no per-object draw call. The whole point is that a
  // camera facing away from most of the city stops paying for it.
  extractFrustumPlanes(pv, frustumPlanes);
  visibleChunks=0; culledChunks=0; submittedStaticVertices=0;
  // Reused rather than reallocated: the keys are already-retained strings from
  // the run table, so a per-frame array would be the only garbage in the loop.
  visibleChunkKeys.length=0;
  for(const run of staticChunks){
    if(!aabbVisible(frustumPlanes,run.min,run.max)){culledChunks++;continue;}
    gl.drawArrays(gl.TRIANGLES,run.first,run.count);
    visibleChunks++; submittedStaticVertices+=run.count; visibleChunkKeys.push(run.key);
  }
  shadowDraw(pv);
  gl.bindBuffer(gl.ARRAY_BUFFER,dynamicBuf); setAttributePointers(); gl.drawArrays(gl.TRIANGLES,0,dynamicVertexCount);
  gl.bindVertexArray(null);
  renderMs=performance.now()-t0;
}

/**
 * Project the current mission stage into screen space and draw a gold chevron
 * + distance ring above it on the 2D overlay. Skipped when the target is behind
 * the camera or when the simulation hasn't rendered yet. The marker follows the
 * *stage*, not a fixed target building, which is what lets a job change where it
 * is pointing without the renderer knowing anything about mission types.
 */
function drawMissionMarker(){
  if(!currentMission||!lastPV)return;
  const stage=currentStage(currentMission);
  if(!stage||!Number.isFinite(stage.x))return;
  const my=terrainHeight(stage.x,stage.z,seed)+6;
  const clip=transformPoint(lastPV,[stage.x,my,stage.z]);
  if(clip[3]<=0.001)return; // behind the camera
  const sx=(clip[0]/clip[3]*0.5+0.5)*overlay.width;
  const sy=(1-(clip[1]/clip[3]*0.5+0.5))*overlay.height;
  const dist=stageDistance(stage,player.x,player.z);
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

/**
 * How far away an NPC or car has to be before it stops being solid, and how
 * many get a body at once.
 *
 * The radius is a reachability bound rather than a quality setting: nothing
 * beyond it can be touched, so a collider out there costs solver time and buys
 * nothing. The budget exists because the crowd near a district centre is not a
 * bounded number; taking the nearest first means the solver's time goes to the
 * agents the player could collide with this frame.
 */
const AGENT_COLLIDER_RADIUS = 260;
const AGENT_COLLIDER_BUDGET = 64;

/**
 * Give the nearby crowd and traffic real collision bodies.
 *
 * Kinematic rather than dynamic on purpose: NPCs follow scripted goals and cars
 * follow lanes, so simulating them with forces would be slower, less
 * controllable, and would give a crowd that shoves the player around rather
 * than one to walk around. Synced every frame as a set, keyed by entity, so an
 * agent that leaves the radius has its body removed and one that enters gets
 * one created. The positions are one frame behind the simulation that drives
 * them, which for a body the player closes on at 300 units a second is well
 * inside a frame of travel.
 */
function syncAgentColliders(){
  if(!physics)return;
  const wanted=new Set();
  const near=[];
  for(const n of world.npcs){
    const d=Math.hypot(n.x-player.x,n.z-player.z);
    if(d<AGENT_COLLIDER_RADIUS)near.push({d,key:`npc${n.id}`,x:n.x,z:n.z,hx:0.45,hy:0.8,hz:0.35});
  }
  for(const c of world.cars){
    const d=Math.hypot(c.x-player.x,c.z-player.z);
    if(d<AGENT_COLLIDER_RADIUS)near.push({d,key:`car${c.id}`,x:c.x,z:c.z,hx:c.horizontal?2.0:1.0,hy:0.6,hz:c.horizontal?1.0:2.0});
  }
  near.sort((a,b)=>a.d-b.d);
  const count=Math.min(near.length,AGENT_COLLIDER_BUDGET);
  for(let i=0;i<count;i++){
    const a=near[i];
    wanted.add(a.key);
    setAgentBox(physics,a.key,a.x,terrainHeight(a.x,a.z,seed)+a.hy,a.z,a.hx,a.hy,a.hz);
  }
  for(const key of physics.dynamicBodies.keys())if(!wanted.has(key))removeAgent(physics,key);
}

/**
 * Building colliders for the cells the streamer is currently holding.
 *
 * Keyed by `cell#index`, so this is a set-sync and not a diff: a cell the
 * streamer evicts has its keys simply stop being wanted, and a cell that is
 * rebuilt with different buildings replaces them by key. That matters because
 * this runs on every static rebuild, and a rebuild that could leave a stale
 * collider behind would produce a wall the player can see through.
 */
function syncBuildingColliders(activeChunks){
  if(!physics)return;
  const wanted=new Set();
  for(const key of activeChunks){
    const list=streamResidency.chunks.get(key)?.buildings;
    if(!list)continue;
    for(let i=0;i<list.length;i++){
      const b=list[i];
      // The visible box runs from the terrain up, so the collider is the same
      // box expressed as a centre and half extents.
      const base=terrainHeight(b.x,b.z,seed);
      const sig=`${b.x.toFixed(2)},${base.toFixed(2)},${b.z.toFixed(2)},${b.w},${b.h},${b.d}`;
      const k=`${key}#${i}`;
      wanted.add(k);
      if(colliderCache.get(k)===sig)continue;
      colliderCache.set(k,sig);
      setStaticBox(physics,k,b.x,base+b.h/2,b.z,b.w/2,b.h/2,b.d/2);
    }
  }
  for(const k of physics.buildingColliders.keys()){
    if(wanted.has(k))continue;
    colliderCache.delete(k);
    removeStaticBox(physics,k);
  }
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
    const demand=baseDemand*weatherMod*(0.72+world.economy*0.35)*(b.popularity||1);
    if(b.open && rand()<dt*demand){b.stock-=0.6;b.customers++;b.revenue+=b.price*0.4;}
    if(b.stock<5)b.open=false;
    // Passive resupply, logistic: it is strongest on an empty shelf and fades to
    // nothing on a full one.
    //
    // This used to be a flat trickle that only applied below 18, which is a hard
    // floor — the measured minimum stock across 714 businesses was exactly 18,
    // pinned there by this line, in every seed and at every point in time. With
    // a floor there is no such thing as a business in trouble, so the "low
    // stock" half of the delivery design was unreachable and the player's
    // deliveries moved goods between businesses that were all fine.
    //
    // A business now settles wherever its supply rate matches its consumption,
    // and that differs per business, so the city has a real distribution: some
    // districts are well served, some are not, and a delivery is how you fix
    // the ones that are not.
    if(b.stock<100)b.stock+=dt*0.010*(b.supply||1)*(1-b.stock/100);
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

/**
 * Choose and build the next job from what the world can currently support.
 *
 * Availability is world state, not a constant: a delivery needs a stocked
 * source and a short target, a restock needs a business that has run dry, a
 * response needs a live incident. A job that could not be completed is not
 * offered, which is the difference between "a delivery appeared" and "a job
 * appeared that could be finished".
 */
/**
 * Choose and build the next job from what the world can currently support.
 *
 * All of the decision — which archetypes are possible, how a template becomes
 * a place, whether a target is even valid — lives in `missions.mjs`. This only
 * gathers the world state it needs and hands it over, so the rules are testable
 * without a city and the game has no opinion about job types.
 */
function chooseMission(){
  const asTarget=b=>{const bl=missionBuilding(b.id);return bl?{...bl,id:b.id,type:b.type,stock:b.stock}:null;};
  // Shortage thresholds are relative to the distribution the generator
  // produces (18..100 at birth) rather than absolute numbers the world may
  // never reach. An absolute threshold that nothing satisfies does not create
  // variety, it silently deletes job types.
  currentMission=createJob({
    rank:player.rank,rand,
    pool:{
      stocked:world.businesses.filter(b=>b.open&&b.stock>45).map(asTarget).filter(Boolean),
      short:world.businesses.filter(b=>!b.open||b.stock<26).map(asTarget).filter(Boolean),
      districts:world.districts,
      incidents:world.events.filter(e=>e.life>0&&e.severity>=0.2&&Number.isFinite(e.x)&&Number.isFinite(e.z)),
      awayFrom:player
    }
  });
  if(currentMission)missionTimer=45;
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
/**
 * Write the objective readout.
 *
 * Driven from the simulation rather than from `updateHUD` because the number
 * that matters here is the live distance to the current stage, and the HUD only
 * refreshes once a second — an objective counting down from 320m to 219m in
 * one jump reads as a stale UI, not as a game.
 */
function updateObjective(){
  if(!currentMission){$('objective').textContent='Explore the world — work will appear from local shortages and incidents.';return;}
  const stage=currentStage(currentMission);
  const d=stageDistance(stage,player.x,player.z);
  $('objective').textContent=`${objectiveText(currentMission)} · ${Number.isFinite(d)?`${Math.round(d)}m`:'—'}`;
}

/**
 * Mirror the current stage onto the legacy `stage`/`sourceId`/`targetId` fields.
 *
 * Those fields are read by the debug and test surface (`missionBuildingFor`),
 * which predates the stage pipeline and resolves a mission to a single
 * building. Keeping them in step means that surface follows the player around a
 * multi-stage job instead of pointing at a leg that was left hours ago. Nothing
 * in the game reads them; the game reads `currentStage()`.
 */
function syncMissionView(){
  const stage=currentStage(currentMission);
  if(!stage)return;
  currentMission.stage=stage.type==='interact'?'pickup':'delivery';
  currentMission.sourceId=stage.businessId;
  currentMission.targetId=stage.businessId;
  currentMission.crates=3;
}

function updateMission(dt){
  // A mission with no stages cannot be finished, and a job on the HUD that can
  // never be completed is worse than no job: it replaces the objective line
  // with something the player cannot act on. This is what a save written
  // before the stage pipeline restores to, so it is a case that really happens.
  if(currentMission&&(!Array.isArray(currentMission.stages)||!currentMission.stages.length)){
    currentMission=null;missionTimer=2;
  }
  if(!currentMission){missionTimer-=dt;if(missionTimer<=0)chooseMission();return;}
  syncMissionView();
  // The interact press is consumed by the stage rules, not by the interact
  // handler, so a job can never be completed by a keypress that happened for
  // some other reason. `pendingInteract` is set only by interact().
  const result=advanceMission(currentMission,{x:player.x,z:player.z,dt,interact:pendingInteract,interactId:pendingInteractId});
  pendingInteract=false;pendingInteractId=undefined;
  if(result.status==='stage'){
    beep(660,0.1);
    showToast(`Next: ${result.stage.label}.`);
    return;
  }
  if(result.status==='expired'){
    currentMission=null;missionTimer=15;showToast('Job expired. A new one will appear.');beep(220,0.2);return;
  }
  if(result.status==='complete')completeMission();
}

function interact(){
  initializeAudio();
  let nearest=null, best=70;
  for(const b of world.businesses){const bl=world.buildings[b.buildingId];if(!bl)continue;const d=Math.hypot(player.x-bl.x,player.z-bl.z);if(d<best){best=d;nearest={business:b,building:bl,d};}}
  // Report the press to the mission rules first. Whether it advances a job is
  // their decision, made against the stage's own target and radius, so a press
  // aimed at the wrong building cannot complete a job by accident.
  if(currentMission&&nearest){
    pendingInteract=true;
    pendingInteractId=nearest.business.id;
  }
  // Apply the world effect of a job stage the moment the press happens, rather
  // than a frame later when the rules accept it. The rules own *whether* a
  // stage completes; this owns what completing it does to the world.
  applyMissionStageEffect(nearest);
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

/**
 * The world effect of interacting at a business that a job stage is aimed at.
 *
 * Runs on the interact press rather than on the stage transition, so stock
 * moves when the player presses the key, not one simulation frame later, and
 * so the effect is applied at most once per press.
 */
function applyMissionStageEffect(nearest){
  const stage=currentStage(currentMission);
  if(!stage||!nearest||stage.type!=='interact')return;
  if(stage.businessId===undefined||stage.businessId!==nearest.business.id)return;
  if(stageDistance(stage,player.x,player.z)>stage.radius)return;
  if(stage.effectApplied)return;
  const b=nearest.business;
  if(stage.id==='collect'||stage.id==='supply'){
    // Supplies come out of a stocked business and into a dry one. A restock
    // draws on the district rather than a specific source, which is why the
    // world does not have to have a paired business for it.
    //
    // Moved in units the economy can actually feel: three crates against a
    // forty-unit shortfall is not a delivery, it is a rounding error, and the
    // business the player just helped looks exactly as empty as it did before.
    const crates=22;
    b.stock=clamp(b.stock+(stage.id==='collect'?-crates:crates),0,140);
    if(stage.id==='supply'){b.open=true;b.reputation=clamp(b.reputation+0.08,0,1.5);b.customers+=1;}
  }
  stage.effectApplied=true;
}

/** Pay out, rank up, and clear the job. Called by the rules on completion. */
function completeMission(){
  const done=currentMission;
  if(!done)return;
  player.money+=done.reward;
  player.energy=clamp(player.energy+9,0,100);
  player.missionsCompleted++;
  player.rank=1+Math.floor(player.missionsCompleted/3);
  const lastStage=done.stages[done.stages.length-1];
  if(lastStage&&Number.isFinite(lastStage.x))world.events.push({id:world.eventSerial++,type:'trade',x:lastStage.x,z:lastStage.z,t:18,life:18,severity:0.25,source:'mission'});
  showToast(`${done.label} complete +$${done.reward}. Rank ${player.rank}.`);
  beep(880,0.18);
  currentMission=null;missionTimer=18;saveGame();
}

function updatePlayer(dt){
  // Movement comes from the action layer, so keyboard, gamepad stick and touch
  // stick are one input rather than three special cases in this function.
  // `moveAxes` reports a screen-relative vector: +y is up, the direction the
  // player is asking to go. EMERGENT's heading convention is that walking
  // forward travels along -[sin(yaw), cos(yaw)] — it has been that way since the
  // first build, so it is not something to change silently here. The two signs
  // therefore disagree, and the disagreement is resolved once, at the point
  // where world axes first exist. The input module must not be told about world
  // axes: it is device-agnostic and is meant to stay that way.
  const axes=moveAxes(input);
  const ix=axes.x, iz=-axes.y;
  const sprint=isDown(input,'sprint')||touchSprint;
  let speed=sprint?DEFAULT_TUNING.sprintSpeed:DEFAULT_TUNING.walkSpeed;
  if(player.energy<18)speed*=0.62;
  if(world.weather===2)speed*=0.92;
  const forward=[Math.sin(yaw),Math.cos(yaw)], right=[Math.cos(yaw),-Math.sin(yaw)];

  // Fixed-timestep accumulator.
  //
  // Rapier advances on a fixed 1/60 step, so feeding it one move per rendered
  // frame would make gravity, jumping and sliding all run at the wrong rate on
  // any machine that is not sitting at exactly 60fps — the simulation would
  // change with the frame rate, which is the single most common way a character
  // controller ends up feeling broken on someone else's machine. The rendered
  // frame time decides how many fixed steps to run, not how big each one is.
  const jump=wasPressed(input,'jump');
  // The simulation is the authority on where the player is, and `player` is
  // written from its result every step, so any disagreement means something
  // outside the movement path moved the player: a loaded save, a new world, or
  // the debug teleport. Reconciling here means those paths cannot leave the
  // body behind in the old position, instead of each one having to remember.
  const bodyX=physics.player?physics.player.translation().x:player.x;
  const bodyZ=physics.player?physics.player.translation().z:player.z;
  if(Math.hypot(player.x-bodyX,player.z-bodyZ)>0.5)syncPlayerToPhysics();
  physicsAccumulator+=dt;
  let steps=0;
  while(physicsAccumulator>=PHYSICS_DT&&steps<MAX_PHYSICS_STEPS){
    const wishX=(forward[0]*iz+right[0]*ix)*speed*PHYSICS_DT;
    const wishZ=(forward[1]*iz+right[1]*ix)*speed*PHYSICS_DT;
    // The world bound clamps the *request*, not the result. Correcting the
    // position afterwards would place the player somewhere the sweep never
    // agreed to, which is how a player ends up inside a wall.
    const targetX=clamp(player.x+wishX,40,world.size-40);
    const targetZ=clamp(player.z+wishZ,40,world.size-40);
    const r=stepPlayer(physics,targetX-player.x,targetZ-player.z,{jump});
    player.x=r.x;player.z=r.z;player.y=r.y;
    player.grounded=r.grounded;
    physicsAccumulator-=PHYSICS_DT;
    steps++;
  }
  // A long stall (a tab regaining focus, a blocking call) must not be repaid
  // with dozens of catch-up steps; dropping the backlog is the lesser evil.
  if(steps>=MAX_PHYSICS_STEPS)physicsAccumulator=0;
  player.speed=axes.magnitude>0.01?speed*axes.magnitude:0;
  player.energy=clamp(player.energy+(axes.magnitude>0.01?-(sprint?5.2:2.1):4.5)*dt,0,100);
  discoverDistricts();
}

/**
 * Put the simulated body where the game state says the player is.
 *
 * Used on spawn, on a new world and on load. `updatePlayer` also reconciles
 * automatically if it finds them out of step, so a new caller that moves the
 * player by other means does not have to know this function exists.
 */
function syncPlayerToPhysics(){
  if(!physics||!player)return;
  buildHeightfield(physics,player.x,player.z,true);
  // Stand on the collider, not on the height function. They differ by a
  // fraction of a unit wherever the terrain is steeper than the collider grid,
  // and a character spawned inside the collider cannot move at all.
  const ground=surfaceHeightAt(physics,player.x,player.z);
  if(ground===null)return;
  if(physics.player)teleportPlayer(physics,player.x,ground,player.z);
  else ensurePlayer(physics,player.x,ground,player.z);
  const feet=playerFeet(physics);
  player.x=feet.x;player.z=feet.z;player.y=feet.y;
  physicsAccumulator=0;
}

function updateSimulation(dt){
  world.time=(world.time+dt*0.04)%24;
  weatherTimer-=dt;if(weatherTimer<=0){world.weather=(world.weather+1+Math.floor(rand()*2))%3;weatherTimer=55+rand()*95;showToast(`Weather changed: ${['clear skies','mist','rain'][world.weather]}.`);}
  eventTimer=Math.max(0,eventTimer-dt);
  dispatchInputActions();
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
  syncAgentColliders();
  economyTimer-=dt;if(economyTimer<=0){updateBusinesses(1.0);economyTimer=1;}
  updateMission(dt);
  updateObjective();
  eventMaintenance(dt);
  saveTimer-=dt;if(saveTimer<=0){saveGame();saveTimer=8;}
  if(audioNodes){audioNodes.noiseGain.gain.value=world.weather===2?0.018:world.weather===1?0.010:0.004;}
  // Press and release edges last exactly one frame, so every consumer has
  // read them before they are cleared.
  endFrame(input);
  // The test surface is assembled at the end of this file, so the input
  // state is attached to it on the first frame rather than being wired into
  // a literal that has not been evaluated yet.
  if(globalThis.EMERGENT&&!globalThis.EMERGENT.input)globalThis.EMERGENT.input=input;
}

function saveGame(){
  if(!world||!player)return;
  const payload={version:3,seed,time:world.time,weather:world.weather,economy:world.economy,heat:world.heat,discovered:world.discovered,events:world.events.slice(-30),player:{x:player.x,z:player.z,money:player.money,energy:player.energy,discoveries:player.discoveries,missionsCompleted:player.missionsCompleted,rank:player.rank},mission:currentMission,bindings:serialiseBindings(input),sensitivity:input.sensitivity,invertY:input.invertY,businesses:world.businesses.map(b=>({id:b.id,stock:b.stock,price:b.price,open:b.open,customers:b.customers,revenue:b.revenue,reputation:b.reputation})),npcs:world.npcs.map(n=>({id:n.id,x:n.x,z:n.z,energy:n.energy,money:n.money,mood:n.mood,state:n.state,activity:n.activity,memory:n.memory.slice(-6)}))};
  try{localStorage.setItem(SAVE_KEY,JSON.stringify(payload));$('saveState').textContent='Saved';}catch(e){$('saveState').textContent='Save unavailable';}
}

function loadGame(){
  try{
    const s=JSON.parse(localStorage.getItem(SAVE_KEY)||'null');if(!s||s.seed!==seed)return false;
    world.time=s.time??world.time;world.weather=s.weather??world.weather;world.economy=s.economy??world.economy;world.heat=s.heat??0;world.discovered=s.discovered||{};world.events=s.events||world.events;
    if(s.player)Object.assign(player,s.player);
    if(s.businesses) for(const saved of s.businesses){const b=world.businesses.find(x=>x.id===saved.id);if(b)Object.assign(b,saved);}
    if(s.npcs) for(const saved of s.npcs){const n=world.npcs.find(x=>x.id===saved.id);if(n)Object.assign(n,saved);}
    currentMission=s.mission||null;
    if(s.bindings)deserialiseBindings(input,s.bindings);
    if(Number.isFinite(s.sensitivity))input.sensitivity=s.sensitivity;
    if(typeof s.invertY==='boolean')input.invertY=s.invertY;
    showToast('Saved world restored.');return true;
  }catch(e){return false;}
}

function newWorld(nextSeed){
  seed=(nextSeed===undefined?(Date.now()>>>0):nextSeed)>>>0;rand=rng(seed);world=generateWorld(seed);
  player={x:4800,z:4800,y:terrainHeight(4800,4800,seed),money:120,energy:100,discoveries:0,speed:0,missionsCompleted:0,rank:1,grounded:true};
  currentMission=null;missionTimer=2;streamKey='';renderAgentState.clear();streamResidency.active.clear();streamResidency.chunks.clear();streamResidency.generated=0;streamResidency.evicted=0;worldIndex.buildings.clear();colliderCache.clear();physicsAccumulator=0;
  buildWorldIndex(); buildStaticScene(); syncPlayerToPhysics(); buildDynamicScene(true);
  saveGame();
  showToast(`New world generated from seed ${seed}.`);
}

function startGame(load=true){
  // The material upload is started once and survives world regeneration, since
  // the textures do not depend on the seed.
  if (!materialUpload) startMaterialUpload();
  const stored=load?(()=>{try{return JSON.parse(localStorage.getItem(SAVE_KEY)||'null')}catch(e){return null}})():null;
  const chosen=load ? (stored?.seed??WORLD_SEED_DEFAULT) : (Number.isFinite(BENCH_SEED)&&BENCH_SEED>0 ? BENCH_SEED : WORLD_SEED_DEFAULT); seed=chosen>>>0;rand=rng(seed);world=generateWorld(seed);
  player={x:4800,z:4800,y:terrainHeight(4800,4800,seed),money:120,energy:100,discoveries:0,speed:0,missionsCompleted:0,rank:1,grounded:true};
  buildWorldIndex();if(load) loadGame();buildWorldIndex();streamResidency.active.clear();streamResidency.chunks.clear();colliderCache.clear();physicsAccumulator=0;buildStaticScene();syncPlayerToPhysics();buildDynamicScene(true);
  gameRunning=true;last=performance.now();requestAnimationFrame(frame);showToast('Explore, find businesses and complete the delivery route.');
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
  if(debugVisible){$('debug').innerHTML=`<b>DEVELOPER TELEMETRY</b><br>FPS ${fps.toFixed(1)} · render ${renderMs.toFixed(2)}ms<br>static ${Math.round(staticVertexCount).toLocaleString()}v · dynamic ${Math.round(dynamicVertexCount).toLocaleString()}v · shadow ${Math.round(shadowVertexCount).toLocaleString()}v<br>cells ${visibleChunks}/${staticChunks.length} drawn · ${culledChunks} culled · ${Math.round(submittedStaticVertices).toLocaleString()}v submitted<br>visible ${visibleObjects} · NPC rendered ${simulatedNpcCount}/${world.npcs.length}<br>recompute ${recomputed} · reuse ${reused}<br>stream ops ${streamOps} · resident ${streamResidency.active.size} chunks · created ${streamGenerated} · evicted ${streamFreed} · last ${streamResidency.lastBuildMs.toFixed(2)}ms<br>world ${world.buildings.length} buildings · ${world.trees.length} trees · ${world.businesses.length} businesses`;}
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

canvas.addEventListener('click',()=>{initializeAudio();canvas.requestPointerLock?.();});
addEventListener('pointerlockchange',()=>{pointerLocked=document.pointerLockElement===canvas;});
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
      staticChunks: staticChunks.length, visibleChunks, culledChunks, submittedStaticVertices,
      visibleChunkKeys: visibleChunkKeys.slice(),
      recomputed, reused, visibleObjects, simulatedNpcCount, streamOps,
      streamGenerated, streamFreed, residentChunks: streamResidency.active.size,
      staticBuildMs, dynamicBuildMs, staticBytes, materialLayers: MATERIALS.count,
      lastViewProjection: lastPV
    };
  },
  /** Quality level index, so tests can assert streaming radii directly. */
  get streamRadius() {
    return qualityLevel === 0 ? 680 : qualityLevel === 1 ? 800 : qualityLevel === 2 ? 980 : 1160;
  },
  /**
   * The live input state, including its binding table.
   *
   * Exposed because "the controls are data, not hard-coded key checks" is only
   * checkable from outside if a test can read a binding and write a new one.
   * Rebinding through this object is the real code path a controls screen uses.
   */
  get input() { return input; },
  /**
   * The live material table.
   *
   * Exposed so a test can check that the material indices the geometry actually
   * carries resolve to real materials. A vertex whose index is out of range
   * samples the wrong texture layer, which reads as a wall wearing the road's
   * asphalt and is otherwise invisible.
   */
  get materials() { return { count: MATERIALS.count, byId: MATERIALS.byId, size: MATERIALS.size, layers: MATERIALS.layers }; },
  /** Whether the baked textures finished uploading. */
  get materialState() { return { ready: materialState.ready, loaded: materialState.loaded, total: materialState.total, failed: materialState.failed.slice() }; },
  /** Camera state, so mouse look can be asserted without reading the matrix. */
  get view() { return { yaw, pitch, pointerLocked, firstPerson }; },
  /** Move the player instantly to a world position (tests / manual QA). */
  teleport(x, z) {
    player.x = x; player.z = z;
    player.y = terrainHeight(x, z, seed) + 0.55;
    streamKey = '';
    return { x: player.x, y: player.y, z: player.z };
  },
  /**
   * Aim the camera (tests / manual QA).
   *
   * Exposed because "does culling follow the camera" is not observable any
   * other way from outside the module: without this a test can only assert
   * that *some* cells were culled, which a culler that ignores its input
   * entirely would also satisfy.
   */
  look(newYaw, newPitch) {
    yaw = newYaw;
    if (newPitch !== undefined) pitch = newPitch;
    return { yaw, pitch };
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
