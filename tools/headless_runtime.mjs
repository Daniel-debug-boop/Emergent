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
  SRC_ALPHA: 0x0302, ONE_MINUS_SRC_ALPHA: 0x0303,

  // -- textures ------------------------------------------------------------
  // Added with the material pipeline. Texture support is not a convenience
  // here: without it the harness could not tell the difference between a
  // renderer that uploads a 26-layer, mipmapped, sRGB-correct texture array and
  // one that binds a 1x1 placeholder and ships.
  TEXTURE_2D: 0x0de1, TEXTURE_2D_ARRAY: 0x8c1a, TEXTURE_3D: 0x806f,
  TEXTURE0: 0x84c0, TEXTURE1: 0x84c1, TEXTURE2: 0x84c2, TEXTURE3: 0x84c3,
  TEXTURE4: 0x84c4, TEXTURE5: 0x84c5, TEXTURE6: 0x84c6, TEXTURE7: 0x84c7,
  TEXTURE_MAG_FILTER: 0x2800, TEXTURE_MIN_FILTER: 0x2801,
  TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803, TEXTURE_WRAP_R: 0x8072,
  TEXTURE_BASE_LEVEL: 0x813c, TEXTURE_MAX_LEVEL: 0x813d,
  NEAREST: 0x2600, LINEAR: 0x2601,
  NEAREST_MIPMAP_NEAREST: 0x2700, LINEAR_MIPMAP_NEAREST: 0x2701,
  NEAREST_MIPMAP_LINEAR: 0x2702, LINEAR_MIPMAP_LINEAR: 0x2703,
  REPEAT: 0x2901, CLAMP_TO_EDGE: 0x812f, MIRRORED_REPEAT: 0x8370,
  RGBA: 0x1908, RGB: 0x1907, RED: 0x1903, RGBA8: 0x8058, RGB8: 0x8051,
  SRGB8_ALPHA8: 0x8c43, R8: 0x8229,
  UNSIGNED_BYTE: 0x1401, UNSIGNED_SHORT: 0x1403, FLOAT: 0x1406,
  UNPACK_FLIP_Y_WEBGL: 0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241,
  UNPACK_ALIGNMENT: 0x0cf5, UNPACK_COLORSPACE_CONVERSION_WEBGL: 0x9243,
  NONE: 0, BROWSER_DEFAULT_WEBGL: 0x9244
};

/**
 * Internal format to the (format, type) pair that is legal with it.
 *
 * This is the table that catches the mistake people actually make: declaring
 * an sRGB array and then uploading it as UNSIGNED_SHORT, or handing a
 * float array to texImage2D with UNSIGNED_BYTE. A real driver reports that as
 * INVALID_OPERATION, and so does this.
 */
const INTERNAL_FORMAT_RULES = {
  [GL_ENUMS.RGBA8]: { formats: [GL_ENUMS.RGBA, GL_ENUMS.RGB], types: [GL_ENUMS.UNSIGNED_BYTE] },
  [GL_ENUMS.SRGB8_ALPHA8]: { formats: [GL_ENUMS.RGBA], types: [GL_ENUMS.UNSIGNED_BYTE] },
  [GL_ENUMS.R8]: { formats: [GL_ENUMS.RED], types: [GL_ENUMS.UNSIGNED_BYTE] }
};

/** Sampler states that require a mip chain to be complete. */
const MIPMAP_FILTERS = new Set([
  GL_ENUMS.NEAREST_MIPMAP_NEAREST, GL_ENUMS.LINEAR_MIPMAP_NEAREST,
  GL_ENUMS.NEAREST_MIPMAP_LINEAR, GL_ENUMS.LINEAR_MIPMAP_LINEAR
]);

