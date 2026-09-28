/**
 * EMERGENT's geometry kit.
 *
 * Every triangle in the world is emitted through one of these functions. They
 * share a single vertex layout, a single material index space and a single set
 * of conventions, which is what makes it possible to build a city out of them
 * without any surface having to be a special case.
 *
 * ## Vertex layout — 10 floats, 40 bytes
 *
 *   position(3) normal(3) colour(3) material(1)
 *
 * There is no UV. Texture coordinates are derived in the vertex shader by
 * projecting the world position onto the plane the normal points out of, scaled
 * by the material's tiling density; see materials.mjs for why that is the right
 * choice for a static world and the wrong one for a moving one.
 *
 * ## Why the kit exists at all
 *
 * A city rendered as boxes reads as a prototype no matter how good the
 * textures are, because the thing that makes architecture recognisable is not
 * its wall texture — it is the shadow line under a window sill, the projection
 * of a cornice, the reveal of a doorway, the rail on a balcony. Those are
 * separate small solids. Emitting them once, here, correctly scaled and
 * correctly materialled, is what turns a box into a building.
 *
 * ## Units
 *
 * One world unit is one metre, and the kit is dimensioned in real-world sizes:
 * a kerb is 0.14 m, a door is 2.1 m, a window sill is 0.9 m, a street lamp is
 * 8 m. Nothing here is scaled to look right in isolation — it is scaled to be
 * right next to a 1.8 m person, because that is the only scale a player can
 * check.
 */

/**
 * Floats per vertex. Mirrored by the renderer's vertexAttribPointer stride.
 *
 * position(3) normal(3) colour(3) material(1) uv(2).
 *
 * The UV pair is present because imported models need real texture coordinates
 * and box mapping cannot supply them. It is the last two floats rather than a
 * fifth attribute so the stride stays one contiguous read, and every procedural
 * emitter in this kit writes zeros — a box-projected surface derives its
 * coordinate in the shader from the normal, and `MAP_MODE.UV` is what tells the
 * shader to use this attribute instead.
 */
export const VERTEX_FLOATS = 12;
export const VERTEX_BYTES = VERTEX_FLOATS * 4;

/**
 * Build the kit, bound to a material table.
 *
 * Materials are resolved by name at call time through `M()`, which throws on an
 * unknown name. A silently-defaulted material is the worst possible failure
 * here: it renders as a plausible grey solid, so a typo would survive review
 * and ship.
 */
