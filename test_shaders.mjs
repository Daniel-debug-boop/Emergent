/**
 * EMERGENT shader tests — the real GLSL, actually parsed.
 *
 * The headless runtime stores shader source and reports COMPILE_STATUS true for
 * everything, because it does not compile GLSL. That means a syntax error in a
 * shader is completely invisible to the rest of the suite: the game would run
 * "clean" — no thrown frame, no validation error, no NaN upload — with a
 * fragment shader the driver rejects on first use, and the player would see a
 * black screen.
 *
 * This is not hypothetical. The scene shader was a flat Lambert term with no
 * material lookup and no texture sampling for the entire life of the project,
 * so every test in the repository passed against a shader that ignored the
 * entire material system. Passing tests are not evidence that the thing you
 * meant to test is the thing being tested.
 *
 * So the shaders are parsed here with a real GLSL parser, and — more usefully —
 * the *wiring* is asserted: every uniform the renderer sets must be declared in
 * the shader that consumes it, and every vertex attribute the buffer supplies
 * must be read by the vertex shader. That second check is the one that catches
 * an uploaded-but-unused stream, which is exactly the class of bug that let the
 * untextured shader survive.
 *
 * Run: `npm run test:shaders`
 */
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { parser } from '@shaderfrog/glsl-parser';

const root = dirname(fileURLToPath(import.meta.url));

/** The one number the shader's uniform array sizes are interpolated from. */
const MAX_MATERIALS = Number(
  readFileSync(join(root, 'materials.mjs'), 'utf8').match(/MAX_MATERIALS = (\d+);/)[1]
);

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

// ---------------------------------------------------------------------------
// Extract the shader sources out of game3d.js
// ---------------------------------------------------------------------------

const gameSource = readFileSync(join(root, 'game3d.js'), 'utf8');

/**
 * Pull a `const NAME = \`...\`;` shader out of the game source and resolve the
 * one template interpolation the shader uses, so the text parsed here is the
 * text the driver is handed. Parsing the un-interpolated source would be
 * parsing something the GPU never sees.
 */
function shaderSource(name) {
  const match = gameSource.match(new RegExp(`const ${name} = \`([\\s\\S]*?)\`;`));
  if (!match) throw new Error(`could not find shader '${name}' in game3d.js`);
  return match[1].replace(/\$\{MAT_UNIFORMS\}/g, String(MAX_MATERIALS));
}

/** Every `uniform <type> <name>` declaration, with arrays flattened to the name. */
function declaredUniforms(glsl) {
  const names = new Set();
  const re = /uniform\s+(?:lowp|mediump|highp\s+)?\w+\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;/g;
  let m;
  while ((m = re.exec(glsl))) {
    names.add(m[1]);
    names.add(`${m[1]}[0]`);
  }
  return names;
}

/** Every `layout(location=N) in <type> <name>` in a vertex shader. */
function readAttributes(glsl) {
  const map = new Map();
  const re = /layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+\w+\s+(\w+)\s*;/g;
  let m;
  while ((m = re.exec(glsl))) map.set(Number(m[1]), m[2]);
  return map;
}

const SHADERS = ['sceneVS', 'sceneFS', 'shadowVS', 'shadowFS'];
const sources = {};
for (const name of SHADERS) sources[name] = shaderSource(name);

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

await test('every shader in the game is valid GLSL', () => {
  for (const name of SHADERS) {
    const glsl = sources[name];
    assert(glsl.startsWith('#version 300 es'), `${name}: must be GLSL ES 3.00`);
    assert(/precision\s+\w+\s+float\s*;/.test(glsl), `${name}: must declare float precision`);
    let error = null;
    // The parser reports unresolved identifiers straight to stderr — "Encountered
    // undefined variable: gl_Position" — which is expected here, because
    // uniforms and built-ins are declared to the driver, not to the parser.
    // Only a thrown parse error is a real failure, so the noise is suppressed
    // rather than left to look like one.
    const realWrite = process.stderr.write.bind(process.stderr);
    process.stderr.write = () => true;
    try {
      parser.parse(glsl);
    } catch (err) {
      error = err;
    } finally {
      process.stderr.write = realWrite;
    }
    assert(
      error === null,
      `${name} does not parse: ${error ? error.message : ''}${error && error.location ? ` at ${JSON.stringify(error.location.start)}` : ''}`
    );
  }
});

await test('the scene shader declares the uniforms the renderer actually sets', () => {
  // Every getUniformLocation name in game3d.js, for the scene program.
  const setNames = new Set();
  const re = /getUniformLocation\(sceneProg,\s*'([^']+)'\)/g;
  let m;
  while ((m = re.exec(gameSource))) setNames.add(m[1]);

  assert(setNames.size > 0, 'expected game3d.js to look up scene uniforms');
  const vs = declaredUniforms(sources.sceneVS);
  const fs = declaredUniforms(sources.sceneFS);
  const declared = new Set([...vs, ...fs]);

  for (const name of setNames) {
    const bare = name.replace(/\[0\]$/, '');
    assert(
      declared.has(name) || declared.has(bare),
      `the renderer sets '${name}' but no scene shader declares it — a uniform that is set and not declared is a silent no-op, which is how a whole material system can be uploaded and never applied`
    );
  }
});

await test('every uniform the scene shader declares is set by the renderer', () => {
  // The other direction. A shader that reads a uniform nobody uploads is
  // undefined behaviour at draw time and a silently wrong pixel on a real GPU,
  // so both directions have to hold.
  const setNames = new Set();
  const re = /getUniformLocation\(sceneProg,\s*'([^']+)'\)/g;
  let m;
  while ((m = re.exec(gameSource))) setNames.add(m[1].replace(/\[0\]$/, ''));

  const declared = new Set([
    ...declaredUniforms(sources.sceneVS),
    ...declaredUniforms(sources.sceneFS)
  ]);
  for (const name of declared) {
    const bare = name.replace(/\[0\]$/, '');
    assert(
      setNames.has(bare),
      `the scene shader declares '${name}' but the renderer never sets it — it would read as zero, which for a material table means every surface renders as the default`
    );
  }
});