const MIP_FILTER_NAMES = Object.fromEntries(
  [...MIPMAP_FILTERS].map(v => [v, Object.keys(GL_ENUMS).find(k => GL_ENUMS[k] === v)])
);

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
    // Kept so a test can inspect every VAO's attribute state, not just the
    // currently bound one. The renderer rebinds constantly, so "whatever is
    // bound right now" is not enough to diagnose a draw.
    this._vaos = [];
    this._buffers = [];
    // Named `viewportState`, not `viewport`: an own field would shadow the
    // WebGL viewport() method on the prototype and the renderer would break.
    this.viewportState = { x: 0, y: 0, width: 0, height: 0 };
    this.clearColorState = [0, 0, 0, 0];
    this._enabled = new Set();
    this.unmodelled = new Set();
    this.unmodelledArgs = new Map();

    // -- texture state -----------------------------------------------------
    this._textures = [];
    this._textureByUnit = new Map();     // unit -> texture currently bound
    this._textureTargetByUnit = new Map();
    this.activeTextureUnit = 0;
    this.pixelStore = new Map();
    this.stats.texturesCreated = 0;
    this.stats.textureUploads = 0;
    this.stats.mipmapsGenerated = 0;
    this.stats.textureBytes = 0;
    this.stats.imageDecodes = 0;
  }

  // -- textures ------------------------------------------------------------

  _newTexture(target) {
    const tex = {
      id: this._nextHandle++,
      target,
      width: 0, height: 0, depth: 0,
      internalFormat: null,
      levels: 0,
      mipsGenerated: false,
      // Per-level upload record: which mip levels actually received pixels.
      uploaded: new Set(),
      params: { [GL_ENUMS.TEXTURE_MIN_FILTER]: GL_ENUMS.NEAREST, [GL_ENUMS.TEXTURE_MAG_FILTER]: GL_ENUMS.LINEAR },
      deleted: false
    };
    this._textures.push(tex);
    this.stats.texturesCreated++;
    return tex;
  }

  createTexture() { return this._newTexture('empty'); }

  activeTexture(unit) {
    if (unit < GL_ENUMS.TEXTURE0 || unit > GL_ENUMS.TEXTURE0 + 31) {
      this._error(`activeTexture: unit ${unit} is out of range`);
    }
    this.activeTextureUnit = unit - GL_ENUMS.TEXTURE0;
  }

  bindTexture(target, texture) {
    if (texture && texture.deleted) { this._error('bindTexture on a deleted texture'); return; }
    if (texture) {
      // A texture object is bound to exactly one target for its lifetime.
      // Silently re-binding it to another target is a real driver error and is
      // the usual reason a sampler "works" on one pass and not the next.
      if (texture.target === 'empty') texture.target = target;
      else if (texture.target !== target) {
        this._error(`texture ${texture.id} is bound to ${target} but was created for ${texture.target}`);
      }
    }
    this._textureByUnit.set(this.activeTextureUnit, texture || null);
    this._textureTargetByUnit.set(this.activeTextureUnit, target);
  }

  _unitTexture(unit) { return this._textureByUnit.get(unit) || null; }

  texParameteri(target, pname, param) {
    const tex = this._unitTexture(this.activeTextureUnit);
    if (!tex) { this._error(`texParameteri(${pname}) with no texture bound to unit ${this.activeTextureUnit}`); return; }
    tex.params[pname] = param;
  }

  pixelStorei(pname, param) { this.pixelStore.set(pname, param); }

  /**
   * Immutable storage for a 2D array: the WebGL2 path for a texture array.
   *
   * Levels are allocated here and must be filled, because a texture whose level
   * 0 has no data is an incomplete texture and every draw that samples it
   * returns black on a real driver.
   */
  texStorage3D(target, levels, internalFormat, width, height, depth) {
    const tex = this._unitTexture(this.activeTextureUnit);
    if (!tex) { this._error('texStorage3D with no texture bound'); return; }
    if (target !== GL_ENUMS.TEXTURE_2D_ARRAY) {
      this._error(`texStorage3D called with target ${target}, expected TEXTURE_2D_ARRAY`);
    }
    if (tex.width || tex.levels) { this._error('texStorage3D on a texture that already has storage'); return; }
    const rule = INTERNAL_FORMAT_RULES[internalFormat];
    if (!rule) this._error(`texStorage3D: internal format ${internalFormat} is not modelled`);
    tex.width = width; tex.height = height; tex.depth = depth;
    tex.internalFormat = internalFormat;
    tex.levels = levels;
    tex.mipsGenerated = false;
  }

  texStorage2D(target, levels, internalFormat, width, height) {
    const tex = this._unitTexture(this.activeTextureUnit);
    if (!tex) { this._error('texStorage2D with no texture bound'); return; }
    tex.width = width; tex.height = height; tex.depth = 1;
    tex.internalFormat = internalFormat;
    tex.levels = levels;
    tex.mipsGenerated = false;
  }

  /**
   * Upload one layer of an array texture, at one mip level.
   *
   * The layer index and the level are both checked against the allocated
   * storage, which is what catches an off-by-one in a loop that fills a
   * material array — the failure that otherwise shows up as one missing
   * material in a world full of correctly textured ones.
   */
  texSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels) {
    const tex = this._unitTexture(this.activeTextureUnit);
    this.stats.textureUploads++;
    if (!tex) { this._error('texSubImage3D with no texture bound'); return; }
    if (!tex.levels) { this._error('texSubImage3D before texStorage3D'); return; }
    if (level < 0 || level >= tex.levels) {
      this._error(`texSubImage3D level ${level} outside the ${tex.levels} allocated`);
    }
    if (zoffset < 0 || zoffset + depth > tex.depth) {
      this._error(`texSubImage3D layer ${zoffset}..${zoffset + depth} outside the ${tex.depth} allocated`);
    }
    if (xoffset < 0 || yoffset < 0 || xoffset + width > tex.width || yoffset + height > tex.height) {
      this._error(`texSubImage3D region ${width}x${height}+${xoffset},${yoffset} outside ${tex.width}x${tex.height}`);
    }
    const rule = INTERNAL_FORMAT_RULES[tex.internalFormat];
    if (rule && !rule.formats.includes(format)) {
      this._error(`texSubImage3D format ${format} is not valid for internal format ${tex.internalFormat}`);
    }
    if (rule && !rule.types.includes(type)) {
      this._error(`texSubImage3D type ${type} is not valid for internal format ${tex.internalFormat}`);
    }
    if (pixels && pixels.length !== width * height * depth * 4) {
      this._error(`texSubImage3D supplied ${pixels.length} bytes, expected ${width * height * depth * 4}`);
    }
    if (level === 0) tex.uploaded.add(zoffset);
    this.stats.textureBytes += width * height * depth * 4;
  }

  /** The 2D path, used for anything that is not a material array. */
  texImage2D(target, level, internalFormat, width, height, border, format, type, pixels) {
    const tex = this._unitTexture(this.activeTextureUnit);
    this.stats.textureUploads++;
    if (!tex) { this._error('texImage2D with no texture bound'); return; }
    if (target === GL_ENUMS.TEXTURE_2D_ARRAY) {
      this._error('texImage2D used to fill a TEXTURE_2D_ARRAY; texSubImage3D is the array path');
      return;
    }
    if (pixels && pixels.length !== width * height * 4) {
      this._error(`texImage2D supplied ${pixels.length} bytes, expected ${width * height * 4}`);
    }
    tex.width = width; tex.height = height; tex.depth = 1;
    tex.internalFormat = internalFormat;
    tex.levels = Math.max(tex.levels, level + 1);
    tex.uploaded.add(0);
    tex.mipsGenerated = false;
    this.stats.textureBytes += width * height * 4;
  }

  generateMipmap(target) {
    const tex = this._unitTexture(this.activeTextureUnit);
    if (!tex) { this._error('generateMipmap with no texture bound'); return; }
    if (!tex.width) { this._error('generateMipmap on a texture with no data'); return; }
    tex.mipsGenerated = true;
    this.stats.mipmapsGenerated++;
  }

  deleteTexture(texture) { if (texture) texture.deleted = true; }

  /**
   * Verify every texture is samplable, the way a driver would at draw time.
   *
   * A texture with a mipmap minification filter and no mip chain is
   * incomplete: it samples as solid black. That is invisible in a screenshot
   * taken at one distance and is the reason a scene can look fine up close and
   * disappear at range, so it is checked explicitly.
   */
  auditTextures() {
    const problems = [];
    for (const tex of this._textures) {
      if (tex.deleted || !tex.levels) continue;
      const min = tex.params[GL_ENUMS.TEXTURE_MIN_FILTER];
      const needsMips = MIPMAP_FILTERS.has(min);
      const hasMips = tex.mipsGenerated || tex.levels > 1;
      if (needsMips && !hasMips) {
        problems.push(`texture ${tex.id} uses ${MIP_FILTER_NAMES[min]} but has no mip chain, so it samples as black`);
      }
      if (tex.depth > 1) {
        const missing = [];
        for (let l = 0; l < tex.depth; l++) if (!tex.uploaded.has(l)) missing.push(l);
        if (missing.length) {
          problems.push(`texture ${tex.id} is a ${tex.depth}-layer array with ${missing.length} empty layer(s): ${missing.slice(0, 6).join(',')}`);
        }
      }
    }
    return problems;
  }

  /** A summary the test suite can assert on without reaching into internals. */
  textureReport() {
    return this._textures.filter(t => !t.deleted).map(t => ({
      id: t.id, target: t.target, width: t.width, height: t.height, layers: t.depth,
      internalFormat: t.internalFormat, levels: t.levels, mips: t.mipsGenerated,
      minFilter: t.params[GL_ENUMS.TEXTURE_MIN_FILTER]
    }));
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
  createBuffer() { const b = { id: this._nextHandle++, data: null, byteLength: 0, hasNaN: false, usage: 0 }; this._buffers.push(b); return b; }
  createVertexArray() { const v = this._makeVao(); this._vaos.push(v); return v; }
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
  /**
   * A vec4 array uniform. Validated like every other uniform, because the
   * material table is uploaded through this and a bad entry index there is a
   * silently black surface rather than an error.
   */
  uniform4fv(loc, value) {
    if (!loc) return;
    if (!value) { this._error(`uniform4fv(${loc.name}) called with no data`); return; }
    for (let i = 0; i < value.length; i++) {
      if (!Number.isFinite(value[i])) {
        this._error(`uniform4fv(${loc.name}) contains non-finite values at index ${i}`);
        return;
      }
    }
    loc.value = value;
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
    translate: record('translate'), rotate: record('rotate'), scale: record('scale'),
    // Image decoding. `drawImage` records the call and `getImageData` returns a
    // deterministic buffer of exactly the right size, so the renderer's whole
    // upload path — canvas sizing, ImageData, texSubImage3D — runs for real
    // while the harness stays free of a PNG decoder. What the pixels *are* is
    // not the harness's business; that they are the right number of them, and
    // that they reached the right texture layer, is.
    drawImage: record('drawImage'),
    getImageData(x, y, w, h) {
      ops.count++;
      return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4).fill(128) };
    },
    putImageData: record('putImageData'),
    createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
    measureText(t) { return { width: String(t).length * 6 }; },
    setTransform: record('setTransform'), transform: record('transform')
  };
}