export function createGeometryKit(materials) {
  const M = (id) => {
    const i = materials.byId[id];
    if (i === undefined) throw new Error(`geometry: unknown material '${id}'`);
    return i;
  };

  // -- core -----------------------------------------------------------------

  /**
   * One vertex. The single choke point every emitter goes through.
   *
   * The undefined check is not paranoia. A material index passed into the
   * colour slot — or the reverse — produces vertices that are *finite* and
   * therefore pass every NaN scan, then draw as garbage. It happened twice
   * while this kit was written, and the only reason it was caught quickly is
   * that it threw here first.
   */
  const v = (arr, p, n, c, m, uv) => {
    if (c === undefined || m === undefined) {
      throw new Error('geometry: a vertex was emitted without a colour or a material\n' + new Error().stack);
    }
    arr.push(p[0], p[1], p[2], n[0], n[1], n[2], c[0], c[1], c[2], m,
      uv === undefined ? 0 : uv[0], uv === undefined ? 0 : uv[1]);
  };

  /** A vertex carrying real texture coordinates, for imported geometry only. */
  const vu = (arr, p, n, c, m, u, w) => {
    if (!Number.isFinite(u) || !Number.isFinite(w)) {
      throw new Error(`geometry: a UV-mapped vertex was emitted with a non-finite coordinate (${u}, ${w})`);
    }
    v(arr, p, n, c, m, [u, w]);
  };

  const quad = (arr, a, b, c, d, n, col, m) => {
    v(arr, a, n, col, m); v(arr, b, n, col, m); v(arr, c, n, col, m);
    v(arr, a, n, col, m); v(arr, c, n, col, m); v(arr, d, n, col, m);
  };

  /** A quad with a distinct colour per corner, for gradients and soft edges. */
  const quadShaded = (arr, a, b, c, d, n, ca, cb, cc, cd, m) => {
    v(arr, a, n, ca, m); v(arr, b, n, cb, m); v(arr, c, n, cc, m);
    v(arr, a, n, ca, m); v(arr, c, n, cc, m); v(arr, d, n, cd, m);
  };

  /** A UV-mapped triangle, for imported geometry. */
  const triUV = (arr, a, b, c, n, col, m, ua, ub, uc) => {
    vu(arr, a, n, col, m, ua[0], ua[1]);
    vu(arr, b, n, col, m, ub[0], ub[1]);
    vu(arr, c, n, col, m, uc[0], uc[1]);
  };

  /**
   * An axis-aligned box.
   *
   * `faces` can override the material per face, which is how one call produces a
   * wall in brick with a metal plinth and a stone band: a building's base and
   * top are rarely the same material as its middle, and splitting them into
   * separate boxes triples the vertex count for no visual gain.
   */
  const box = (arr, x, y, z, w, h, d, col, m, opt) => {
    const top = (opt && opt.top) || null;
    const topCol = (opt && opt.topColor) || col;
    const fm = (opt && opt.faces) || null;
    const side = fm && fm.side !== undefined ? fm.side : m;
    const topM = (fm && fm.top !== undefined) ? fm.top : (top !== null ? top : side);
    const botM = fm && fm.bottom !== undefined ? fm.bottom : side;
    const x0 = x - w / 2, x1 = x + w / 2, y0 = y, y1 = y + h, z0 = z - d / 2, z1 = z + d / 2;
    quad(arr, [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0], [0, 0, -1], col, fm && fm.front !== undefined ? fm.front : side);
    quad(arr, [x1, y0, z1], [x0, y0, z1], [x0, y1, z1], [x1, y1, z1], [0, 0, 1], col, fm && fm.back !== undefined ? fm.back : side);
    quad(arr, [x0, y0, z1], [x0, y0, z0], [x0, y1, z0], [x0, y1, z1], [-1, 0, 0], col, fm && fm.left !== undefined ? fm.left : side);
    quad(arr, [x1, y0, z0], [x1, y0, z1], [x1, y1, z1], [x1, y1, z0], [1, 0, 0], col, fm && fm.right !== undefined ? fm.right : side);
    quad(arr, [x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1], [0, 1, 0], topCol, topM);
    quad(arr, [x0, y0, z1], [x1, y0, z1], [x1, y0, z0], [x0, y0, z0], [0, -1, 0], col, botM);
  };

  /** A box that tapers, for canopies, chimneys, towers and cargo containers. */
  const taper = (arr, x, y, z, wBot, dBot, wTop, dTop, h, col, m, capTop = true) => {
    const bx0 = x - wBot / 2, bx1 = x + wBot / 2, bz0 = z - dBot / 2, bz1 = z + dBot / 2;
    const tx0 = x - wTop / 2, tx1 = x + wTop / 2, tz0 = z - dTop / 2, tz1 = z + dTop / 2;
    const ty = y + h;
    quad(arr, [bx0, y, bz0], [bx1, y, bz0], [tx1, ty, tz0], [tx0, ty, tz0], [0, 0, -1], col, m);
    quad(arr, [bx1, y, bz1], [bx0, y, bz1], [tx0, ty, tz1], [tx1, ty, tz1], [0, 0, 1], col, m);
    quad(arr, [bx0, y, bz1], [bx0, y, bz0], [tx0, ty, tz0], [tx0, ty, tz1], [-1, 0, 0], col, m);
    quad(arr, [bx1, y, bz0], [bx1, y, bz1], [tx1, ty, tz1], [tx1, ty, tz0], [1, 0, 0], col, m);
    if (capTop) quad(arr, [tx0, ty, tz0], [tx1, ty, tz0], [tx1, ty, tz1], [tx0, ty, tz1], [0, 1, 0], col, m);
  };

  /** A closed cylinder. `caps` false gives a tube, which is what pipes need. */
  const cylinder = (arr, x, y, z, r, h, col, m, segments = 8, caps = true) => {
    for (let i = 0; i < segments; i++) {
      const a = (i / segments) * Math.PI * 2, b = ((i + 1) / segments) * Math.PI * 2;
      const ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b);
      const p0 = [x + ca * r, y, z + sa * r], p1 = [x + cb * r, y, z + sb * r];
      const q0 = [p0[0], y + h, p0[2]], q1 = [p1[0], y + h, p1[2]];
      const n0 = [ca, 0, sa], n1 = [cb, 0, sb];
      // Each facet gets its own two triangles and its own normals. Averaging
      // them into one quad normal is what makes a low-segment cylinder shade
      // like a flat ribbon rather than a tube.
      v(arr, p0, n0, col, m); v(arr, p1, n1, col, m); v(arr, q1, n1, col, m);
      v(arr, p0, n0, col, m); v(arr, q1, n1, col, m); v(arr, q0, n0, col, m);
      if (caps) {
        v(arr, [x, y + h, z], [0, 1, 0], col, m); v(arr, q0, n0, col, m); v(arr, q1, n1, col, m);
        v(arr, [x, y, z], [0, -1, 0], col, m); v(arr, p1, n1, col, m); v(arr, p0, n0, col, m);
      }
    }
  };

  /** A cone, for street furniture caps and tree tops. */
  const cone = (arr, x, y, z, r, h, col, m, segments = 8) => {
    const apex = [x, y + h, z];
    // A cone's side normal tilts back from the radial direction by the
    // semi-vertical angle, so it is (radial, rise/radius). It has to be
    // normalised: an un-normalised normal is still "valid" enough for a dot
    // product to run, which means the surface silently renders too bright
    // rather than failing. Every cone in the world was wrong by a factor of
    // sqrt(h^2 + r^2) before this was caught.
    const nl = Math.hypot(h, r) || 1;
    for (let i = 0; i < segments; i++) {
      const a = (i / segments) * Math.PI * 2, b = ((i + 1) / segments) * Math.PI * 2;
      const ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b);
      const p0 = [x + ca * r, y, z + sa * r], p1 = [x + cb * r, y, z + sb * r];
      const n0 = [(ca * h) / nl, r / nl, (sa * h) / nl], n1 = [(cb * h) / nl, r / nl, (sb * h) / nl];
      v(arr, p0, n0, col, m); v(arr, p1, n1, col, m); v(arr, apex, n0, col, m);
      v(arr, [x, y, z], [0, -1, 0], col, m); v(arr, p1, n1, col, m); v(arr, p0, n0, col, m);
    }
  };

  /**
   * A stacked-ring blob: the canopy of a tree, a bush, a cloud.
   *
   * Cheap, and cheap is the point — a canopy is the single most repeated solid
   * in a city, so it has to be both a few hundred triangles and shaped so that
   * no two of them look alike. The ring radii come from the caller's seed, so
   * variation is deterministic per instance.
   */
  const blob = (arr, x, y, z, radius, height, col, m, rand, rings = 4, segments = 7) => {
    const profile = [];
    for (let i = 0; i <= rings; i++) {
      const t = i / rings;
      // Wider low, narrower high, with a seeded wobble so no two silhouettes
      // are the same. `0.55 + 0.45*sin(pi*t)` is a hemisphere; the wobble is
      // what makes it a tree rather than a ball.
      const base = Math.sin(Math.PI * (0.18 + 0.82 * t));
      const wobble = 0.82 + 0.36 * rand();
      profile.push({ t, r: radius * base * wobble, y: y + height * t });
    }
    for (let i = 0; i < rings; i++) {
      const lo = profile[i], hi = profile[i + 1];
      for (let s = 0; s < segments; s++) {
        const a = (s / segments) * Math.PI * 2, b = ((s + 1) / segments) * Math.PI * 2;
        const p0 = [x + Math.cos(a) * lo.r, lo.y, z + Math.sin(a) * lo.r];
        const p1 = [x + Math.cos(b) * lo.r, lo.y, z + Math.sin(b) * lo.r];
        const q0 = [x + Math.cos(a) * hi.r, hi.y, z + Math.sin(a) * hi.r];
        const q1 = [x + Math.cos(b) * hi.r, hi.y, z + Math.sin(b) * hi.r];
        const shade = 0.82 + 0.30 * (i / rings);
        const cLo = [col[0] * shade, col[1] * shade, col[2] * shade];
        const cHi = [col[0] * (shade + 0.12), col[1] * (shade + 0.12), col[2] * (shade + 0.12)];
        v(arr, p0, nrm(p0, [x, (lo.y + hi.y) / 2, z]), cLo, m);
        v(arr, p1, nrm(p1, [x, (lo.y + hi.y) / 2, z]), cLo, m);
        v(arr, q1, nrm(q1, [x, (lo.y + hi.y) / 2, z]), cHi, m);
        v(arr, p0, nrm(p0, [x, (lo.y + hi.y) / 2, z]), cLo, m);
        v(arr, q1, nrm(q1, [x, (lo.y + hi.y) / 2, z]), cHi, m);
        v(arr, q0, nrm(q0, [x, (lo.y + hi.y) / 2, z]), cHi, m);
      }
    }
  };

  function nrm(p, c) {
    const dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    const l = Math.hypot(dx, dy, dz) || 1;
    return [dx / l, dy / l, dz / l];
  }

  /** A single horizontal quad, for road markings and decals. */
  const plane = (arr, x, y, z, w, d, col, m) => {
    const x0 = x - w / 2, x1 = x + w / 2, z0 = z - d / 2, z1 = z + d / 2;
    quad(arr, [x0, y, z0], [x1, y, z0], [x1, y, z1], [x0, y, z1], [0, 1, 0], col, m);
  };

  /** A single vertical quad, for signs and billboards. */
  const panel = (arr, x, y, z, w, h, facing, col, m) => {
    const hw = w / 2, y0 = y, y1 = y + h;
    if (facing === 'z+') quad(arr, [x - hw, y0, z], [x + hw, y0, z], [x + hw, y1, z], [x - hw, y1, z], [0, 0, 1], col, m);
    else if (facing === 'z-') quad(arr, [x + hw, y0, z], [x - hw, y0, z], [x - hw, y1, z], [x + hw, y1, z], [0, 0, -1], col, m);
    else if (facing === 'x+') quad(arr, [x, y0, z + hw], [x, y0, z - hw], [x, y1, z - hw], [x, y1, z + hw], [1, 0, 0], col, m);
    else quad(arr, [x, y0, z - hw], [x, y0, z + hw], [x, y1, z + hw], [x, y1, z - hw], [-1, 0, 0], col, m);
  };

  // -- architectural detail -------------------------------------------------

  /**
   * A window: reveal, frame, glass and sill.
   *
   * The frame and sill are what make it read as an opening in a wall rather
   * than a rectangle painted on one. The glass is inset by 0.12 m, which is
   * roughly right, and the sill projects 0.09 m so it catches a shadow line.
   */
  const window = (arr, x, y, z, w, h, facing, rand) => {
    const mFrame = M('paint_metal');
    const mGlass = M('glass');
    const mSill = M('wall_stone');
    const frame = 0.07;
    const t = 0.16;
    // `facing` is the outward direction; the unit is built flat and then placed.
    const put = (ox, oy, sw, sh, depth, col, m) => {
      if (facing === 'z+' || facing === 'z-') box(arr, x + ox, y + oy, z, sw, sh, depth, col, m);
      else box(arr, x, y + oy, z + ox, depth, sh, sw, col, m);
    };
    // Reveal: a slightly larger, darker box behind the frame so the opening has
    // depth even before the glass is drawn.
    put(0, 0, w + frame * 2, h + frame * 2, t * 0.6, [0.08, 0.08, 0.09], M('metal_dark'));
    put(0, frame, w, h, t, [0.20, 0.26, 0.30], mGlass);
    // Frame bars: two verticals, two horizontals, and a transom.
    put(-w / 2 - frame / 2, frame / 2, frame, h + frame, t, [0.82, 0.80, 0.76], mFrame);
    put(w / 2 + frame / 2, frame / 2, frame, h + frame, t, [0.82, 0.80, 0.76], mFrame);
    put(0, -frame / 2, w + frame * 2, frame, t, [0.82, 0.80, 0.76], mFrame);
    put(0, h + frame / 2, w + frame * 2, frame, t, [0.82, 0.80, 0.76], mFrame);
    put(0, h * 0.62, w, frame * 0.8, t * 0.9, [0.82, 0.80, 0.76], mFrame);
    // Sill, projecting and slightly wider than the opening.
    put(0, -0.10, w + 0.30, 0.10, t + 0.12, [0.68, 0.66, 0.62], mSill);
    // A shutter on one side, on about a third of windows, because a facade
    // where every window is identical is the most obvious tell of a generator.
    if (rand() < 0.34) {
      const side = rand() < 0.5 ? -1 : 1;
      const sw = w * 0.5;
      const ox = side * (w / 2 + sw / 2 + 0.04);
      put(ox, frame, sw, h, t * 0.7, [0.30 + 0.18 * rand(), 0.34, 0.30], M('paint_metal'));
    }
  };

  /** A doorway with a reveal, a step and, sometimes, a small canopy. */
  const door = (arr, x, y, z, w, h, facing, rand, awning) => {
    const dir = facing === 'z+' || facing === 'z-' ? 'z' : 'x';
    const t = 0.2;
    const put = (ox, oy, sw, sh, depth, col, m) => {
      if (dir === 'z') box(arr, x + ox, y + oy, z, sw, sh, depth, col, m);
      else box(arr, x, y + oy, z + ox, depth, sh, sw, col, m);
    };
    put(0, 0, w + 0.26, h + 0.14, t * 0.5, [0.10, 0.10, 0.11], M('metal_dark'));
    put(0, 0.02, w, h, t, [0.26, 0.16, 0.11], M('paint_metal'));
    put(0, 0.02, w * 0.34, h * 0.9, t * 0.6, [0.34, 0.22, 0.15], M('paint_metal'));
    put(0, -0.06, w + 0.5, 0.12, t + 0.5, [0.62, 0.60, 0.56], M('wall_concrete'));
    if (awning) {
      // A shallow sloping canopy: three slabs, angled, with a fascia.
      const aw = w * 2.1, ad = 1.1;
      const steps = 3;
      for (let i = 0; i < steps; i++) {
        const oy = h + 0.34 - i * 0.11;
        const oo = (i + 0.5) * (ad / steps) * (dir === 'z' ? 1 : 1);
        const c = i % 2 === 0 ? [0.72, 0.18, 0.16] : [0.86, 0.84, 0.80];
        if (dir === 'z') {
          box(arr, x, oy, z + 0.2 + oo, aw, 0.10, ad / steps + 0.02, c, M('tarpaulin'));
        } else {
          box(arr, x + 0.2 + oo, oy, z, ad / steps + 0.02, 0.10, aw, c, M('tarpaulin'));
        }
      }
      if (dir === 'z') box(arr, x, h + 0.30, z + 0.2 + ad, aw, 0.26, 0.08, [0.72, 0.18, 0.16], M('tarpaulin'));
      else box(arr, x + 0.2 + ad, h + 0.30, z, 0.08, 0.26, aw, [0.72, 0.18, 0.16], M('tarpaulin'));
    }
  };

  /**
   * A projecting band that runs around a building.
   *
   * Cornices, string courses and plinths are all this: a horizontal element
   * that projects past the wall. It casts a shadow line on the facade below it,
   * and that shadow line is most of what stops a tall box reading as a tall
   * box.
   */
  const band = (arr, x, y, z, w, d, project, height, col, m) => {
    box(arr, x, y, z, w + project * 2, height, d + project * 2, col, m);
    // The underside is what reads: without it the band floats.
    box(arr, x, y - 0.05, z, w + project, 0.05, d + project, [col[0] * 0.6, col[1] * 0.6, col[2] * 0.6], m);
  };

  /** A parapet wall around a flat roof, with a coping stone on top. */
  const parapet = (arr, x, y, z, w, d, height, col, m) => {
    const t = 0.28;
    const parts = [
      [x, y, z - d / 2 + t / 2, w, t],
      [x, y, z + d / 2 - t / 2, w, t],
      [x - w / 2 + t / 2, y, z, t, d],
      [x + w / 2 - t / 2, y, z, t, d]
    ];
    for (const [px, py, pz, pw, pd] of parts) box(arr, px, py, pz, pw, height, pd, col, m);
    // Coping: a slightly wider cap so the wall has a top edge to catch light.
    const c = [Math.min(1, col[0] * 1.15 + 0.05), Math.min(1, col[1] * 1.15 + 0.05), Math.min(1, col[2] * 1.15 + 0.05)];
    for (const [px, py, pz, pw, pd] of parts) box(arr, px, py + height, pz, pw + 0.12, 0.09, pd + 0.12, c, m);
  };

  /** A pitched roof, as a prism. */
  const gableRoof = (arr, x, y, z, w, d, h, col, m, overhang = 0.45) => {
    const x0 = x - w / 2 - overhang, x1 = x + w / 2 + overhang;
    const z0 = z - d / 2 - overhang, z1 = z + d / 2 + overhang;
    const ridgeA = [x, y + h, z0], ridgeB = [x, y + h, z1];
    // 1/sqrt(2): a 45-degree roof plane. Written as 0.7 it is 1% short of unit,
    // which is the sort of thing that never looks wrong in isolation and makes
    // every roof in the city a hair too bright.
    const k = Math.SQRT1_2;
    quad(arr, [x0, y, z0], [x1, y, z0], ridgeB, ridgeA, [0, k, -k], col, m);
    quad(arr, [x1, y, z1], [x0, y, z1], ridgeA, ridgeB, [0, k, k], col, m);
    // Gable ends.
    quad(arr, [x0, y, z1], [x1, y, z1], ridgeB, ridgeB, [0, 0, 1], col, m);
    v(arr, [x0, y, z1], [0, 0, 1], col, m); v(arr, [x1, y, z1], [0, 0, 1], col, m); v(arr, [x, y + h, z1], [0, 0, 1], col, m);
    v(arr, [x0, y, z0], [0, 0, -1], col, m); v(arr, [x1, y, z0], [0, 0, -1], col, m); v(arr, [x, y + h, z0], [0, 0, -1], col, m);
    box(arr, x, y - 0.1, z, w, 0.12, d, [col[0] * 0.5, col[1] * 0.5, col[2] * 0.5], m);
  };

  /** A railing: posts, a top rail and a mid rail. */
  const railing = (arr, x, y, z, length, height, axis, col, m, spacing = 1.1) => {
    const posts = Math.max(2, Math.round(length / spacing));
    for (let i = 0; i <= posts; i++) {
      const o = -length / 2 + (length * i) / posts;
      if (axis === 'x') box(arr, x + o, y, z, 0.05, height, 0.05, col, m);
      else box(arr, x, y, z + o, 0.05, height, 0.05, col, m);
    }
    const rails = [[height - 0.04, 0.06], [height * 0.52, 0.04]];
    for (const [oy, t] of rails) {
      if (axis === 'x') box(arr, x, y + oy, z, length, t, t, col, m);
      else box(arr, x, y + oy, z, t, t, length, col, m);
    }
  };

  /** A projecting balcony: slab, railing, and two brackets. */
  const balcony = (arr, x, y, z, w, d, col, m) => {
    box(arr, x, y, z, w, 0.14, d, col, m);
    box(arr, x, y - 0.16, z, w * 0.9, 0.16, d * 0.8, [col[0] * 0.7, col[1] * 0.7, col[2] * 0.7], m);
    railing(arr, x, y + 0.14, z + d / 2 - 0.06, w, 1.05, 'x', [0.28, 0.28, 0.30], M('paint_metal'));
    railing(arr, x - w / 2 + 0.06, y + 0.14, z, d - 0.12, 1.05, 'z', [0.28, 0.28, 0.30], M('paint_metal'));
    railing(arr, x + w / 2 - 0.06, y + 0.14, z, d - 0.12, 1.05, 'z', [0.28, 0.28, 0.30], M('paint_metal'));
  };

  /** A run of stairs, each step a real solid. */
  const stairs = (arr, x, y, z, steps, width, rise, run, col, m) => {
    for (let i = 0; i < steps; i++) {
      box(arr, x, y + i * rise, z - i * run, width, rise, run, col, m);
    }
  };

  /** A rooftop air-conditioning unit: housing, grille bars, a fan cowl. */
  const acUnit = (arr, x, y, z, w, d, h, rand) => {
    box(arr, x, y, z, w, h, d, [0.55, 0.56, 0.55], M('paint_metal'));
    // Grille: a row of thin bars on the long face.
    const bars = Math.max(3, Math.round(w / 0.14));
    for (let i = 0; i < bars; i++) {
      const ox = -w / 2 + (w / bars) * (i + 0.5);
      box(arr, x + ox, y + h * 0.18, z - d / 2 - 0.02, 0.05, h * 0.62, 0.03, [0.20, 0.21, 0.21], M('metal_dark'));
    }
    box(arr, x, y + h, z, w * 0.9, 0.05, d * 0.9, [0.30, 0.31, 0.31], M('metal_dark'));
    cylinder(arr, x, y + h + 0.05, z, w * 0.3, 0.10, [0.24, 0.25, 0.25], M('metal_dark'), 8);
  };

  /** A vent stack: a pipe with a cowl. */
  const vent = (arr, x, y, z, h, r) => {
    cylinder(arr, x, y, z, r, h, [0.48, 0.49, 0.48], M('metal_bare'), 7);
    cylinder(arr, x, y + h, z, r * 1.9, 0.10, [0.36, 0.37, 0.36], M('metal_bare'), 7);
  };

  /** A chimney stack. */
  const chimney = (arr, x, y, z, w, d, h, col, m) => {
    box(arr, x, y, z, w, h, d, col, m);
    box(arr, x, y + h, z, w + 0.16, 0.14, d + 0.16, [col[0] * 0.8, col[1] * 0.8, col[2] * 0.8], m);
    for (const ox of [-w * 0.22, w * 0.22]) {
      cylinder(arr, x + ox, y + h + 0.14, z, 0.10, 0.28, [0.18, 0.16, 0.15], M('metal_dark'), 6);
    }
  };

  /** A water tank on a roof: legs, a cylinder, a lid. */
  const roofTank = (arr, x, y, z, r, h) => {
    for (const [ox, oz] of [[-r * 0.6, -r * 0.6], [r * 0.6, -r * 0.6], [-r * 0.6, r * 0.6], [r * 0.6, r * 0.6]]) {
      box(arr, x + ox, y, z + oz, 0.09, 0.9, 0.09, [0.3, 0.3, 0.3], M('metal_dark'));
    }
    cylinder(arr, x, y + 0.9, z, r, h, [0.55, 0.52, 0.46], M('paint_metal'), 10);
    cone(arr, x, y + 0.9 + h, z, r * 1.04, r * 0.5, [0.42, 0.40, 0.36], M('paint_metal'), 10);
  };

  /** A run of pipework along a wall, with brackets. */
  const pipeRun = (arr, x, y, z, length, axis, col, m, brackets = 3) => {
    const along = axis === 'x' ? [length, 0.09, 0.09] : [0.09, 0.09, length];
    box(arr, x, y, z, along[0], along[1], along[2], col, m);
    for (let i = 0; i < brackets; i++) {
      const t = (length * (i + 0.5)) / brackets - length / 2;
      if (axis === 'x') box(arr, x + t, y - 0.02, z, 0.05, 0.13, 0.16, [0.3, 0.3, 0.3], M('metal_dark'));
      else box(arr, x, y - 0.02, z + t, 0.16, 0.13, 0.05, [0.3, 0.3, 0.3], M('metal_dark'));
    }
  };

  /** A fire escape: landings, ladders and rails, on the rear of a block. */
  const fireEscape = (arr, x, y, z, w, levels, floorH, facing) => {
    const dir = facing === 'x' ? 'x' : 'z';
    for (let l = 0; l < levels; l++) {
      const ly = y + l * floorH;
      if (dir === 'z') box(arr, x, ly, z + w / 2, 1.3, 0.09, w, [0.32, 0.32, 0.33], M('metal_dark'));
      else box(arr, x + w / 2, ly, z, w, 0.09, 1.3, [0.32, 0.32, 0.33], M('metal_dark'));
      // Ladder to the next level.
      const lx = dir === 'z' ? x - 0.45 : x + w - 0.45;
      const lz = dir === 'z' ? z + w - 0.5 : z - 0.45;
      box(arr, lx, ly, lz, 0.06, floorH, 0.06, [0.35, 0.35, 0.35], M('metal_dark'));
      box(arr, lx + 0.3, ly, lz, 0.06, floorH, 0.06, [0.35, 0.35, 0.35], M('metal_dark'));
      for (let r = 0; r < 5; r++) {
        box(arr, lx + 0.15, ly + 0.25 + r * (floorH / 5.5), lz, 0.34, 0.04, 0.04, [0.35, 0.35, 0.35], M('metal_dark'));
      }
    }
  };

  /** A shop or trade sign board, with a frame and a bracket. */
  const signBoard = (arr, x, y, z, w, h, facing, lit) => {
    const m = lit ? M('sign') : M('paint_metal');
    const col = lit ? [0.9, 0.35, 0.22] : [0.20, 0.22, 0.24];
    panel(arr, x, y, z, w, h, facing, col, m);
    const t = 0.07;
    if (facing === 'z+' || facing === 'z-') {
      box(arr, x, y - t, z, w + t, t, t, [0.14, 0.14, 0.15], M('metal_dark'));
      box(arr, x, y + h, z, w + t, t, t, [0.14, 0.14, 0.15], M('metal_dark'));
    } else {
      box(arr, x, y - t, z, t, t, w + t, [0.14, 0.14, 0.15], M('metal_dark'));
      box(arr, x, y + h, z, t, t, w + t, [0.14, 0.14, 0.15], M('metal_dark'));
    }
  };

  /** A vertical blade sign, perpendicular to the facade. */
  const bladeSign = (arr, x, y, z, h, facing, col) => {
    box(arr, x, y, z, 0.10, h, 0.9, col, M('sign'));
    box(arr, x, y + h * 0.5, z, 0.5, 0.06, 0.06, [0.2, 0.2, 0.2], M('metal_dark'));
  };

  /** A roller shutter, closed. Corrugation comes from the material's normal. */
  const shutter = (arr, x, y, z, w, h, facing, col) => {
    const put = (oy, sh) => {
      if (facing === 'z+' || facing === 'z-') box(arr, x, y + oy, z, w, sh, 0.10, col, M('wall_metal_shutter'));
      else box(arr, x, y + oy, z, 0.10, sh, w, col, M('wall_metal_shutter'));
    };
    put(0, h);
    put(-0.12, 0.12);
  };

  // -- street furniture -----------------------------------------------------

  /** A street lamp: base, tapered column, bracket arm, luminaire. */
  const streetLamp = (arr, x, y, z, h, facing, rand) => {
    const m = M('paint_metal');
    cylinder(arr, x, y, z, 0.22, 0.35, [0.35, 0.35, 0.36], M('metal_dark'), 8);
    taper(arr, x, y + 0.35, z, 0.30, 0.30, 0.13, 0.13, h - 0.6, [0.42, 0.43, 0.44], m);
    // Arm, curving out over the carriageway.
    const steps = 4;
    for (let i = 0; i < steps; i++) {
      const t = i / steps;
      const o = t * 1.5;
      const oy = y + h - 0.25 + Math.sin(t * Math.PI * 0.5) * 0.45;
      box(arr, x, oy, z + o, 0.11, 0.11, 1.5 / steps + 0.06, [0.42, 0.43, 0.44], m);
    }
    box(arr, x, y + h + 0.16, z + 1.5, 0.30, 0.16, 0.72, [0.45, 0.46, 0.47], m);
    box(arr, x, y + h + 0.08, z + 1.5, 0.24, 0.09, 0.62, [1.0, 0.94, 0.78], M('lamp'));
  };

  /** A traffic light: pole, head, three lenses and a hood over each. */
  const trafficLight = (arr, x, y, z, h, facing) => {
    const m = M('paint_metal');
    cylinder(arr, x, y, z, 0.16, 0.25, [0.3, 0.3, 0.3], M('metal_dark'), 8);
    cylinder(arr, x, y + 0.25, z, 0.11, h, [0.36, 0.38, 0.34], m, 7);
    const hx = facing === 'x' ? 0.16 : 0;
    const hz = facing === 'x' ? 0 : 0.16;
    box(arr, x + hx, y + h - 0.1, z + hz, 0.34, 1.05, 0.30, [0.16, 0.17, 0.16], M('metal_dark'));
    const lensCols = [[0.95, 0.12, 0.10], [0.95, 0.72, 0.10], [0.15, 0.85, 0.25]];
    for (let i = 0; i < 3; i++) {
      const ly = y + h + 0.62 - i * 0.32;
      const lx = facing === 'x' ? x + 0.18 : x;
      const lz = facing === 'x' ? z : z + 0.18;
      cylinder(arr, lx, ly, lz, 0.10, 0.04, lensCols[i], M('glass'), 7, true);
      // Hood.
      box(arr, lx, ly + 0.08, lz, 0.26, 0.04, 0.26, [0.12, 0.12, 0.12], M('metal_dark'));
    }
  };

  const bollard = (arr, x, y, z) => {
    cylinder(arr, x, y, z, 0.11, 0.95, [0.24, 0.25, 0.26], M('paint_metal'), 7);
    cylinder(arr, x, y + 0.95, z, 0.12, 0.06, [0.85, 0.84, 0.80], M('road_paint'), 7);
  };

  const bench = (arr, x, y, z, facing) => {
    const timber = [0.52, 0.42, 0.30];
    const steel = [0.24, 0.25, 0.26];
    for (const ox of [-0.75, 0.75]) {
      if (facing === 'z') box(arr, x + ox, y, z, 0.09, 0.44, 0.55, steel, M('metal_dark'));
      else box(arr, x, y, z + ox, 0.55, 0.44, 0.09, steel, M('metal_dark'));
    }
    const slats = 4;
    for (let i = 0; i < slats; i++) {
      const o = -0.21 + i * 0.14;
      if (facing === 'z') box(arr, x, y + 0.44, z + o, 1.75, 0.05, 0.11, timber, M('wall_siding'));
      else box(arr, x + o, y + 0.44, z, 0.11, 0.05, 1.75, timber, M('wall_siding'));
    }
    // Back rest, leaning back.
    for (let i = 0; i < 3; i++) {
      const oy = 0.55 + i * 0.16;
      const o = -0.26 - i * 0.03;
      if (facing === 'z') box(arr, x, y + oy, z + o, 1.75, 0.09, 0.05, timber, M('wall_siding'));
      else box(arr, x + o, y + oy, z, 0.05, 0.09, 1.75, timber, M('wall_siding'));
    }
  };

  const bin = (arr, x, y, z, rand) => {
    const c = rand() < 0.4 ? [0.18, 0.30, 0.20] : [0.24, 0.25, 0.26];
    taper(arr, x, y, z, 0.52, 0.52, 0.58, 0.58, 0.86, c, M('paint_metal'));
    cylinder(arr, x, y + 0.86, z, 0.30, 0.10, [0.14, 0.15, 0.15], M('metal_dark'), 8);
  };

  const dumpster = (arr, x, y, z, facing) => {
    const w = 1.9, d = 1.15, h = 1.15;
    const w2 = facing === 'z' ? w : d;
    const d2 = facing === 'z' ? d : w;
    taper(arr, x, y, z, w2 * 0.86, d2 * 0.86, w2, d2, h, [0.20, 0.30, 0.26], M('paint_metal'));
    box(arr, x, y + h, z, w2 * 1.02, 0.07, d2 * 1.02, [0.16, 0.24, 0.21], M('paint_metal'));
    // Castors.
    for (const [ox, oz] of [[-w2 * 0.38, -d2 * 0.38], [w2 * 0.38, -d2 * 0.38], [-w2 * 0.38, d2 * 0.38], [w2 * 0.38, d2 * 0.38]]) {
      cylinder(arr, x + ox - 0.07, y - 0.14, z + oz, 0.11, 0.14, [0.1, 0.1, 0.1], M('rubber'), 6);
    }
  };

  const hydrant = (arr, x, y, z) => {
    const m = M('paint_metal');
    const c = [0.72, 0.14, 0.10];
    cylinder(arr, x, y, z, 0.20, 0.10, [0.3, 0.3, 0.3], M('metal_dark'), 8);
    cylinder(arr, x, y + 0.10, z, 0.15, 0.62, c, m, 8);
    cylinder(arr, x, y + 0.72, z, 0.19, 0.14, c, m, 8);
    cone(arr, x, y + 0.86, z, 0.12, 0.12, c, m, 8);
    cylinder(arr, x - 0.19, y + 0.40, z, 0.08, 0.10, c, m, 6);
  };

  const mailbox = (arr, x, y, z, facing) => {
    const c = [0.18, 0.32, 0.52];
    if (facing === 'z') {
      box(arr, x, y + 0.30, z, 0.10, 0.60, 0.10, [0.2, 0.2, 0.2], M('metal_dark'));
      box(arr, x, y + 0.85, z, 0.52, 0.72, 0.62, c, M('paint_metal'));
      box(arr, x, y + 1.57, z, 0.56, 0.10, 0.66, [0.14, 0.22, 0.36], M('paint_metal'));
    } else {
      box(arr, x, y + 0.30, z, 0.10, 0.60, 0.10, [0.2, 0.2, 0.2], M('metal_dark'));
      box(arr, x, y + 0.85, z, 0.62, 0.72, 0.52, c, M('paint_metal'));
      box(arr, x, y + 1.57, z, 0.66, 0.10, 0.56, [0.14, 0.22, 0.36], M('paint_metal'));
    }
  };

  const busShelter = (arr, x, y, z, len, facing) => {
    const m = M('paint_metal');
    const frame = [0.40, 0.42, 0.44];
    const w = facing === 'z' ? len : 1.5;
    const d = facing === 'z' ? 1.5 : len;
    for (const [ox, oz] of [[-w / 2 + 0.1, -d / 2 + 0.1], [w / 2 - 0.1, -d / 2 + 0.1], [-w / 2 + 0.1, d / 2 - 0.1], [w / 2 - 0.1, d / 2 - 0.1]]) {
      box(arr, x + ox, y, z + oz, 0.09, 2.5, 0.09, frame, m);
    }
    box(arr, x, y + 2.5, z, w + 0.3, 0.10, d + 0.3, frame, m);
    // A translucent roof panel over the frame, and a rear wall, so a bus shelter
    // is a roof on four legs rather than four legs.
    panel(arr, x, y + 2.60, z, w + 0.3, d + 0.3, 'y+', [0.55, 0.62, 0.66], M('glass'));
    // Rear and end glazing.
    if (facing === 'z') {
      panel(arr, x, y + 0.5, z - d / 2, w, 2.0, 'z-', [0.6, 0.7, 0.78], M('glass'));
      panel(arr, x - w / 2, y + 0.5, z, d, 2.0, 'x-', [0.6, 0.7, 0.78], M('glass'));
    } else {
      panel(arr, x - w / 2, y + 0.5, z, d, 2.0, 'x-', [0.6, 0.7, 0.78], M('glass'));
      panel(arr, x, y + 0.5, z - d / 2, w, 2.0, 'z-', [0.6, 0.7, 0.78], M('glass'));
    }
    bench(arr, facing === 'z' ? x : x - 0.3, y, facing === 'z' ? z - 0.4 : z, facing === 'z' ? 'x' : 'z');
  };

  /** A utility cabinet, the small grey box on every wall in a real city. */
  const utilityBox = (arr, x, y, z, w, h, d) => {
    box(arr, x, y, z, w, h, d, [0.42, 0.44, 0.43], M('paint_metal'));
    box(arr, x, y + h * 0.3, z, w * 0.9, 0.04, d + 0.02, [0.20, 0.21, 0.21], M('metal_dark'));
    box(arr, x + w * 0.3, y + h * 0.55, z, 0.06, 0.10, d + 0.03, [0.14, 0.15, 0.15], M('metal_dark'));
  };

  /**
   * A shipping container: corrugated sides, corner castings, doors.
   *
   * Corrugation is the material's normal map rather than geometry, which is why
   * it is `wall_corrugated` and not a stack of ribs — at the distance a
   * container is seen, the normal is the whole effect and the ribs would be
   * 400 triangles a player never looks at directly.
   */
  const container = (arr, x, y, z, len, facing, rand) => {
    const w = 2.44, h = 2.59, d = len;
    const ww = facing === 'z' ? d : w;
    const dd = facing === 'z' ? w : d;
    const hue = rand();
    const col = hue < 0.3 ? [0.55, 0.26, 0.18] : hue < 0.55 ? [0.20, 0.34, 0.40] : hue < 0.8 ? [0.36, 0.38, 0.34] : [0.45, 0.40, 0.20];
    box(arr, x, y + 0.12, z, ww, h - 0.12, dd, col, M('wall_corrugated'));
    // Corner castings.
    for (const ox of [-ww / 2, ww / 2]) for (const oz of [-dd / 2, dd / 2]) {
      box(arr, x + ox * 0.96, y, z + oz * 0.96, 0.22, 0.22, 0.22, [0.2, 0.2, 0.2], M('metal_dark'));
      box(arr, x + ox * 0.96, y + h - 0.22, z + oz * 0.96, 0.22, 0.22, 0.22, [0.2, 0.2, 0.2], M('metal_dark'));
    }
    // Doors on one end.
    const endX = facing === 'z' ? x : x + ww / 2 - 0.05;
    const endZ = facing === 'z' ? x === 0 ? z : z + dd / 2 - 0.05 : z;
    if (facing === 'z') {
      panel(arr, x, y + 0.2, z + dd / 2, ww * 0.46, h - 0.5, 'z+', [col[0] * 0.92, col[1] * 0.92, col[2] * 0.92], M('wall_corrugated'));
      panel(arr, x, y + 0.2, z - dd / 2, ww * 0.46, h - 0.5, 'z-', [col[0] * 0.92, col[1] * 0.92, col[2] * 0.92], M('wall_corrugated'));
    } else {
      panel(arr, x + ww / 2, y + 0.2, z, dd * 0.46, h - 0.5, 'x+', [col[0] * 0.92, col[1] * 0.92, col[2] * 0.92], M('wall_corrugated'));
      panel(arr, x - ww / 2, y + 0.2, z, dd * 0.46, h - 0.5, 'x-', [col[0] * 0.92, col[1] * 0.92, col[2] * 0.92], M('wall_corrugated'));
    }
  };

  /** A pallet: three bearers and seven deck boards. */
  const pallet = (arr, x, y, z) => {
    const timber = [0.55, 0.45, 0.32];
    for (const oz of [-0.5, 0, 0.5]) box(arr, x, y, z + oz, 1.2, 0.09, 0.12, timber, M('wall_siding'));
    for (let i = 0; i < 6; i++) box(arr, x - 0.5 + i * 0.2, y + 0.09, z, 0.11, 0.03, 1.2, timber, M('wall_siding'));
  };

  const crate = (arr, x, y, z, s, rand) => {
    const t = 0.55 + 0.5 * rand();
    const c = [0.46 * t, 0.36 * t, 0.24 * t];
    box(arr, x, y, z, s, s * 0.8, s, c, M('wall_siding'));
    // Banding, which is what makes a crate read as a crate and not a cube.
    box(arr, x, y + s * 0.3, z, s + 0.02, 0.03, s + 0.02, [0.3, 0.3, 0.3], M('metal_dark'));
    box(arr, x, y + s * 0.6, z, s + 0.02, 0.03, s + 0.02, [0.3, 0.3, 0.3], M('metal_dark'));
  };

  const trafficCone = (arr, x, y, z) => {
    box(arr, x, y, z, 0.36, 0.04, 0.36, [0.16, 0.16, 0.16], M('plastic'));
    cone(arr, x, y + 0.04, z, 0.15, 0.62, [0.90, 0.32, 0.06], M('plastic'), 7);
    cylinder(arr, x, y + 0.34, z, 0.11, 0.10, [0.92, 0.92, 0.90], M('road_paint'), 7);
  };

  /** A manhole cover, set flush into the road. */
  const manhole = (arr, x, y, z) => {
    cylinder(arr, x, y, z, 0.36, 0.035, [0.30, 0.29, 0.27], M('metal_dark'), 10);
    cylinder(arr, x, y + 0.035, z, 0.30, 0.02, [0.22, 0.21, 0.20], M('metal_dark'), 10);
  };

  return {
    M, v, vu, quad, quadShaded, triUV, box, taper, cylinder, cone, blob, plane, panel,
    window, door, band, parapet, gableRoof, railing, balcony, stairs,
    acUnit, vent, chimney, roofTank, pipeRun, fireEscape,
    signBoard, bladeSign, shutter,
    streetLamp, trafficLight, bollard, bench, bin, dumpster, hydrant, mailbox,
    busShelter, utilityBox, container, pallet, crate, trafficCone, manhole
  };
}
