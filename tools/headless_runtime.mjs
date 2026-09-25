/**
 * EMERGENT headless runtime harness.
 *
 * Provides enough WebGL2 + DOM surface for `game3d.js` to boot and run its real
 * frame loop inside Node, with semantic validation the browser does silently:
 *
 *  - `drawArrays` verifies every enabled vertex attribute stays inside its
 *    bound buffer (WebGL drops such draws with INVALID_OPERATION; here it is a
 *    recorded, testable error).
 *  - `bufferData` scans uploads for non-finite floats so NaN geometry is
 *    caught at upload time instead of rendering as invisible primitives.
 *  - uniform calls on a null location (uniform optimized out of the shader)
 *    are recorded as errors instead of being silently ignored.
 *
 * Used two ways:
 *  1. Imported by `test_headless_game.mjs` for interactive scenario testing.
 *  2. Run directly as a CLI to drive the in-page benchmark (`--mode=standard`)
 *     and emit the measured result as JSON on stdout.
 *
 * The harness never fabricates GPU timings: it reports CPU-side wall time of
 * the simulated frames only, exactly like the in-browser benchmark.
 */

// ---------------------------------------------------------------------------
// WebGL2 stub
// ---------------------------------------------------------------------------

const GL_ENUMS = {
  VERTEX_SHADER: 0x8b31, FRAGMENT_SHADER: 0x8b30, COMPILE_STATUS: 0x8b81,
  LINK_STATUS: 0x8b82, ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893,
  STATIC_DRAW: 0x88e4, DYNAMIC_DRAW: 0x88e8, STREAM_DRAW: 0x88e0,
  FLOAT: 0x1406, TRIANGLES: 0x0004, TRIANGLE_STRIP: 0x0005, POINTS: 0x0000,
  DEPTH_TEST: 0x0b71, LEQUAL: 0x0203, CULL_FACE: 0x0b44, BACK: 0x0405,
  COLOR_BUFFER_BIT: 0x4000, DEPTH_BUFFER_BIT: 0x0100, BLEND: 0x0be2,
  SRC_ALPHA: 0x0302, ONE_MINUS_SRC_ALPHA: 0x0303
};

class HeadlessGL {
  constructor(canvas) {
    this.canvas = canvas;
    for (const [name, value] of Object.entries(GL_ENUMS)) this[name] = value;
    this.stats = {
      drawCalls: 0, drawnVertices: 0, bufferUploads: 0, nanUploads: 0,
      shaderCompiles: 0, programLinks: 0, errors: []
    };
    this._nextHandle = 1;
    this._defaultVao = this._makeVao();
    this.boundVertexArray = this._defaultVao;
    this.boundBuffer = null;
    this.currentProgram = null;
    this._programs = new Set();
    // Named `viewportState`, not `viewport`: an own field would shadow the
    // WebGL viewport() method on the prototype and the renderer would break.
    this.viewportState = { x: 0, y: 0, width: 0, height: 0 };
    this.clearColorState = [0, 0, 0, 0];
    this._enabled = new Set();
    this.unmodelled = new Set();
    this.unmodelledArgs = new Map();
  }

  _makeVao() {
    return { attributes: new Map(), elementArrayBuffer: null };
  }

  /**
   * Record GL entry points the harness does not model.
   *
   * A silent no-op would let a test report "WebGL clean" while the engine was
   * actually issuing calls nobody verified. Instead the name is captured in
   * `unmodelled` and the test suite asserts the list stays empty, so the
   * harness is forced to keep pace with the renderer.
   */
  static instrument(gl) {
    return new Proxy(gl, {
      get(target, prop, receiver) {
        if (typeof prop !== 'string' || prop in target) return Reflect.get(target, prop, receiver);
        if (typeof target[prop] === 'undefined' && !(prop in target)) {
          target.unmodelled.add(prop);
          return (...args) => { target.unmodelledArgs.set(prop, args.length); };
        }
        return Reflect.get(target, prop, receiver);
      }
    });
  }