await test('the scene vertex shader reads every vertex attribute the buffers supply', () => {
  // This is the check that would have caught the original bug. The buffer
  // supplies position, normal, colour and a material index; a vertex shader
  // that reads only the first three uploads a perfectly valid, perfectly
  // ignored attribute and every test in the suite still passes.
  const read = readAttributes(sources.sceneVS);
  assertEqual(read.size, 4, 'the scene vertex shader should read exactly four attributes');
  assertEqual(read.get(0), 'p', 'location 0 is position');
  assertEqual(read.get(1), 'n', 'location 1 is normal');
  assertEqual(read.get(2), 'c', 'location 2 is colour');
  assertEqual(read.get(3), 'm', 'location 3 is the material index');

  // And the buffer has to match: setAttributePointers must point all four.
  const ptrRe = /vertexAttribPointer\((\d),(\d),gl\.FLOAT,false,(?:VERTEX_BYTES|STRIDE),(\d+)\)/g;
  const pointers = new Map();
  let m;
  while ((m = ptrRe.exec(gameSource))) pointers.set(Number(m[1]), Number(m[2]));
  for (const [loc, size] of [[0, 3], [1, 3], [2, 3], [3, 1]]) {
    assertEqual(
      pointers.get(loc),
      size,
      `vertexAttribPointer(${loc}) should supply ${size} floats to match the shader's declaration`
    );
  }
});

await test('the scene shader samples the material texture arrays it is given', () => {
  const fs = sources.sceneFS;
  for (const sampler of ['uAlbedoTex', 'uNormalTex', 'uArmTex']) {
    assert(
      new RegExp(`sampler2DArray\\s+${sampler}\\b`).test(fs),
      `the fragment shader must declare ${sampler} as a sampler2DArray`
    );
    assert(
      new RegExp(`texture\\(\\s*${sampler}\\s*,`).test(fs),
      `the fragment shader must actually sample ${sampler}`
    );
  }
  // The layer index has to come from the material table, not from a constant.
  assert(
    /uniform\s+vec4\s+uMatA\[/.test(fs) && /uniform\s+vec4\s+uMatB\[/.test(fs),
    'the fragment shader must read the material table uniforms'
  );
  // And the material index has to reach the fragment stage from the vertex
  // attribute, which is the specific thing that was missing.
  assert(/layout\(location=3\)\s*in\s+float\s+m/.test(sources.sceneVS),
    'the vertex shader must read the material index attribute');
  assert(/out\s+float\s+vMat/.test(sources.sceneVS), 'the vertex shader must pass the material index through');
  assert(/in\s+float\s+vMat/.test(fs), 'the fragment shader must receive the material index');
});

await test('the material table uniform arrays are sized from MAX_MATERIALS', () => {
  const declaredA = sources.sceneFS.match(/uniform\s+vec4\s+uMatA\[\s*(\d+)\s*\]/);
  const declaredB = sources.sceneFS.match(/uniform\s+vec4\s+uMatB\[\s*(\d+)\s*\]/);
  assert(declaredA && declaredB, 'both uMatA and uMatB must be declared as arrays');
  assertEqual(Number(declaredA[1]), MAX_MATERIALS, 'uMatA array size must match MAX_MATERIALS');
  assertEqual(Number(declaredB[1]), MAX_MATERIALS, 'uMatB array size must match MAX_MATERIALS');

  // And the table the renderer uploads has to actually fit in it, or the
  // driver truncates the array and every material past the limit renders as
  // whatever row zero happens to be.
  const materialCount = Object.keys(
    JSON.parse(readFileSync(join(root, 'assets', 'materials.json'), 'utf8')).materials
  ).length;
  assert(materialCount > 0, 'expected the bake to have produced a material set');
  assert(
    materialCount <= MAX_MATERIALS,
    `the material set has ${materialCount} entries but uMatA is sized ${MAX_MATERIALS}`
  );
});

await test('the shadow program has its own minimal shader', () => {
  assert(/uPV/.test(declaredUniforms(sources.shadowVS) ? [...declaredUniforms(sources.shadowVS)].join(',') : ''),
    'the shadow vertex shader must take the projection uniform');
  assertEqual(readAttributes(sources.shadowVS).size, 1,
    'the shadow vertex shader should read position only');
});

await test('the fragment shader is lit, not a flat colour multiply', () => {
  // A regression guard on the specific regression: the old shader was
  // `vC * (ambient + lambert)`, which passes every other test in the suite.
  const fs = sources.sceneFS;
  assert(/d_ggx|g_smith/.test(fs) || /ggx|smith/i.test(fs),
    'the fragment shader should use a microfacet BRDF, not a Lambert term');
  assert(!/float\s+light\s*=\s*0\.28/.test(fs),
    'the flat Lambert term is gone; this guard exists so it cannot silently return');
});

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

let failures = 0;
let totalChecks = 0;
for (const t of results) {
  totalChecks += t.checks;
  if (t.error) {
    failures++;
    console.log(`✗ ${t.name}`);
    console.log(`    ${t.error.split('\n').slice(0, 4).join('\n    ')}`);
  } else {
    console.log(`✓ ${t.name} — ok (${t.checks} checks)`);
  }
}
console.log(`${results.length - failures}/${results.length} shader tests passed, ${totalChecks} assertions`);
process.exit(failures ? 1 : 0);
