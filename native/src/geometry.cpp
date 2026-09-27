// Native geometry emitters. See `geometry.hpp`.

#include "emergent/geometry.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace emergent {
namespace {

inline void growBounds(bool &has, float mn[3], float mx[3], float x, float y, float z) {
    if (!has) {
        mn[0] = mx[0] = x;
        mn[1] = mx[1] = y;
        mn[2] = mx[2] = z;
        has = true;
        return;
    }
    if (x < mn[0]) mn[0] = x;
    if (y < mn[1]) mn[1] = y;
    if (z < mn[2]) mn[2] = z;
    if (x > mx[0]) mx[0] = x;
    if (y > mx[1]) mx[1] = y;
    if (z > mx[2]) mx[2] = z;
}

}  // namespace

void MeshBuilder::vertex(float px, float py, float pz, float nx, float ny, float nz, float r, float g, float b,
                         int32_t material, float u, float v) {
    // The undefined check is not paranoia. A material index passed into the
    // colour slot produces vertices that are *finite* and therefore pass every
    // NaN scan, then draw as garbage. It happened twice while the JavaScript
    // kit was written, and the only reason it was caught quickly is that this
    // is the single choke point every emitter goes through.
    if (!(r >= 0.0f && r <= 1.0f) || !(g >= 0.0f && g <= 1.0f) || !(b >= 0.0f && b <= 1.0f) ||
        material < 0 || material >= materials_->count()) {
        std::fprintf(stderr,
                     "geometry: a vertex was emitted with colour (%g, %g, %g) or material %d, which the "
                     "table does not cover\n",
                     r, g, b, material);
        std::abort();
    }
    const float vals[kFloatsPerVertex] = {px, py, pz, nx, ny, nz, r, g, b,
                                          static_cast<float>(material), u, v};
    for (int i = 0; i < kFloatsPerVertex; i++) {
        if (!std::isfinite(vals[i])) {
            std::fprintf(stderr, "geometry: a non-finite value reached the vertex buffer at component %d\n", i);
            std::abort();
        }
    }
    for (float f : vals) data_.push_back(f);
    growBounds(hasBounds_, min_, max_, px, py, pz);
}

