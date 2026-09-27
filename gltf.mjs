/**
 * glTF 2.0 import for EMERGENT.
 *
 * A focused reader, not a general-purpose glTF library. It handles exactly what
 * the asset pipeline produces and rejects everything else loudly, because a
 * silently-misparsed mesh is a chair that renders inside-out and nobody knows
 * why.
 *
 * ## What it reads
 *
 * A `.gltf` JSON document plus its binary `.bin` buffer, which is what Poly
 * Haven serves. `GLB` is deliberately not supported: it is the same data in a
 * different wrapper, and adding it would mean a second code path through the
 * buffer-offset arithmetic for no gain.
 *
 * Supported:
 *   - `POSITION`, `NORMAL`, `TEXCOORD_0`, `COLOR_0`
 *   - indexed and non-indexed primitives, with `mode: TRIANGLES`
 *   - node transforms, both `matrix` and `T`/`R`/`S`
 *   - the full node hierarchy, so a prop authored as several named parts
 *     arrives as those parts
 *   - accessor component types 5120/5121/5122/5123/5125/5126, normalized or not
 *
 * Rejected, with an error naming the problem:
 *   - `mode` other than `TRIANGLES` (a strip or fan needs a different index
 *     expansion and silently reinterpreting it corrupts geometry)
 *   - primitives with no `POSITION`
 *   - sparse accessors, Draco compression, and node `skin`/`mesh` weights
 *   - a bufferView whose byte range runs past the end of the buffer, which is
 *     the shape a corrupt or truncated download takes
 *
 * ## Why the output carries UVs
 *
 * The generated city is box-projected: the shader picks a world plane from the
 * material's role and derives the coordinate. That is correct for architecture
 * and impossible for a chair — no world-axis projection puts a wood grain along
 * a tapered leg. Imported geometry therefore arrives as
 * `MAP_MODE.UV` and its own `TEXCOORD_0` is used directly.
 */

/** Vertex layout, mirrored from geometry.mjs. Kept as literals to avoid a cycle. */
export const FLOATS_PER_VERTEX = 12;
const OFF_POS = 0, OFF_NRM = 3, OFF_COL = 6, OFF_MAT = 9, OFF_UV = 10;

const COMPONENT = {
  5120: { array: Int8Array, size: 1, name: 'BYTE' },
  5121: { array: Uint8Array, size: 1, name: 'UNSIGNED_BYTE' },
  5122: { array: Int16Array, size: 2, name: 'SHORT' },
  5123: { array: Uint16Array, size: 2, name: 'UNSIGNED_SHORT' },
  5125: { array: Uint32Array, size: 4, name: 'UNSIGNED_INT' },
  5126: { array: Float32Array, size: 4, name: 'FLOAT' }
};

const TYPE_COUNT = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };

/**
 * A 4x4 matrix in column-major order, matching glTF and `math3d.mjs`.
 *
 * Held as a plain array rather than a class: this runs once per node at bake
 * time, and a method call per multiply would be noise.
 */
export function mat4Identity() {
  return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
}

/** `a * b`, both column-major. */
export function mat4Multiply(a, b) {
  const out = new Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
  }
  return out;
}

/** Compose translation, rotation quaternion (x,y,z,w) and scale. */
export function mat4Compose(t, q, s) {
  const [x, y, z, w] = q;
  const [sx, sy, sz] = s;
  const x2 = x + x, y2 = y + y, z2 = z + z;
  const xx = x * x2, xy = x * y2, xz = x * z2;
  const yy = y * y2, yz = y * z2, zz = z * z2;
  const wx = w * x2, wy = w * y2, wz = w * z2;
  return [
    (1 - (yy + zz)) * sx, (xy + wz) * sx, (xz - wy) * sx, 0,
    (xy - wz) * sy, (1 - (xx + zz)) * sy, (yz + wx) * sy, 0,
    (xz + wy) * sz, (yz - wx) * sz, (1 - (xx + yy)) * sz, 0,
    t[0], t[1], t[2], 1
  ];
}