  _error(msg) {
    this.stats.errors.push(msg);
    if (this.stats.errors.length > 100) this.stats.errors.shift();
  }

  // -- object creation ----------------------------------------------------
  createBuffer() { return { id: this._nextHandle++, data: null, byteLength: 0, hasNaN: false, usage: 0 }; }
  createVertexArray() { return this._makeVao(); }
  createShader(type) { return { id: this._nextHandle++, type, source: '', compileOk: true, problems: [] }; }
  createProgram() {
    const program = { id: this._nextHandle++, shaders: [], uniforms: new Map(), linked: false };
    this._programs.add(program);
    return program;
  }
  getUniformLocation(program, name) {
    if (!program || !this._programs.has(program)) { this._error('getUniformLocation on unknown program'); return null; }
    const loc = { program, name };
    program.uniforms.set(name, { location: loc, value: null });
    return loc;
  }

  // -- shader/program assembly ---------------------------------------------
  shaderSource(shader, source) { shader.source = String(source); }
  compileShader(shader) {
    this.stats.shaderCompiles++;
    // Per-shader status, not a global error count: the engine reads
    // COMPILE_STATUS immediately after compiling, and errors recorded later in
    // the run must not retroactively invalidate an earlier successful compile.
    shader.problems = [];
    if (!/#version\s+300\s+es/.test(shader.source)) shader.problems.push('missing #version 300 es');
    if (/\b(attribute|varying)\b/.test(shader.source)) shader.problems.push('uses GLSL ES 1.00 attribute/varying syntax');
    if (!/\bvoid\s+main\s*\(/.test(shader.source)) shader.problems.push('has no void main() entry point');
    shader.compileOk = shader.problems.length === 0;
    for (const p of shader.problems) this._error(`shader ${shader.id}: ${p}`);
  }
  getShaderParameter(shader, pname) {
    return pname === GL_ENUMS.COMPILE_STATUS ? shader.compileOk : true;
  }
  getShaderInfoLog(shader) { return (shader.problems || []).join('; '); }
  attachShader(program, shader) { program.shaders.push(shader); }
  linkProgram(program) {
    this.stats.programLinks++;
    const vs = program.shaders.filter(s => s.type === GL_ENUMS.VERTEX_SHADER);
    const fs = program.shaders.filter(s => s.type === GL_ENUMS.FRAGMENT_SHADER);
    program.problems = [];
    if (vs.length !== 1) program.problems.push(`expected 1 vertex shader, found ${vs.length}`);
    if (fs.length !== 1) program.problems.push(`expected 1 fragment shader, found ${fs.length}`);
    for (const s of [...vs, ...fs]) {
      if (!s.compileOk) program.problems.push(`attached shader ${s.id} did not compile`);
    }
    program.linked = program.problems.length === 0;
    for (const p of program.problems) this._error(`program ${program.id}: ${p}`);
  }
  getProgramParameter(program, pname) {
    return pname === GL_ENUMS.LINK_STATUS ? program.linked : true;
  }
  getProgramInfoLog(program) { return (program.problems || []).join('; '); }

  // -- state ----------------------------------------------------------------
  useProgram(program) {
    if (program && !this._programs.has(program)) this._error('useProgram on unknown program');
    this.currentProgram = program;
  }
  bindVertexArray(vao) { this.boundVertexArray = vao || this._defaultVao; }
  bindBuffer(target, buffer) {
    if (target === GL_ENUMS.ARRAY_BUFFER) this.boundBuffer = buffer;
    if (target === GL_ENUMS.ELEMENT_ARRAY_BUFFER && this.boundVertexArray) {
      this.boundVertexArray.elementArrayBuffer = buffer;
    }
  }
  enable(cap) { this._enabled.add(cap); }
  disable(cap) { this._enabled.delete(cap); }
  depthFunc() {}
  cullFace() {}
  blendFunc() {}
  depthMask() {}
  clearDepth(d) { this.clearDepthValue = d; }
  lineWidth(w) { this.lineWidthValue = w; }
  colorMask(r, g, b, a) { this.colorMaskValue = [r, g, b, a]; }
  viewport(x, y, w, h) { this.viewportState = { x, y, width: w, height: h }; }
  clearColor(r, g, b, a) { this.clearColorState = [r, g, b, a]; }
  clear() {}
  flush() {}
  finish() {}
  getParameter() { return 0; }
  getExtension(name) { return null; }
  deleteBuffer(b) { if (b) b.data = null; }
  deleteProgram(p) { this._programs.delete(p); }
  deleteShader() {}
  getError() { return this.stats.errors.length ? 1 : 0; }

  // -- buffers ---------------------------------------------------------------
  bufferData(target, srcData, usage) {
    const buffer = this.boundBuffer;
    if (!buffer) { this._error('bufferData with no ARRAY_BUFFER bound'); return; }
    buffer.usage = usage;
    if (srcData && typeof srcData.length === 'number') {
      buffer.data = srcData.slice(0);
      buffer.byteLength = buffer.data.byteLength;
      this.stats.bufferUploads++;
      let nan = false;
      for (let i = 0; i < buffer.data.length; i++) {
        if (!Number.isFinite(buffer.data[i])) { nan = true; break; }
      }
      if (nan) {
        buffer.hasNaN = true;
        this.stats.nanUploads++;
        this._error(`bufferData upload #${this.stats.bufferUploads} contains non-finite floats (${buffer.data.length} values)`);
      } else {
        buffer.hasNaN = false;
      }
    }
  }
  enableVertexAttribArray(index) {
    const attr = this.boundVertexArray.attributes.get(index) || {};
    attr.enabled = true;
    this.boundVertexArray.attributes.set(index, attr);
  }
  disableVertexAttribArray(index) {
    const attr = this.boundVertexArray.attributes.get(index);
    if (attr) attr.enabled = false;
  }
  vertexAttribPointer(index, size, type, normalized, stride, offset) {
    this.boundVertexArray.attributes.set(index, {
      enabled: true, size, type, normalized, stride, offset, buffer: this.boundBuffer
    });
  }

  // -- uniforms ---------------------------------------------------------------
  _checkUniform(loc, name) {
    if (!loc) { this._error(`uniform${name}() on null location (uniform optimized out or wrong program)`); return false; }
    if (this.currentProgram && loc.program !== this.currentProgram) {
      this._error(`uniform${name}() used while a different program is bound`);
      return false;
    }
    return true;
  }
  uniformMatrix4fv(loc, transpose, value) {
    if (!this._checkUniform(loc, 'Matrix4fv')) return;
    const v = Float32Array.from(value);
    for (let i = 0; i < v.length; i++) {
      if (!Number.isFinite(v[i])) { this._error(`uniformMatrix4fv(${loc.name}) contains non-finite values`); return; }
    }
    loc.program.uniforms.get(loc.name).value = v;
  }
  uniform3f(loc, x, y, z) {
    if (!this._checkUniform(loc, '3f')) return;
    loc.program.uniforms.get(loc.name).value = [x, y, z];
  }
  uniform1f(loc, v) {
    if (!this._checkUniform(loc, '1f')) return;
    loc.program.uniforms.get(loc.name).value = v;
  }
  uniform1i(loc, v) {
    if (!this._checkUniform(loc, '1i')) return;
    loc.program.uniforms.get(loc.name).value = v;
  }

  // -- draw ---------------------------------------------------------------------
  drawElements(mode, count, type, offset) {
    // Element-array draws need index storage to be validated; the game only
    // uses them if an index buffer is uploaded, which bufferData records.
    if (!this.boundVertexArray.elementArrayBuffer) {
      this._error('drawElements with no ELEMENT_ARRAY_BUFFER bound');
      return;
    }
    this.drawArrays(mode, 0, count);
  }

  drawArrays(mode, first, count) {
    const vao = this.boundVertexArray;
    if (!(count > 0)) return;
    for (const [index, attr] of vao.attributes) {
      if (!attr.enabled || !attr.buffer) continue;
      // Bytes touched = offset + one vertex, then a stride jump per remaining
      // vertex. With stride 0 the array is tightly packed, so every vertex
      // advances by size*4 bytes instead.
      const componentBytes = attr.size * 4;
      const lastVertex = first + count - 1;
      const needed = attr.stride > 0
        ? attr.offset + componentBytes + attr.stride * lastVertex
        : attr.offset + componentBytes * (first + count);
      const available = attr.buffer.data ? attr.buffer.data.byteLength : 0;
      if (needed > available) {
        this._error(
          `drawArrays: attribute ${index} reads ${needed} bytes but its buffer holds ${available} ` +
          `(first=${first} count=${count} stride=${attr.stride} offset=${attr.offset})`
        );
        return;
      }
      if (index === 0 && attr.buffer.hasNaN) {
        this._error('drawArrays: position buffer contains non-finite data');
        return;
      }
    }
    this.stats.drawCalls++;
    this.stats.drawnVertices += count;
  }
}

// ---------------------------------------------------------------------------
// 2D context stub (mission overlay)
// ---------------------------------------------------------------------------

function make2DContext() {
  const ops = { count: 0 };
  const record = name => () => { ops.count++; };
  return {
    ops,
    fillStyle: '#fff', strokeStyle: '#fff', lineWidth: 1, font: '12px sans-serif',
    globalAlpha: 1, textAlign: 'left',
    clearRect: record('clearRect'), fillRect: record('fillRect'),
    beginPath: record('beginPath'), closePath: record('closePath'),
    moveTo: record('moveTo'), lineTo: record('lineTo'),
    arc: record('arc'), fill: record('fill'), stroke: record('stroke'),
    fillText: record('fillText'), strokeText: record('strokeText'),
    save: record('save'), restore: record('restore'),
    translate: record('translate'), rotate: record('rotate'), scale: record('scale')
  };
}

// ---------------------------------------------------------------------------
// DOM stubs
// ---------------------------------------------------------------------------

function makeClassList() {
  const set = new Set();
  return {
    add: (...names) => names.forEach(n => set.add(n)),
    remove: (...names) => names.forEach(n => set.delete(n)),
    toggle: (name, force) => {
      const on = force === undefined ? !set.has(name) : force;
      if (on) set.add(name); else set.delete(name);
      return on;
    },
    contains: name => set.has(name),
    _set: set
  };
}

function makeElement(tag) {
  const el = {
    tagName: String(tag).toUpperCase(),
    id: '',
    style: {},
    dataset: {},
    children: [],
    textContent: '',
    listeners: {},
    classList: makeClassList(),
    appendChild(child) { this.children.push(child); child.parent = this; return child; },
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    removeEventListener() {},
    setPointerCapture() {},
    releasePointerCapture() {},
    requestPointerLock() {},
    focus() {},
    click() { for (const fn of this.listeners.click || []) fn({ preventDefault() {} }); },
    getContext(type) {
      if (type === '2d') { this._ctx2d ||= make2DContext(); return this._ctx2d; }
      if (type === 'webgl2') { this._gl ||= HeadlessGL.instrument(new HeadlessGL(this)); return this._gl; }
      return null;
    }
  };
  let width = 300, height = 150;
  Object.defineProperty(el, 'width', { get: () => width, set: v => { width = Math.max(0, Math.floor(v)); } });
  Object.defineProperty(el, 'height', { get: () => height, set: v => { height = Math.max(0, Math.floor(v)); } });
  Object.defineProperty(el, 'innerHTML', {
    get: () => el._innerHTML || '',
    set: v => { el._innerHTML = String(v); }
  });
  return el;
}

function makeLocalStorage() {
  const map = new Map();
  return {
    getItem: k => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: k => map.delete(k),
    clear: () => map.clear(),
    key: i => [...map.keys()][i] ?? null,
    get length() { return map.size; }
  };
}

class FakeAudioParam {
  constructor(v = 0) { this.value = v; }
  setValueAtTime() {} exponentialRampToValueAtTime() {} linearRampToValueAtTime() {}
}
class FakeAudioNode {
  constructor() {
    this.gain = new FakeAudioParam(1);
    this.frequency = new FakeAudioParam(440);
    this.detune = new FakeAudioParam(0);
    this.Q = new FakeAudioParam(1);
    this.type = 'sine';
    this.buffer = null;
    this.loop = false;
  }
  connect(node) { return node; }
  disconnect() {} start() {} stop() {}
}
class FakeAudioContext {
  constructor() {
    this.sampleRate = 48000;
    this.currentTime = 0;
    this.state = 'running';
    this.destination = new FakeAudioNode();
  }
  createGain() { return new FakeAudioNode(); }
  createOscillator() { return new FakeAudioNode(); }
  createBiquadFilter() { return new FakeAudioNode(); }
  createBuffer(channels, length) { return { length, channels, getChannelData: () => new Float32Array(length) }; }
  createBufferSource() { return new FakeAudioNode(); }
  resume() { return Promise.resolve(); }
  close() { return Promise.resolve(); }
}

// ---------------------------------------------------------------------------
// Environment installation
// ---------------------------------------------------------------------------

const rafQueue = [];
const globalListeners = {};
let virtualNow = performance.now();

/**
 * Install the headless browser environment onto globalThis.
 * Must be called before importing game3d.js.
 */
export function setupHeadless(options = {}) {
  const {
    url = 'http://localhost:8765/index.html',
    width = 1280,
    height = 720
  } = options;

  const location = new URL(url);
  const elements = new Map();
  const document = {
    body: makeElement('body'),
    pointerLockElement: null,
    exitPointerLock() { this.pointerLockElement = null; },
    getElementById(id) {
      if (!elements.has(id)) {
        const el = makeElement('div');
        el.id = id;
        elements.set(id, el);
      }
      return elements.get(id);
    },
    createElement(tag) {
      const el = makeElement(tag);
      if (el.id) elements.set(el.id, el);
      return el;
    },
    addEventListener(type, fn) { (globalListeners[type] ||= []).push(fn); },
    removeEventListener() {}
  };
  globalThis.window = globalThis;
  globalThis.document = document;
  globalThis.location = location;
  // Node 22 exposes `navigator` as an accessor-only global, so a plain
  // assignment throws. Define an own data property instead.
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    writable: true,
    value: { userAgent: 'EMERGENT-headless-Node', maxTouchPoints: 0, hardwareConcurrency: 4 }
  });
  globalThis.innerWidth = width;
  globalThis.innerHeight = height;
  globalThis.devicePixelRatio = 1;
  globalThis.addEventListener = (type, fn) => { (globalListeners[type] ||= []).push(fn); };
  globalThis.removeEventListener = (type, fn) => {
    globalListeners[type] = (globalListeners[type] || []).filter(f => f !== fn);
  };
  globalThis.requestAnimationFrame = fn => { rafQueue.push(fn); return rafQueue.length; };
  globalThis.cancelAnimationFrame = () => {};
  globalThis.localStorage = makeLocalStorage();
  globalThis.AudioContext = FakeAudioContext;
  return { document, location };
}

