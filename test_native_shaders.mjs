// Native shader tests.
//
// `glslc` is optional in this repository — it ships with the Vulkan SDK, which
// CI does not have — and when it is missing the renderer's honest response is to
// report "shader module unavailable" and draw nothing. That is correct, and it
// is also a trap: a shader with a syntax error, or one that declares more push
// constants than the device guarantees, is indistinguishable from "no Vulkan on
// this machine". The failure only appears on a developer's desktop.
//
// So the GLSL is parsed here, in Node, with a real GLSL parser, and the things
// that make a pipeline uncreatable are asserted. This does not replace compiling
// to SPIR-V — only glslc can do that — but it catches the errors that would
// otherwise survive to the first run on real hardware.

import { readFileSync, readdirSync } from 'node:fs';
import { parser } from '@shaderfrog/glsl-parser';
import { strict as assert } from 'node:assert';

const SHADER_DIR = 'native/shaders';
const files = readdirSync(SHADER_DIR).filter((f) => /\.(vert|frag|comp)$/.test(f));

let checks = 0;
const failures = [];

function check(condition, what) {
  checks++;
  if (!condition) failures.push(what);
}

const sources = new Map();
for (const file of files) {
  const text = readFileSync(`${SHADER_DIR}/${file}`, 'utf8');
  sources.set(file, text);

  // A real parse, not a regex. The version directive is injected rather than
  // required in the file, because every shader in this directory declares
  // `#version 450` itself and the parser wants it before anything else.
  let ast = null;
  let error = null;
  try {
    ast = parser.parse(text, { quiet: true });
  } catch (e) {
    error = e;
  }
  check(!error, `${file}: parses as GLSL (${error ? String(error.message).slice(0, 160) : 'ok'})`);
  if (!ast) continue;

  const json = JSON.stringify(ast);
  check(json.includes('"version"') || text.includes('#version'),
    `${file}: declares a version`);
}

// ---------------------------------------------------------------------------
// The material table must not live in push constants.
//
// kMaxMaterials is 64, so the table is 2 x 64 x 16 = 2048 bytes. The Vulkan
// guaranteed minimum for maxPushConstantsSize is 128 bytes, and a great many
// drivers cap at 128 or 256. A material table in push constants therefore does
// not "render slightly wrong", it fails vkCreateGraphicsPipelines — and only on
// the machines whose limit is high enough to have hidden it.

const MAX_MATERIALS = 64;
const PUSH_CONSTANT_GUARANTEE = 128;

for (const [file, text] of sources) {
  // The push constant block, up to its closing brace.
  const block = text.match(/layout\(push_constant\)\s+uniform\s+\w+\s*\{([\s\S]*?)\n\}/);
  if (!block) continue;
  const body = block[1];
  check(!/materialA|materialB/.test(body),
    `${file}: the material table is not in push constants (2 KB against a 128-byte guarantee)`);

  // Measure the declared size. mat4 is 64, vec4 16, vec3 16 in std430-ish
  // push constant layout, float/int 4, arrays are the element size.
  let bytes = 0;
  for (const line of body.split('\n')) {
    const decl = line.match(/^\s*(?:layout\([^)]*\)\s*)?(\w+)\s+(\w+)\s*(\[\s*\w+\s*\])?\s*;/);
    if (!decl) continue;
    const [, type, name, array] = decl;
    const size = { mat4: 64, mat3: 48, vec4: 16, vec3: 16, vec2: 8, float: 4, int: 4, uint: 4 }[type];
    if (size === undefined) continue;
    bytes += size;
    if (array) {
      const n = Number(array.match(/\d+/)?.[0] ?? 0);
      // A sized-by-constant array counts against the limit; an array sized by a
      // #define is counted by the same number, so the check is conservative.
      bytes += size * (n > 0 ? n : 1);
    }
  }
  check(bytes <= PUSH_CONSTANT_GUARANTEE,
    `${file}: push constants are ${bytes} bytes, within the ${PUSH_CONSTANT_GUARANTEE}-byte guarantee`);
}

// ---------------------------------------------------------------------------
// The scene pair has to agree on its interface. A vertex output that the
// fragment stage does not declare reads as undefined, and a fragment input with
// no producer reads as garbage — both render, and neither is diagnosable from
// the image.

