/**
 * The model set, without the 28 MB.
 *
 * WHY THIS EXISTS. `assets/models.gen.mjs` is the baked payload: 21 CC0 models,
 * three LODs each, base64-encoded vertex data, about 28 MB. It is a build
 * artefact, it is not in git, and a clean checkout therefore does not have it.
 * `interiors.mjs` used to import it statically, which meant a clean checkout
 * could not load the module at all -- not a degraded game, a module-not-found
 * error at startup, in the test suite and in the dist.
 *
 * So the shape of the data lives here, in a file small enough to commit, and the
 * payload is injected into it. The room enum, the vertex stride and the LOD
 * decode are all part of the *format*, not of the *content*, and a consumer
 * needs all three whether or not any model has been loaded yet.
 *
 * The consequence worth stating: with nothing injected, `MODELS` is empty and
 * `modelSetLoaded()` is false, and interiors build a shell with no furniture in
 * it. That is a visibly emptier building, not a crash, and not a wrong room.
 * Anything that cares reports it rather than rendering nothing quietly.
 */

/** Interleaved vertex stride: position, normal, colour, material, uv. */
export const FLOATS_PER_VERTEX = 12;

/** The room archetypes, and the plan each one is furnished from. */
export const ROOM = {
  RESIDENTIAL: 'residential',
  OFFICE: 'office',
  RETAIL: 'retail',
  WORKSHOP: 'workshop',
};

/**
 * The loaded models, keyed by id.
 *
 * Rebound by setModelSet() rather than mutated in place, so a module that
 * destructured the old object keeps a coherent snapshot instead of seeing the
 * map change underneath it mid-frame.
 */
export let MODELS = {};

/** Every loaded model id, in declaration order. */
export let MODEL_IDS = [];

/** True once a payload has been injected. */
export function modelSetLoaded() {
  return MODEL_IDS.length > 0;
}

/**
 * Adopt a generated model set.
 *
 * Takes the module namespace of `assets/models.gen.mjs` rather than the payload
 * itself, so the caller can do `setModelSet(await import('./models.gen.mjs'))`
 * and the 28 MB stays in one place. Returns the model ids in declaration order.
 * Validated rather than trusted: a set that disagrees about the vertex stride
 * would silently produce rooms of nonsense geometry, and the stride is the one
 * field every consumer assumes.
 */
export function setModelSet(generated) {
  if (!generated || typeof generated !== 'object') {
    throw new TypeError('setModelSet needs the generated module namespace');
  }
  if (generated.FLOATS_PER_VERTEX !== FLOATS_PER_VERTEX) {
    throw new Error(
      `model set has vertex stride ${generated.FLOATS_PER_VERTEX}, this build expects ${FLOATS_PER_VERTEX}`
    );
  }
  for (const [key, value] of Object.entries(ROOM)) {
    if (generated.ROOM && generated.ROOM[key] !== value) {
      throw new Error(
        `model set room "${key}" is ${generated.ROOM[key]}, this build expects ${value}`
      );
    }
  }
  const models = generated.MODELS;
  if (!models || typeof models !== 'object') {
    throw new Error('model set has no MODELS map');
  }
  // Every model must carry the fields the furniture placer reads, and they must
  // agree with the ids it looks up. A set missing one would place nothing for
  // that model and no error anywhere.
  for (const [id, model] of Object.entries(models)) {
    if (!Array.isArray(model.lods) || model.lods.length === 0) {
      throw new Error(`model "${id}" has no LODs`);
    }
    for (let level = 0; level < model.lods.length; level++) {
      const lod = model.lods[level];
      if (!lod || typeof lod.data !== 'string') {
        throw new Error(`model "${id}" LOD${level} has no encoded data`);
      }
      if (!Number.isFinite(model.bounds?.min?.[0]) || !Number.isFinite(model.bounds?.max?.[0])) {
        throw new Error(`model "${id}" has no finite bounds`);
      }
    }
  }
  MODELS = models;
  MODEL_IDS = Object.keys(models);
  return MODEL_IDS;
}

/**
 * Decode one LOD's base64 into a Float32Array.
 *
 * Hand-rolled rather than delegated to the generated module, so the decode
 * exists whether or not a payload has been injected -- and so the round-trip is
 * testable without a 28 MB fixture.
 */
export function lodVertices(model, level) {
  const l = model.lods[level];
  if (!l) throw new Error(`model has no LOD${level}`);
  const bin = atob(l.data);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}