/** Dispatch a synthetic window-level event to the game (e.g. keydown). */
export function fireGlobal(type, event = {}) {
  for (const fn of globalListeners[type] || []) {
    fn({ preventDefault() {}, repeat: false, ...event });
  }
}

/**
 * Clear per-run state so a fresh game can boot in the same process.
 * The game module is side-effectful on import, so tests re-import it with a
 * cache-busting query; this resets everything the module reaches for.
 */
export function resetHarness() {
  rafQueue.length = 0;
  for (const key of Object.keys(globalListeners)) delete globalListeners[key];
  virtualNow = performance.now();
}

/**
 * Pump the rAF chain with a fixed frame interval. Throws with the original
 * stack if a frame callback throws — a broken rAF chain (the symptom of an
 * exception inside frame()) fails loudly here instead of silently freezing.
 */
export async function pumpFrames(count, dtMs = 33.333) {
  for (let i = 0; i < count; i++) {
    const cb = rafQueue.shift();
    if (!cb) throw new Error(`rAF chain broke after ${i} frames (frame callback threw or game stopped)`);
    virtualNow += dtMs;
    try {
      cb(virtualNow);
    } catch (err) {
      throw new Error(`frame ${i} threw: ${err.stack || err}`);
    }
    await new Promise(resolve => setImmediate(resolve));
  }
}