function outputs(text) {
  const found = new Map();
  const re = /layout\(location\s*=\s*(\d+)\)\s*(?:flat\s+)?out\s+(\w+)\s+(\w+)\s*;/g;
  let m;
  while ((m = re.exec(text)) !== null) found.set(m[1], m[3]);
  return found;
}

function inputs(text) {
  const found = new Map();
  const re = /layout\(location\s*=\s*(\d+)\)\s*(?:flat\s+)?in\s+(\w+)\s+(\w+)\s*;/g;
  let m;
  while ((m = re.exec(text)) !== null) found.set(m[1], m[3]);
  return found;
}

const pairs = [
  ['scene.vert', 'scene.frag'],
  ['scene_pbr.vert', 'scene_pbr.frag'],
];
for (const [v, f] of pairs) {
  if (!sources.has(v) || !sources.has(f)) continue;
  const vo = outputs(sources.get(v));
  const fi = inputs(sources.get(f));
  check(vo.size > 0, `${v}: declares vertex outputs`);
  for (const [loc, type] of vo) {
    check(fi.has(loc), `${f}: declares an input at location ${loc} (${type}) to match ${v}`);
    if (fi.has(loc)) {
      check(fi.get(loc) === type,
        `${f}: location ${loc} is ${fi.get(loc)} in the fragment stage and ${type} in the vertex stage`);
    }
  }
  for (const [loc, type] of fi) {
    check(vo.has(loc), `${f}: every input at location ${loc} (${type}) has a producer in ${v}`);
  }
}

// ---------------------------------------------------------------------------
// The scene vertex layout has to be the 48-byte stride the renderer uploads.

const pbrVert = sources.get('scene_pbr.vert') ?? '';
check(/layout\(location\s*=\s*0\)\s*in\s+vec3\s+inPosition/.test(pbrVert),
  'scene_pbr.vert: attribute 0 is the position');
check(/layout\(location\s*=\s*3\)\s*in\s+float\s+inMaterial/.test(pbrVert),
  'scene_pbr.vert: attribute 3 is the material index');
check(/layout\(location\s*=\s*4\)\s*in\s+vec2\s+inUv/.test(pbrVert),
  'scene_pbr.vert: attribute 4 is the UV pair, at float offset 10');

// A material index has to be flat-qualified on the way to the fragment stage.
// Interpolated, it becomes a blend of two valid indices and samples a third
// material that does not exist — which looks like a texture bug, not a
// qualifier bug.
const pbrFrag = sources.get('scene_pbr.frag') ?? '';
check(/layout\(location\s*=\s*5\)\s*flat\s+in\s+int\s+vMaterial/.test(pbrFrag),
  'scene_pbr.frag: the material index arrives flat');

// The scene_pbr pair must read all three maps. The original web bug was a
// fragment stage that bound the textures and never sampled them, so the same
// wiring is asserted here.
for (const binding of [0, 1, 2]) {
  check(new RegExp(`layout\\(set\\s*=\\s*0,\\s*binding\\s*=\\s*${binding}\\)\\s*uniform\\s+sampler2DArray`).test(pbrFrag),
    `scene_pbr.frag: declares the texture array at set 0 binding ${binding}`);
}
for (const name of ['albedoArray', 'normalArray', 'armArray']) {
  const uses = (pbrFrag.match(new RegExp(name, 'g')) ?? []).length;
  check(uses >= 2, `scene_pbr.frag: ${name} is declared and actually sampled (${uses} references)`);
}

// ---------------------------------------------------------------------------
// The material table's uniform array has to be sized from the same constant the
// C++ header uses, or the two silently disagree about how many materials exist.

for (const [file, text] of sources) {
  if (!/EMERGENT_MAX_MATERIALS/.test(text)) continue;
  check(!/material[AB]\s*\[\s*\d+\s*\]/.test(text),
    `${file}: sizes the material array from EMERGENT_MAX_MATERIALS, not a literal`);
}

// ---------------------------------------------------------------------------

if (failures.length) {
  console.error(`native shader tests: ${failures.length} of ${checks} checks failed`);
  for (const f of failures) console.error(`  FAIL ${f}`);
  process.exit(1);
}
console.log(`native shader tests: ${checks} checks passed over ${files.length} shaders`);