void MeshBuilder::triangle(const float a[3], const float b[3], const float c[3], const float n[3], float r, float g,
                           float bl, int32_t material) {
    vertex(a[0], a[1], a[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(b[0], b[1], b[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(c[0], c[1], c[2], n[0], n[1], n[2], r, g, bl, material);
}

void MeshBuilder::quad(const float a[3], const float b[3], const float c[3], const float d[3], const float n[3],
                       float r, float g, float bl, int32_t material) {
    vertex(a[0], a[1], a[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(b[0], b[1], b[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(c[0], c[1], c[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(a[0], a[1], a[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(c[0], c[1], c[2], n[0], n[1], n[2], r, g, bl, material);
    vertex(d[0], d[1], d[2], n[0], n[1], n[2], r, g, bl, material);
}

void MeshBuilder::box(float x, float y, float z, float w, float h, float d, float r, float g, float b,
                      int32_t material) {
    // Bottom-anchored, which is what every emitter here assumes: a box at y is
    // a thing standing *on* y, not one centred on it. An earlier centred
    // convention made every kerb and every sill float or sink by half its depth.
    const float x0 = x - w * 0.5f, x1 = x + w * 0.5f;
    const float y0 = y, y1 = y + h;
    const float z0 = z - d * 0.5f, z1 = z + d * 0.5f;

    const float top[3] = {0, 1, 0};
    const float bottom[3] = {0, -1, 0};
    const float px[3] = {1, 0, 0};
    const float nx[3] = {-1, 0, 0};
    const float pz[3] = {0, 0, 1};
    const float nz[3] = {0, 0, -1};

    const float tA[3] = {x0, y1, z0}, tB[3] = {x1, y1, z0}, tC[3] = {x1, y1, z1}, tD[3] = {x0, y1, z1};
    const float bA[3] = {x0, y0, z1}, bB[3] = {x1, y0, z1}, bC[3] = {x1, y0, z0}, bD[3] = {x0, y0, z0};
    const float fA[3] = {x0, y0, z0}, fB[3] = {x1, y0, z0}, fC[3] = {x1, y1, z0}, fD[3] = {x0, y1, z0};
    const float kA[3] = {x1, y0, z1}, kB[3] = {x0, y0, z1}, kC[3] = {x0, y1, z1}, kD[3] = {x1, y1, z1};
    const float lA[3] = {x1, y0, z0}, lB[3] = {x1, y0, z1}, lC[3] = {x1, y1, z1}, lD[3] = {x1, y1, z0};
    const float rrA[3] = {x0, y0, z1}, rrB[3] = {x0, y0, z0}, rrC[3] = {x0, y1, z0}, rrD[3] = {x0, y1, z1};

    quad(tA, tB, tC, tD, top, r, g, b, material);
    quad(bA, bB, bC, bD, bottom, r, g, b, material);
    quad(fA, fB, fC, fD, nz, r, g, b, material);
    quad(kA, kB, kC, kD, pz, r, g, b, material);
    quad(lA, lB, lC, lD, px, r, g, b, material);
    quad(rrA, rrB, rrC, rrD, nx, r, g, b, material);
}

void MeshBuilder::cylinder(float x, float y, float z, float radius, float height, float r, float g, float b,
                          int32_t material, int segments) {
    if (segments < 3) segments = 3;
    for (int i = 0; i < segments; i++) {
        const float a0 = static_cast<float>(2.0 * M_PI * i / segments);
        const float a1 = static_cast<float>(2.0 * M_PI * (i + 1) / segments);
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        // The side normal is the radial direction, *normalised*. Emitting
        // (cos, sin) without dividing by the radius is a bug that looks like a
        // lighting change: a radius-5 cylinder's sides are lit at up to 5x the
        // correct brightness and nothing crashes.
        const float n0x = c0, n0z = s0;
        const float n1x = c1, n1z = s1;
        const float p00[3] = {x + c0 * radius, y, z + s0 * radius};
        const float p10[3] = {x + c1 * radius, y, z + s1 * radius};
        const float p01[3] = {x + c1 * radius, y + height, z + s1 * radius};
        const float p11[3] = {x + c0 * radius, y + height, z + s0 * radius};
        vertex(p00[0], p00[1], p00[2], n0x, 0, n0z, r, g, b, material);
        vertex(p10[0], p10[1], p10[2], n1x, 0, n1z, r, g, b, material);
        vertex(p01[0], p01[1], p01[2], n1x, 0, n1z, r, g, b, material);
        vertex(p00[0], p00[1], p00[2], n0x, 0, n0z, r, g, b, material);
        vertex(p01[0], p01[1], p01[2], n1x, 0, n1z, r, g, b, material);
        vertex(p11[0], p11[1], p11[2], n0x, 0, n0z, r, g, b, material);
    }
    // Caps. The winding is chosen so the top faces up and the bottom faces down;
    // a cap with the wrong winding is invisible from above, which is how a
    // missing top face presents itself.
    const float top[3] = {0, 1, 0};
    const float bot[3] = {0, -1, 0};
    const float c[3] = {x, y + height, z};
    const float cb[3] = {x, y, z};
    for (int i = 0; i < segments; i++) {
        const float a0 = static_cast<float>(2.0 * M_PI * i / segments);
        const float a1 = static_cast<float>(2.0 * M_PI * (i + 1) / segments);
        const float e0[3] = {x + std::cos(a0) * radius, y + height, z + std::sin(a0) * radius};
        const float e1[3] = {x + std::cos(a1) * radius, y + height, z + std::sin(a1) * radius};
        triangle(c, e1, e0, top, r, g, b, material);
        const float f0[3] = {x + std::cos(a0) * radius, y, z + std::sin(a0) * radius};
        const float f1[3] = {x + std::cos(a1) * radius, y, z + std::sin(a1) * radius};
        triangle(cb, f0, f1, bot, r, g, b, material);
    }
}

void MeshBuilder::cone(float x, float y, float z, float radius, float height, float r, float g, float b,
                      int32_t material, int segments) {
    if (segments < 3) segments = 3;
    const float slope = std::atan2(radius, height);
    const float ny = std::sin(slope);
    const float nr = std::cos(slope);
    const float apex[3] = {x, y + height, z};
    for (int i = 0; i < segments; i++) {
        const float a0 = static_cast<float>(2.0 * M_PI * i / segments);
        const float a1 = static_cast<float>(2.0 * M_PI * (i + 1) / segments);
        // Averaging the two edge normals gives the side normal of a *flat*
        // facet, which is what a low-segment cone actually is. Using a single
        // vertex normal instead makes a 6-sided cone look like a smooth one and
        // hides the faceting that gives it its shape.
        const float mx = (std::cos(a0) + std::cos(a1)) * 0.5f;
        const float mz = (std::sin(a0) + std::sin(a1)) * 0.5f;
        const float ml = std::sqrt(mx * mx + mz * mz);
        const float nx = ml > 1e-6f ? mx / ml * nr : 0.0f;
        const float nz = ml > 1e-6f ? mz / ml * nr : 0.0f;
        const float n[3] = {nx, ny, nz};
        const float e0[3] = {x + std::cos(a0) * radius, y, z + std::sin(a0) * radius};
        const float e1[3] = {x + std::cos(a1) * radius, y, z + std::sin(a1) * radius};
        triangle(e0, e1, apex, n, r, g, b, material);
    }
}

void MeshBuilder::plane(float x, float y, float z, float w, float d, float r, float g, float b, int32_t material) {
    const float n[3] = {0, 1, 0};
    const float a[3] = {x - w * 0.5f, y, z - d * 0.5f};
    const float c[3] = {x + w * 0.5f, y, z - d * 0.5f};
    const float e[3] = {x + w * 0.5f, y, z + d * 0.5f};
    const float f[3] = {x - w * 0.5f, y, z + d * 0.5f};
    quad(a, c, e, f, n, r, g, b, material);
}

void MeshBuilder::panel(float x, float y, float z, float w, float h, int axis, float r, float g, float b,
                        int32_t material) {
    float n[3] = {0, 0, 1};
    float a[3], b2[3], c[3], d2[3];
    if (axis == 0) {
        n[0] = 1; n[1] = 0; n[2] = 0;
        a[0] = x; a[1] = y; a[2] = z - w * 0.5f;
        b2[0] = x; b2[1] = y; b2[2] = z + w * 0.5f;
        c[0] = x; c[1] = y + h; c[2] = z + w * 0.5f;
        d2[0] = x; d2[1] = y + h; d2[2] = z - w * 0.5f;
    } else {
        a[0] = x - w * 0.5f; a[1] = y; a[2] = z;
        b2[0] = x + w * 0.5f; b2[1] = y; b2[2] = z;
        c[0] = x + w * 0.5f; c[1] = y + h; c[2] = z;
        d2[0] = x - w * 0.5f; d2[1] = y + h; d2[2] = z;
    }
    quad(a, b2, c, d2, n, r, g, b, material);
}

void MeshBuilder::bounds(float min[3], float max[3]) const {
    for (int i = 0; i < 3; i++) {
        min[i] = hasBounds_ ? min_[i] : 0.0f;
        max[i] = hasBounds_ ? max_[i] : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// The detail kit
// ---------------------------------------------------------------------------

void GeometryKit::box(MeshBuilder &b, float x, float y, float z, float w, float h, float d, const float col[3],
                      int32_t mat) const {
    b.box(x, y, z, w, h, d, col[0], col[1], col[2], mat);
}

void GeometryKit::window(MeshBuilder &b, float x, float y, float z, float w, float h, int axis) const {
    // A window is a reveal, a frame, a pane and a projecting sill. The sill is
    // what makes it read as an opening in a wall rather than a dark rectangle
    // painted on one.
    const int32_t frame = M("paint_metal");
    const int32_t glass = M("glass");
    const int32_t sill = M("wall_stone");
    const float reveal = 0.16f;
    const float t = 0.10f;
    if (axis == 0) {
        b.box(x, y, z - w * 0.5f - t, t, h + t * 2, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y, z + w * 0.5f + t, t, h + t * 2, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y + h + t, z, t, t, w + t * 2, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y - t, z, t, t, w + t * 2, 0.08f, 0.08f, 0.09f, frame);
        b.box(x + reveal * 0.5f, y, z, reveal, h, w, 0.16f, 0.24f, 0.30f, glass);
        b.box(x + reveal, y - 0.10f, z, 0.22f, 0.10f, w + 0.30f, 0.55f, 0.54f, 0.50f, sill);
    } else {
        b.box(x - w * 0.5f - t, y, z, t, h + t * 2, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x + w * 0.5f + t, y, z, t, h + t * 2, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y + h + t, z, w + t * 2, t, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y - t, z, w + t * 2, t, t, 0.08f, 0.08f, 0.09f, frame);
        b.box(x, y, z + reveal * 0.5f, w, h, reveal, 0.16f, 0.24f, 0.30f, glass);
        b.box(x, y - 0.10f, z + reveal, w + 0.30f, 0.10f, 0.22f, 0.55f, 0.54f, 0.50f, sill);
    }
}

void GeometryKit::door(MeshBuilder &b, float x, float y, float z, float w, float h, int axis) const {
    const int32_t leaf = M("paint_metal");
    const int32_t step = M("wall_stone");
    if (axis == 0) {
        b.box(x, y, z, 0.10f, h, w, 0.22f, 0.15f, 0.11f, leaf);
        b.box(x, y, z, 0.34f, 0.14f, w + 0.5f, 0.48f, 0.47f, 0.44f, step);
    } else {
        b.box(x, y, z, w, h, 0.10f, 0.22f, 0.15f, 0.11f, leaf);
        b.box(x, y, z, w + 0.5f, 0.14f, 0.34f, 0.48f, 0.47f, 0.44f, step);
    }
}

void GeometryKit::band(MeshBuilder &b, float x, float y, float z, float w, float d, float h, const float col[3],
                       int32_t mat) const {
    b.box(x, y, z, w, h, d, col[0], col[1], col[2], mat);
}

void GeometryKit::parapet(MeshBuilder &b, float x, float y, float z, float w, float d, const float col[3],
                          int32_t mat) const {
    b.box(x, y, z - d * 0.5f, w, 0.55f, 0.24f, col[0], col[1], col[2], mat);
    b.box(x, y, z + d * 0.5f, w, 0.55f, 0.24f, col[0], col[1], col[2], mat);
    b.box(x - w * 0.5f, y, z, 0.24f, 0.55f, d, col[0], col[1], col[2], mat);
    b.box(x + w * 0.5f, y, z, 0.24f, 0.55f, d, col[0], col[1], col[2], mat);
}

void GeometryKit::railing(MeshBuilder &b, float x, float y, float z, float len, float h, int axis,
                          const float col[3], int32_t mat) const {
    const float post = 0.05f;
    if (axis == 0) {
        b.box(x, y, z, len, post, post, col[0], col[1], col[2], mat);
        const int n = std::max(2, static_cast<int>(len / 1.2f));
        for (int i = 0; i <= n; i++) {
            const float px = x - len * 0.5f + len * i / n;
            b.box(px, y, z, post, h, post, col[0], col[1], col[2], mat);
        }
    } else {
        b.box(x, y, z, post, post, len, col[0], col[1], col[2], mat);
        const int n = std::max(2, static_cast<int>(len / 1.2f));
        for (int i = 0; i <= n; i++) {
            const float pz = z - len * 0.5f + len * i / n;
            b.box(x, y, pz, post, h, post, col[0], col[1], col[2], mat);
        }
    }
}

void GeometryKit::acUnit(MeshBuilder &b, float x, float y, float z) const {
    b.box(x, y, z, 0.9f, 0.7f, 0.9f, 0.62f, 0.62f, 0.60f, M("metal_bare"));
    b.cylinder(x, y + 0.7f, z, 0.28f, 0.10f, 0.30f, 0.30f, 0.32f, M("metal_dark"), 8);
}

void GeometryKit::pipeRun(MeshBuilder &b, float x, float y, float z, float len, float r, int axis) const {
    const int32_t mat = M("metal_rust");
    if (axis == 0) b.cylinder(x - len * 0.5f, y, z, r, len, 0.42f, 0.30f, 0.22f, mat, 6);
    else {
        // A vertical run: a horizontal cylinder would need a rotation the
        // emitter does not have, so it is built as a stack of thin boxes. Not
        // as elegant and completely solid.
        b.box(x, y, z, r * 2, len, r * 2, 0.42f, 0.30f, 0.22f, mat);
    }
}

void GeometryKit::streetLamp(MeshBuilder &b, float x, float y, float z, int facing) const {
    // 8 m, which is the height the detail kit's other measurements are scaled
    // against. The arm reaches over the carriageway, which is what makes it a
    // street lamp rather than a pole.
    const float h = 8.0f;
    b.cylinder(x, y, z, 0.12f, h, 0.20f, 0.21f, 0.22f, M("paint_metal"), 8);
    const float ax = facing == 0 ? 0.9f : 0.0f;
    const float az = facing == 0 ? 0.0f : 0.9f;
    b.box(x + ax * 0.5f, y + h - 0.12f, z + az * 0.5f, ax > 0 ? 1.0f : 0.10f, 0.10f, az > 0 ? 1.0f : 0.10f,
          0.20f, 0.21f, 0.22f, M("paint_metal"));
    b.box(x + ax, y + h - 0.24f, z + az, 0.44f, 0.14f, 0.30f, 0.10f, 0.10f, 0.11f, M("metal_dark"));
    b.box(x + ax, y + h - 0.34f, z + az, 0.36f, 0.10f, 0.24f, 1.0f, 0.94f, 0.80f, M("lamp"));
}

void GeometryKit::trafficLight(MeshBuilder &b, float x, float y, float z, int facing) const {
    b.cylinder(x, y, z, 0.10f, 3.4f, 0.18f, 0.19f, 0.20f, M("paint_metal"), 8);
    b.box(x, y + 3.4f, z, 0.26f, 0.90f, 0.24f, 0.10f, 0.11f, 0.12f, M("metal_dark"));
    const float fx = facing == 0 ? 0.16f : 0.0f;
    const float fz = facing == 0 ? 0.0f : 0.16f;
    b.box(x + fx, y + 3.5f, z + fz, fx > 0 ? 0.06f : 0.16f, 0.18f, fz > 0 ? 0.06f : 0.16f, 0.85f, 0.15f, 0.10f,
          M("sign"));
    b.box(x + fx, y + 3.72f, z + fz, fx > 0 ? 0.06f : 0.16f, 0.18f, fz > 0 ? 0.06f : 0.16f, 0.95f, 0.85f, 0.25f,
          M("lamp"));
}

void GeometryKit::bin(MeshBuilder &b, float x, float y, float z) const {
    b.cylinder(x, y, z, 0.28f, 0.85f, 0.20f, 0.24f, 0.20f, M("paint_metal"), 8);
    b.cylinder(x, y + 0.85f, z, 0.30f, 0.06f, 0.16f, 0.19f, 0.16f, M("metal_dark"), 8);
}

void GeometryKit::bench(MeshBuilder &b, float x, float y, float z, int facing) const {
    const int32_t wood = M("floor_wood");
    const int32_t steel = M("metal_dark");
    const float len = 1.8f;
    if (facing == 0) {
        for (int i = 0; i < 3; i++) b.box(x, y + 0.42f, z - 0.18f + i * 0.16f, len, 0.05f, 0.12f, 0.42f, 0.31f, 0.20f, wood);
        for (int i = 0; i < 3; i++) b.box(x, y + 0.50f + i * 0.16f, z + 0.24f, len, 0.12f, 0.05f, 0.42f, 0.31f, 0.20f, wood);
        b.box(x - len * 0.5f + 0.1f, y, z, 0.06f, 0.42f, 0.5f, 0.22f, 0.22f, 0.24f, steel);
        b.box(x + len * 0.5f - 0.1f, y, z, 0.06f, 0.42f, 0.5f, 0.22f, 0.22f, 0.24f, steel);
    } else {
        for (int i = 0; i < 3; i++) b.box(x - 0.18f + i * 0.16f, y + 0.42f, z, 0.12f, 0.05f, len, 0.42f, 0.31f, 0.20f, wood);
        for (int i = 0; i < 3; i++) b.box(x + 0.24f, y + 0.50f + i * 0.16f, z, 0.05f, 0.12f, len, 0.42f, 0.31f, 0.20f, wood);
        b.box(x, y, z - len * 0.5f + 0.1f, 0.5f, 0.42f, 0.06f, 0.22f, 0.22f, 0.24f, steel);
        b.box(x, y, z + len * 0.5f - 0.1f, 0.5f, 0.42f, 0.06f, 0.22f, 0.22f, 0.24f, steel);
    }
}

void GeometryKit::container(MeshBuilder &b, float x, float y, float z, int facing) const {
    // 6.06 x 2.44 x 2.59 m, which is a real shipping container and is why a
    // container reads as a container.
    const float w = facing == 0 ? 6.06f : 2.44f;
    const float d = facing == 0 ? 2.44f : 6.06f;
    const int32_t mat = M("wall_corrugated");
    b.box(x, y, z, w, 2.59f, d, 0.42f, 0.48f, 0.44f, mat);
    const int ribs = 9;
    for (int i = 0; i <= ribs; i++) {
        const float t = -0.5f + static_cast<float>(i) / ribs;
        if (facing == 0) b.box(x + t * w * 0.94f, y, z + d * 0.5f, 0.08f, 2.59f, 0.05f, 0.36f, 0.41f, 0.38f, mat);
        else b.box(x + w * 0.5f, y, z + t * d * 0.94f, 0.05f, 2.59f, 0.08f, 0.36f, 0.41f, 0.38f, mat);
    }
}

void GeometryKit::pallet(MeshBuilder &b, float x, float y, float z) const {
    const int32_t wood = M("floor_wood");
    for (int i = 0; i < 5; i++) b.box(x - 0.4f + i * 0.2f, y, z, 0.14f, 0.03f, 1.0f, 0.40f, 0.30f, 0.20f, wood);
    for (int i = 0; i < 3; i++) b.box(x, y, z - 0.4f + i * 0.4f, 1.0f, 0.10f, 0.12f, 0.36f, 0.27f, 0.18f, wood);
}

void GeometryKit::crate(MeshBuilder &b, float x, float y, float z, float s) const {
    b.box(x, y, z, s, s * 0.8f, s, 0.52f, 0.38f, 0.24f, M("floor_wood"));
    b.box(x, y + s * 0.8f, z, s * 0.98f, 0.04f, s * 0.98f, 0.44f, 0.32f, 0.20f, M("floor_wood"));
}

void GeometryKit::bollard(MeshBuilder &b, float x, float y, float z) const {
    b.cylinder(x, y, z, 0.09f, 0.95f, 0.20f, 0.21f, 0.22f, M("paint_metal"), 8);
    b.cylinder(x, y + 0.95f, z, 0.10f, 0.05f, 0.85f, 0.85f, 0.80f, M("road_paint"), 8);
}

void GeometryKit::signBoard(MeshBuilder &b, float x, float y, float z, float w, float h, int facing) const {
    b.box(x, y, z, facing == 0 ? 0.08f : w, h, facing == 0 ? w : 0.08f, 0.16f, 0.30f, 0.42f, M("sign"));
    b.box(x, y - 0.12f, z, 0.06f, 0.12f, 0.06f, 0.20f, 0.20f, 0.22f, M("metal_dark"));
}

}  // namespace emergent