/** The most recently created WebGL2 context (the game's renderer). */
export function currentGL() {
  const body = globalThis.document.body;
  for (const child of body.children) {
    if (child._gl) return child._gl;
  }
  return null;
}

/** The 2D context of the mission-marker overlay canvas, if the game made one. */
export function overlayCtxOf() {
  for (const child of globalThis.document.body.children) {
    if (child._ctx2d) return child._ctx2d;
  }
  return null;
}

// ---------------------------------------------------------------------------
// CLI: drive the in-page benchmark exactly as a browser tab would
// ---------------------------------------------------------------------------

function argValue(name, fallback) {
  const hit = process.argv.find(a => a.startsWith(`--${name}=`));
  return hit ? hit.split('=').slice(1).join('=') : fallback;
}

if (process.argv[1] && import.meta.url === new URL(`file://${process.argv[1]}`).href) {
  const mode = argValue('mode', 'standard');
  const seed = argValue('seed', '173927');
  const quality = argValue('quality', 'HIGH');
  const frames = Number(argValue('frames', '300')); // 300 * 33.3ms ~= 10s of simulated time

  setupHeadless({ url: `http://localhost:8765/index.html?benchmark=${mode}&seed=${seed}&quality=${quality}` });
  const t0 = performance.now();
  await import(new URL('../game3d.js', import.meta.url).href);
  await pumpFrames(frames, 33.333);
  const wallMs = performance.now() - t0;

  const benchJSON = globalThis.document.body.dataset.benchmark;
  const gl = currentGL();
  if (!benchJSON) {
    console.error(JSON.stringify({ ok: false, reason: 'benchmark result missing after frame pump', gl: gl && gl.stats }, null, 2));
    process.exit(1);
  }
  const bench = JSON.parse(benchJSON);
  const failures = [];
  if (!(bench.fps > 0)) failures.push('fps not measured');
  if (!(bench.staticVertices > 10000)) failures.push('static geometry suspiciously small');
  if (!(bench.dynamicVertices > 0)) failures.push('no dynamic geometry rendered');
  if (!(bench.streamOps >= 1)) failures.push('no streaming operations recorded');
  if (gl && gl.stats.errors.length) failures.push(`WebGL validation errors: ${gl.stats.errors[0]}`);
  if (gl && gl.stats.nanUploads) failures.push('NaN vertex data was uploaded');

  console.log(JSON.stringify({
    ok: failures.length === 0,
    failures,
    wallMs: +wallMs.toFixed(1),
    mode,
    bench,
    gl: gl ? {
      drawCalls: gl.stats.drawCalls,
      drawnVertices: gl.stats.drawnVertices,
      bufferUploads: gl.stats.bufferUploads,
      nanUploads: gl.stats.nanUploads,
      shaderCompiles: gl.stats.shaderCompiles,
      programLinks: gl.stats.programLinks,
      errorCount: gl.stats.errors.length
    } : null
  }, null, 2));
  process.exit(failures.length ? 1 : 0);
}