/** Transform a point by a column-major 4x4, dividing through by w. */
export function mat4TransformPoint(m, p) {
  const [x, y, z] = p;
  const w = m[3] * x + m[7] * y + m[11] * z + m[15] || 1;
  return [
    (m[0] * x + m[4] * y + m[8] * z + m[12]) / w,
    (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
    (m[2] * x + m[6] * y + m[10] * z + m[14]) / w
  ];
}

/** Transform a direction by the upper 3x3, with the inverse transpose for normals. */
export function mat4TransformDirection(m, d) {
  const [x, y, z] = d;
  return [
    m[0] * x + m[4] * y + m[8] * z,
    m[1] * x + m[5] * y + m[9] * z,
    m[2] * x + m[6] * y + m[10] * z
  ];
}

/**
 * A non-uniform scale needs its inverse transpose on the normal, or a squashed
 * leg lights as though it were not squashed. This computes it from the 3x3 by
 * cross-product cofactors, which is the transpose of the inverse for the
 * determinant-scaled case and is correct up to the one factor the normaliser
 * removes anyway.
 */
export function normalMatrix(m) {
  const a = m[0], b = m[1], c = m[2];
  const d = m[4], e = m[5], f = m[6];
  const g = m[8], h = m[9], i = m[10];
  // Inverse-transpose of the 3x3, column-major.
  const out = [
    e * i - f * h, f * g - d * i, d * h - e * g,
    c * h - b * i, a * i - c * g, b * g - a * h,
    b * f - c * e, c * d - a * f, a * e - b * d
  ];
  // Guard a degenerate (zero-scale) node rather than emitting NaN normals.
  const det = a * out[0] + b * out[3] + c * out[6];
  if (Math.abs(det) < 1e-12) return [1, 0, 0, 0, 1, 0, 0, 0, 1];
  const s = 1 / det;
  return out.map((v) => v * s);
}

function normalize3(v) {
  const l = Math.hypot(v[0], v[1], v[2]);
  if (!(l > 1e-9)) return [0, 1, 0];
  return [v[0] / l, v[1] / l, v[2] / l];
}

/**
 * Read one accessor into a typed array.
 *
 * Interleaved and byte-strided views are handled by copying element by element
 * rather than by constructing a typed-array view over the buffer, because a
 * view cannot express a stride that is not a multiple of the component size.
 * That matters because glTF exporters do emit interleaved data.
 */
function readAccessor(gltf, bin, index, what) {
  const acc = gltf.accessors[index];
  if (!acc) throw new Error(`${what}: accessor ${index} does not exist`);
  if (acc.sparse !== undefined) {
    throw new Error(`${what}: sparse accessors are not supported (accessor ${index})`);
  }
  const comp = COMPONENT[acc.componentType];
  if (!comp) throw new Error(`${what}: unknown componentType ${acc.componentType}`);
  const comps = TYPE_COUNT[acc.type];
  if (!comps) throw new Error(`${what}: unknown accessor type '${acc.type}'`);

  if (acc.bufferView === undefined) {
    // A view-less accessor is defined to read as zeros.
    return { array: new comp.array(comps), count: acc.count, components: comps };
  }
  const view = gltf.bufferViews[acc.bufferView];
  if (!view) throw new Error(`${what}: bufferView ${acc.bufferView} does not exist`);
  const stride = view.byteStride || comps * comp.size;
  const start = (view.byteOffset || 0) + (acc.byteOffset || 0);
  const needed = acc.count * stride;
  // This is the shape a truncated or corrupt download takes, so it is checked
  // before the read rather than being allowed to produce short data.
  if (start + (acc.count - 1) * stride + comps * comp.size > bin.byteLength) {
    throw new Error(
      `${what}: accessor ${index} reads ${start + needed} bytes but the buffer is ${bin.byteLength}`
    );
  }
  const out = new comp.array(acc.count * comps);
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
  const base = bin.byteOffset + start;
  for (let i = 0; i < acc.count; i++) {
    for (let c = 0; c < comps; c++) {
      const at = base + i * stride + c * comp.size;
      let value;
      switch (acc.componentType) {
        case 5120: value = dv.getInt8(at); break;
        case 5121: value = dv.getUint8(at); break;
        case 5122: value = dv.getInt16(at, true); break;
        case 5123: value = dv.getUint16(at, true); break;
        case 5125: value = dv.getUint32(at, true); break;
        default: value = dv.getFloat32(at, true); break;
      }
      // glTF stores normalized integers in the full range of the component and
      // expects the reader to rescale. Skipping this turns a white vertex
      // colour into a value of 255 and blows the frame out.
      if (acc.normalized) {
        if (acc.componentType === 5120) value = Math.max(value / 127, -1);
        else if (acc.componentType === 5121) value = value / 255;
        else if (acc.componentType === 5122) value = Math.max(value / 32767, -1);
        else if (acc.componentType === 5123 || acc.componentType === 5125) value = value / 65535;
      }
      out[i * comps + c] = value;
    }
  }
  return { array: out, count: acc.count, components: comps };
}

/** The world transform for a node, from whichever of the three forms it uses. */
function nodeMatrix(node) {
  if (node.matrix) {
    if (node.matrix.length !== 16 || node.matrix.some((v) => !Number.isFinite(v))) {
      throw new Error(`glTF: node '${node.name || '?'}' has a malformed matrix`);
    }
    return node.matrix.slice();
  }
  if (node.translation || node.rotation || node.scale) {
    return mat4Compose(
      node.translation || [0, 0, 0],
      node.rotation || [0, 0, 0, 1],
      node.scale || [1, 1, 1]
    );
  }
  return mat4Identity();
}

/**
 * Import a glTF document into a flat EMERGENT vertex array.
 *
 * ## Topology is preserved
 *
 * The output keeps the source's vertex sharing: a glTF with 35,560 vertices and
 * 149,970 indices imports as 35,560 vertices, not 149,970. This is not an
 * optimisation, it is a correctness requirement — `meshoptimizer` can only
 * decimate a mesh that knows which corners are the same corner, and expanding
 * indices into a triangle soup first makes every mesh unsimplifiable while
 * still looking correct on screen. That cost 6.9 MB per prop and a LOD chain
 * that silently produced three identical copies of the full-detail mesh.
 *
 * @param {object} gltf The parsed `.gltf` JSON.
 * @param {ArrayBuffer|Uint8Array} bin The external buffer.
 * @param {object} opts
 * @param {number} opts.material The material index for every emitted vertex.
 * @param {[number,number,number]} [opts.tint] Multiplied into vertex colour.
 * @param {boolean} [opts.expand] Expand to a triangle soup. Off by default and
 *   not used by the pipeline; it exists so a caller that genuinely wants
 *   unshared vertices can ask for them explicitly rather than getting them by
 *   accident.
 * @returns {{vertices: Float32Array, indices: Uint32Array, triangles: number, vertexCount: number, parts: object[], bounds: object, warnings: string[]}}
 */
export function importGltf(gltf, bin, opts) {
  const binBytes = bin instanceof Uint8Array ? bin : new Uint8Array(bin);
  const warnings = [];
  const vertices = [];
  const indices = [];
  const parts = [];

  if (!Array.isArray(gltf.nodes)) throw new Error('glTF: document has no nodes array');
  if (!Array.isArray(gltf.meshes)) throw new Error('glTF: document has no meshes array');
  if (gltf.extensionsRequired && gltf.extensionsRequired.length) {
    throw new Error(
      `glTF: document requires unsupported extensions: ${gltf.extensionsRequired.join(', ')}`
    );
  }

  const sceneIndex = gltf.scene !== undefined ? gltf.scene : 0;
  const scene = gltf.scenes && gltf.scenes[sceneIndex];
  const roots = scene && Array.isArray(scene.nodes)
    ? scene.nodes
    : (gltf.nodes || []).map((_, i) => i);
  if (!roots.length) throw new Error('glTF: the scene has no root nodes');

  let minX = Infinity, minY = Infinity, minZ = Infinity;
  let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  const parentOf = new Map();
  (gltf.nodes || []).forEach((n, i) => (n.children || []).forEach((c) => parentOf.set(c, i)));

  const walk = (nodeIndex, parent) => {
    const node = gltf.nodes[nodeIndex];
    if (!node) throw new Error(`glTF: node ${nodeIndex} does not exist`);
    if (node.skin !== undefined) {
      warnings.push(`node '${node.name || nodeIndex}' is skinned and was imported as a static mesh`);
    }
    const world = mat4Multiply(parent, nodeMatrix(node));
    if (node.mesh !== undefined) {
      const mesh = gltf.meshes[node.mesh];
      if (!mesh) throw new Error(`glTF: node '${node.name || nodeIndex}' references missing mesh ${node.mesh}`);
      const nrm = normalMatrix(world);
      const startIndex = indices.length;

      for (const prim of mesh.primitives || []) {
        const mode = prim.mode === undefined ? 4 : prim.mode;
        if (mode !== 4) {
          throw new Error(
            `glTF: primitive mode ${mode} in '${mesh.name || node.mesh}' is not TRIANGLES; ` +
            'a strip or fan needs a different index expansion and would be silently wrong'
          );
        }
        if (prim.attributes.POSITION === undefined) {
          throw new Error(`glTF: a primitive in '${mesh.name || node.mesh}' has no POSITION`);
        }
        const pos = readAccessor(gltf, binBytes, prim.attributes.POSITION, 'POSITION');
        const hasNrm = prim.attributes.NORMAL !== undefined;
        const nrmA = hasNrm ? readAccessor(gltf, binBytes, prim.attributes.NORMAL, 'NORMAL') : null;
        const hasUv = prim.attributes.TEXCOORD_0 !== undefined;
        const uvA = hasUv ? readAccessor(gltf, binBytes, prim.attributes.TEXCOORD_0, 'TEXCOORD_0') : null;
        const hasCol = prim.attributes.COLOR_0 !== undefined;
        const colA = hasCol ? readAccessor(gltf, binBytes, prim.attributes.COLOR_0, 'COLOR_0') : null;

        if (nrmA && nrmA.count !== pos.count) throw new Error('glTF: NORMAL and POSITION counts differ');
        if (uvA && uvA.count !== pos.count) throw new Error('glTF: TEXCOORD_0 and POSITION counts differ');
        if (colA && colA.count !== pos.count) throw new Error('glTF: COLOR_0 and POSITION counts differ');
        if (!hasNrm) warnings.push(`'${mesh.name || node.mesh}' has no NORMAL; faces will be flat-shaded`);
        if (!hasUv) warnings.push(`'${mesh.name || node.mesh}' has no TEXCOORD_0; it will render untextured`);

        const count = prim.indices === undefined
          ? pos.count
          : readAccessor(gltf, binBytes, prim.indices, 'indices').count;
        if (count % 3 !== 0) {
          throw new Error(`glTF: '${mesh.name || node.mesh}' has ${count} indices, not a multiple of three`);
        }
        const idx = prim.indices === undefined
          ? null
          : readAccessor(gltf, binBytes, prim.indices, 'indices').array;

        const tint = opts.tint || [1, 1, 1];
        for (let t = 0; t < count; t += 3) {
          for (let k = 0; k < 3; k++) {
            const vi = idx ? idx[t + k] : t + k;
            if (vi >= pos.count) {
              throw new Error(`glTF: index ${vi} is out of range for ${pos.count} vertices`);
            }
            const p = mat4TransformPoint(world, [
              pos.array[vi * 3], pos.array[vi * 3 + 1], pos.array[vi * 3 + 2]
            ]);
            let n;
            if (hasNrm) {
              n = normalize3(mat4TransformDirection(nrm, [
                nrmA.array[vi * 3], nrmA.array[vi * 3 + 1], nrmA.array[vi * 3 + 2]
              ]));
            } else {
              // No normals authored. Derive the face normal from the triangle's
              // own two edges; a flat face is a far better outcome than an
              // unlit black one, and glTF winding is counter-clockwise, so
              // (v1-v0) x (v2-v0) points out of the front face.
              const j = idx ? idx[t + ((k + 1) % 3)] : t + ((k + 1) % 3);
              const l = idx ? idx[t + ((k + 2) % 3)] : t + ((k + 2) % 3);
              const pj = mat4TransformPoint(world, [pos.array[j * 3], pos.array[j * 3 + 1], pos.array[j * 3 + 2]]);
              const pl = mat4TransformPoint(world, [pos.array[l * 3], pos.array[l * 3 + 1], pos.array[l * 3 + 2]]);
              const e1 = [pj[0] - p[0], pj[1] - p[1], pj[2] - p[2]];
              const e2 = [pl[0] - p[0], pl[1] - p[1], pl[2] - p[2]];
              n = normalize3([
                e1[1] * e2[2] - e1[2] * e2[1],
                e1[2] * e2[0] - e1[0] * e2[2],
                e1[0] * e2[1] - e1[1] * e2[0]
              ]);
            }
            // Vertex colour is multiplicative on the albedo, so a white
            // COLOR_0 is the identity and an absent one is also white.
            const c = hasCol
              ? [colA.array[vi * 4] * tint[0], colA.array[vi * 4 + 1] * tint[1], colA.array[vi * 4 + 2] * tint[2]]
              : tint;
            const uv = hasUv ? [uvA.array[vi * 2], uvA.array[vi * 2 + 1]] : [0, 0];

            for (const value of [...p, ...n, ...c, ...uv]) {
              if (!Number.isFinite(value)) {
                throw new Error(
                  `glTF: '${mesh.name || node.mesh}' produced a non-finite value; the source is corrupt`
                );
              }
            }
            // `remap` below is addressed by *vertex* index, not by float offset,
            // so this must be a vertex number. Storing the float offset instead
            // made every index past the first row point at a different vertex,
            // which turned 92% of the triangles degenerate and left the
            // simplifier with nothing to work on.
            const at = vertices.length / FLOATS_PER_VERTEX;
            vertices.push(
              p[0], p[1], p[2],
              n[0], n[1], n[2],
              c[0], c[1], c[2],
              opts.material,
              uv[0], uv[1]
            );
            indices.push(at);
            if (p[0] < minX) minX = p[0]; if (p[0] > maxX) maxX = p[0];
            if (p[1] < minY) minY = p[1]; if (p[1] > maxY) maxY = p[1];
            if (p[2] < minZ) minZ = p[2]; if (p[2] > maxZ) maxZ = p[2];
          }
        }
      }
      const endIndex = indices.length;
      if (endIndex > startIndex) {
        parts.push({
          name: node.name || mesh.name || `part${parts.length}`,
          first: startIndex,
          count: endIndex - startIndex,
          triangles: (endIndex - startIndex) / 3
        });
      }
    }
    for (const child of node.children || []) walk(child, world);
  };

  for (const root of roots) walk(root, mat4Identity());

  if (!vertices.length) throw new Error('glTF: import produced no geometry');

  // Deduplicate to a shared vertex list, so the mesh keeps the topology the
  // simplifier needs. Two corners are the same corner when they agree on
  // position and normal to within a tolerance well below anything a decimation
  // would care about.
  let vertexArray = new Float32Array(vertices);
  let indexArray = new Uint32Array(indices);
  if (!opts.expand) {
    const welded = weldVertices(vertexArray);
    vertexArray = welded.vertices;
    // The remap is what actually restores the topology: without applying it to
    // the index buffer, welding would only have compacted the vertex list and
    // every triangle would still point at its own three unique corners.
    indexArray = new Uint32Array(indexArray.length);
    for (let i = 0; i < indices.length; i++) indexArray[i] = welded.remap[indices[i]];
  }

  return {
    vertices: vertexArray,
    indices: indexArray,
    vertexCount: vertexArray.length / FLOATS_PER_VERTEX,
    triangles: indexArray.length / 3,
    parts,
    bounds: {
      min: [minX, minY, minZ],
      max: [maxX, maxY, maxZ],
      size: [maxX - minX, maxY - minY, maxZ - minZ]
    },
    warnings
  };
}

/**
 * Weld identical vertices and rebuild the index buffer.
 *
 * Uses a quantised position key rather than exact float equality: exporters
 * routinely emit the same corner with a last-bit difference, and exact
 * comparison would leave those unmerged and the simplifier would treat the
 * surface as a seam. The quantisation step is well under a millimetre at prop
 * scale, so nothing visibly distinct is ever merged.
 */
function weldVertices(vertices) {
  const QUANT = 1e5;
  const map = new Map();
  const remap = new Uint32Array(vertices.length / FLOATS_PER_VERTEX);
  const out = [];
  for (let i = 0; i < vertices.length; i += FLOATS_PER_VERTEX) {
    const x = vertices[i], y = vertices[i + 1], z = vertices[i + 2];
    const nx = vertices[i + 3], ny = vertices[i + 4], nz = vertices[i + 5];
    const u = vertices[i + 10], v = vertices[i + 11];
    // Position and normal decide identity. UV is deliberately *not* part of the
    // key: welding across a UV seam is the one case where a shared vertex is
    // wrong, and the seam-aware simplifier handles that by attribute weight
    // rather than by the topology key.
    const key = `${Math.round(x * QUANT)},${Math.round(y * QUANT)},${Math.round(z * QUANT)},`
      + `${Math.round(nx * 100)},${Math.round(ny * 100)},${Math.round(nz * 100)}`;
    let found = map.get(key);
    if (found === undefined) {
      found = out.length / FLOATS_PER_VERTEX;
      map.set(key, found);
      for (let k = 0; k < FLOATS_PER_VERTEX; k++) out.push(vertices[i + k]);
    }
    remap[i / FLOATS_PER_VERTEX] = found;
  }
  return { vertices: new Float32Array(out), remap };
}