/**
 * A stand-in for `HTMLImageElement`.
 *
 * Reports the size the renderer's material descriptor declares, which is the
 * number the renderer is about to assert against. If a baked tile is ever the
 * wrong size, the descriptor is what the game reads and the game is what fails,
 * so the harness only has to answer the question that is actually being asked.
 */
function makeImage(width = 512, height = 512) {
  return {
    naturalWidth: width,
    naturalHeight: height,
    width, height,
    complete: true,
    _src: '',
    get src() { return this._src; },
    set src(v) { this._src = String(v); },
    decode() { return Promise.resolve(this); },
    addEventListener() {},
    removeEventListener() {}
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
    requestPointerLock() { if (this._owner) { this._owner.pointerLockElement = this; this._owner.dispatch('pointerlockchange'); } },
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

/** The size a decoded image reports. Defaults to the baked texture size. */
let imageSize = { width: 512, height: 512 };

/** Make `new Image()` report a different decoded size, to test a mismatch. */
export function setImageSize(width, height = width) {
  imageSize = { width, height };
}
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
  const created = [];
  const document = {
    body: makeElement('body'),
    pointerLockElement: null,
    exitPointerLock() { this.pointerLockElement = null; this.dispatch('pointerlockchange'); },
    getElementById(id) {
      if (!elements.has(id)) {
        const el = makeElement('div');
        el.id = id;
        el._owner = document;
        elements.set(id, el);
      }
      return elements.get(id);
    },
    createElement(tag) {
      const el = makeElement(tag);
      el._owner = document;
      created.push(el);
      if (el.id) elements.set(el.id, el);
      return el;
    },
    /** First created element matching a tag name or `#id`. */
    querySelector(selector) {
      const id = selector.startsWith('#') ? selector.slice(1) : null;
      return created.find(el => (id ? el.id === id : el.tagName === selector.toUpperCase())) || null;
    },
    addEventListener(type, fn) { (globalListeners[type] ||= []).push(fn); },
    removeEventListener() {},
    /**
     * Fire a window-level event.
     *
     * Pointer lock is the reason this exists: in a browser the request is
     * answered asynchronously by the user agent and the page only learns about
     * it through this event. Modelling the request as a no-op left
     * `document.pointerLockElement` permanently null, so mouse look — the one
     * control that has no fallback binding at all — could not be exercised.
     */
    dispatch(type, event = {}) { fireGlobal(type, { target: document, ...event }); }
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
  // Images report the size the material descriptor declares, so the renderer's
  // upload path runs for real. `setImageSize` lets a test make the decoded size
  // disagree with the descriptor and assert that the renderer notices.
  globalThis.Image = class { constructor() { return makeImage(imageSize.width, imageSize.height); } };
  return { document, location };
}

/**
 * Dispatch a synthetic window-level event to the game (e.g. keydown).
 *
 * Modelled on the parts of `Event` a listener is entitled to call. A plain
 * object was enough until the game started using `stopImmediatePropagation` to
 * own keyboard input, at which point the harness threw a TypeError instead of
 * telling anyone the real thing was missing — the failure mode this harness
 * exists to prevent, in the harness itself.
 */
export function fireGlobal(type, event = {}) {
  // A real dispatch stops handing the event to later listeners once one calls
  // `stopImmediatePropagation`. Without that, two handlers for one key press
  // both run here while only one of them would run in a browser — which is
  // exactly the class of bug the harness is supposed to catch.
  const state = { stopped: false };
  const evt = {
    preventDefault() {},
    stopPropagation() { state.stopped = true; },
    stopImmediatePropagation() { state.stopped = true; },
    target: globalThis,
    type,
    repeat: false,
    ...event
  };
  for (const fn of globalListeners[type] || []) {
    fn(evt);
    if (state.stopped) break;
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
  imageSize = { width: 512, height: 512 };
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
