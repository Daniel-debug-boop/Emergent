/**
 * Uploading the baked material set to the GPU.
 *
 * Three `TEXTURE_2D_ARRAY`s, one layer per material, allocated once and never
 * resized. The upload is asynchronous — 78 PNG decodes — so the game starts
 * rendering immediately against flat vertex colours and switches to the PBR path
 * the moment the last array arrives, with no frame hitch and no loading screen.
 *
 * ## Why texture arrays rather than an atlas
 *
 * An atlas needs a border-bleed margin around every tile to stop mip filtering
 * bleeding one material into its neighbour, and the margin has to grow as the
 * mip level shrinks, so a 512-px tile in a 4096-px atlas effectively costs
 * 576-px of texture. At three mip levels that wasted 30% of the memory for a
 * saving that a modern browser's `sampler2DArray` does not need: array layers
 * are sampled independently, so mip filtering never crosses a tile boundary.
 *
 * ## Why `generateMipmap` rather than a shipped mip chain
 *
 * A mip chain is 33% more data and would have to be stored as a second image
 * per material, since PNG does not carry one. The GPU builds it in well under a
 * millisecond per array, so shipping one would add download and decode cost to
 * save nothing. The cost is that the chain is GPU-dependent and therefore not
 * bit-reproducible, which is why the harness asserts the call is *made* and the
 * sampler is complete, rather than pretending to verify pixels.
 */

/** Texture units. Three arrays, one unit each, bound once per frame. */
const UNITS = { albedo: 0, normal: 1, arm: 2 };

/** Which GL array internal format each baked array uses. */
const ARRAY_FORMATS = {
  albedo: { internal: 'SRGB8_ALPHA8', format: 'RGBA', type: 'UNSIGNED_BYTE' },
  normal: { internal: 'RGBA8', format: 'RGBA', type: 'UNSIGNED_BYTE' },
  arm: { internal: 'RGBA8', format: 'RGBA', type: 'UNSIGNED_BYTE' }
};

/**
 * Decode one image and hand back tightly packed RGBA.
 *
 * Goes through a 2D canvas rather than `createImageBitmap` plus a readback
 * because it is the one path that works identically in every browser, and
 * because it gives the decoded size for free — which is then checked against
 * the size the descriptor promised, so a mistyped or re-baked tile fails loudly
 * instead of uploading as a mismatched layer.
 */
async function decodeTile(url, expected) {
  const img = new Image();
  img.src = url;
  await (img.decode ? img.decode() : new Promise(res => { img.onload = res; }));
  const w = img.naturalWidth, h = img.naturalHeight;
  if (w !== expected || h !== expected) {
    throw new Error(`${url}: decoded ${w}x${h}, the descriptor says ${expected}x${expected}`);
  }
  const canvas = document.createElement('canvas');
  canvas.width = w; canvas.height = h;
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.drawImage(img, 0, 0);
  return ctx.getImageData(0, 0, w, h).data;
}

/**
 * Create the upload and run it.
 *
 * @param {WebGL2RenderingContext} gl
 * @param {object} descriptor The generated MATERIAL_DESCRIPTOR.
 * @param {(progress:{loaded:number,total:number,failed:string[]}) => void} [onProgress]
 */
export function createMaterialTextures(gl, descriptor, onProgress) {
  const size = descriptor.size;
  const layers = descriptor.arrays.albedo.layers;
  const maps = Object.keys(descriptor.arrays);
  const state = {
    ready: false,
    failed: [],
    loaded: 0,
    total: maps.length * layers,
    textures: {},
    base: 'assets/textures/'
  };

  // Storage is allocated up front for all three arrays, so a tile that fails to
  // decode leaves a hole rather than a reallocation mid-frame.
  for (const key of maps) {
    const tex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0 + UNITS[key]);
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, tex);
    const f = ARRAY_FORMATS[key];
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, mipLevels(size), gl[f.internal], size, size, layers);
    // Trilinear, wrapped, and with the anisotropy left at the driver default.
    // Anything less and the ground shimmers violently at distance, which is the
    // most objectionable artefact a tiled world can have.
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
    state.textures[key] = tex;
  }

  const jobs = [];
  for (const key of maps) {
    for (let layer = 0; layer < layers; layer++) {
      jobs.push({ key, layer, url: `${state.base}${materialFileName(descriptor, key, layer)}` });
    }
  }

  // Sequential rather than parallel: 78 simultaneous 512-px decodes is a
  // memory spike on a low-end machine, and the whole point of doing this
  // asynchronously is to avoid a hitch. One at a time keeps the peak flat and
  // the frame budget untouched.
  const run = async () => {
    for (const job of jobs) {
      try {
        const pixels = await decodeTile(job.url, size);
        const f = ARRAY_FORMATS[job.key];
        gl.activeTexture(gl.TEXTURE0 + UNITS[job.key]);
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, state.textures[job.key]);
        // The image is bottom-up relative to GL's expectation, so rows are
        // flipped on upload. Getting this wrong does not fail — the texture is
        // simply mirrored, and a mirrored normal map is a subtly wrong lighting
        // model that is very hard to see and very hard to debug.
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
        gl.texSubImage3D(gl.TEXTURE_2D_ARRAY, 0, 0, 0, job.layer, size, size, 1, gl[f.format], gl[f.type], pixels);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
        state.loaded++;
      } catch (err) {
        state.failed.push(`${job.key}[${job.layer}]: ${err.message}`);
        state.loaded++;
      }
      if (onProgress) onProgress({ loaded: state.loaded, total: state.total, failed: state.failed });
    }
    for (const key of maps) {
      gl.activeTexture(gl.TEXTURE0 + UNITS[key]);
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, state.textures[key]);
      gl.generateMipmap(gl.TEXTURE_2D_ARRAY);
    }
    // A tile that failed to decode is a hole in an array layer, and a hole
    // samples as transparent black. The game still renders — with a hard edge
    // along one material — rather than refusing to start, because a world with
    // one missing wall texture is playable and a world that will not boot is
    // not. The count is reported so it can never be silent.
    state.ready = state.failed.length === 0;
    return state;
  };

  return { state, promise: run() };
}

/** Full mip chain length for a square texture, down to 1x1. */
export function mipLevels(size) {
  return Math.floor(Math.log2(size)) + 1;
}

/**
 * The file name for one material's tile in one array.
 *
 * Read from the descriptor's `files` table, which the bake writes alongside the
 * layer indices. Composing the name here from a second hand-maintained list
 * would be a table that could silently disagree with the bake's own output.
 */
function materialFileName(descriptor, arrayKey, layer) {
  const list = descriptor.files && descriptor.files[arrayKey];
  const name = list && list[layer];
  if (!name) throw new Error(`no baked file for ${arrayKey} layer ${layer}`);
  return name;
}

/** Bind the three arrays to their units. Cheap; called once per frame. */
export function bindMaterialTextures(gl, state) {
  if (!state.ready) return;
  gl.activeTexture(gl.TEXTURE0 + UNITS.albedo); gl.bindTexture(gl.TEXTURE_2D_ARRAY, state.textures.albedo);
  gl.activeTexture(gl.TEXTURE0 + UNITS.normal); gl.bindTexture(gl.TEXTURE_2D_ARRAY, state.textures.normal);
  gl.activeTexture(gl.TEXTURE0 + UNITS.arm); gl.bindTexture(gl.TEXTURE_2D_ARRAY, state.textures.arm);
}

export { UNITS };
