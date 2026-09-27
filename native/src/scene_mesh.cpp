#include "emergent/scene_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace emergent {
namespace {

/**
 * Material lookup that degrades instead of aborting.
 *
 * `MeshBuilder::vertex` aborts the process on an unknown material index, which
 * is the right behaviour for a programming error but the wrong one for
 * content: a table that is missing one facade material should render a city
 * with a plain wall, not take the game down. So the scene resolves every id
 * through here, and a miss falls back to a material that is always present.
 */
class Resolver {
public:
    Resolver(const MaterialTable &table, const GeometryKit &kit) : table_(table), kit_(kit) {}

    int32_t get(const char *id) {
        const auto it = cache_.find(id);
        if (it != cache_.end()) return it->second;
        int32_t index = -1;
        if (table_.byId.find(id) != table_.byId.end()) {
            index = table_.index(id);
        } else {
            ++misses_;
            index = table_.byId.count("paint_metal") ? table_.index("paint_metal") : 0;
        }
        cache_.emplace(id, index);
        return index;
    }

    uint32_t misses() const { return misses_; }

    const GeometryKit &kit() const { return kit_; }

private:
    const MaterialTable &table_;
    const GeometryKit &kit_;
    std::unordered_map<std::string, int32_t> cache_;
    uint32_t misses_ = 0;
};

/** A cheap deterministic per-building stream, so facades never reshuffle. */
struct DetailRng {
    uint32_t s;
    explicit DetailRng(uint32_t seed) : s(seed * 2654435761u + 1013904223u) { if (s == 0) s = 0x9e3779b9u; }
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() >> 8) / static_cast<float>(1 << 24); }
    float range(float lo, float hi) { return lo + unit() * (hi - lo); }
    int32_t pick(int32_t n) { return n > 0 ? static_cast<int32_t>(next() % static_cast<uint32_t>(n)) : 0; }
};

bool insideRadius(const SceneBuildOptions &o, double x, double z, double pad) {
    const double dx = x - o.centerX;
    const double dz = z - o.centerZ;
    const double r = o.radius + pad;
    return dx * dx + dz * dz <= r * r;
}

/**
 * Clip an axis-aligned rectangle to the slice box, in place.
 *
 * @return false when nothing survives.
 *
 * The slice is a circle, but the clip is a box. Clipping to the box rather than
 * the circle is a deliberate simplification: it bounds the geometry exactly
 * with four comparisons, and the corners it lets through are outside the
 * circular radius but inside the diagonal, which costs a thin sliver of
 * overdraw at the very edge of the stream and nothing else. Clipping to the
 * circle instead would need a polygon clip and would gain almost nothing.
 */
bool clipToSlice(float &x0, float &x1, float &z0, float &z1, const SceneBuildOptions &o) {
    const float lo = static_cast<float>(o.centerX - o.radius);
    const float hi = static_cast<float>(o.centerX + o.radius);
    const float lo2 = static_cast<float>(o.centerZ - o.radius);
    const float hi2 = static_cast<float>(o.centerZ + o.radius);
    x0 = std::max(x0, lo);
    x1 = std::min(x1, hi);
    z0 = std::max(z0, lo2);
    z1 = std::min(z1, hi2);
    return x1 > x0 && z1 > z0;
}

// Facade materials by archetype. These are the baked CC0 set, so a wall is a
// photograph of a real wall rather than a flat tint, and the whole point of
// carrying the bake across to the native renderer is that it is used here.
const char *facadeFor(const Building &b) {
    if (b.industrial) return "wall_corrugated";
    switch (b.kind) {
        case 0: return "wall_brick";   // shopfront terrace
        case 1: return "wall_stone";   // tower
        case 2: return "wall_corrugated";
        default: return "wall_plaster";  // house
    }
}

const float kStone[3] = {0.52f, 0.51f, 0.48f};
const float kPale[3] = {0.68f, 0.66f, 0.61f};
const float kDark[3] = {0.24f, 0.25f, 0.27f};

/** One building: mass, facade, and as much detail as the level allows. */
void emitBuilding(MeshBuilder &b, const Building &bd, const SceneBuildOptions &o, Resolver &res,
                  int32_t seed) {
    const GeometryKit &kit = res.kit();
    DetailRng rng(bd.rngState ^ 0x5bf03635u);

    const float w = static_cast<float>(bd.w);
    const float d = static_cast<float>(bd.d);
    const float h = static_cast<float>(bd.h);
    const float x = static_cast<float>(bd.x);
    const float z = static_cast<float>(bd.z);

    // Sit the building on the ground under its own footprint rather than at its
    // centre. On a slope those differ by enough to leave a floating corner or a
    // buried one, and a building with one corner in the dirt is the single most
    // common way a generated city looks procedurally generated.
    const float corner[4][2] = {{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    float ground = std::numeric_limits<float>::max();
    for (const auto &c : corner) {
        ground = std::min(ground, static_cast<float>(
                                        terrainHeight(bd.x + c[0] * bd.w, bd.z + c[1] * bd.d, seed)));
    }

    const int32_t mass = res.get(facadeFor(bd));

    // The mass. One box, and everything else sits on it.
    kit.box(b, x, ground, z, w, h, d, kStone, mass);

    const int32_t floors = std::max(1, bd.floors);
    const float floorHeight = h / static_cast<float>(floors);

    if (o.detailLevel >= 1) {
        // A plinth. Every real building has one, and its absence is why a box
        // reads as a box: the eye expects the base to be a different material.
        kit.box(b, x, ground, z, w + 0.30f, std::min(0.6f, floorHeight * 0.35f), d + 0.30f, kDark,
                res.get("pavement_concrete"));

        // A string course under the roofline, which is what stops a tall facade
        // reading as an unbroken slab.
        kit.band(b, x, ground + h - 0.45f, z, w + 0.18f, d + 0.18f, 0.30f, kPale, res.get("wall_stone"));

        // Windows, on the two long faces. A shopfront gets a taller ground
        // floor and a sign; everything else gets a regular grid.
        for (int face = 0; face < 2; ++face) {
            const int axis = face;  // 0 = the X-normal face, 1 = the Z-normal face
            const float span = (axis == 0) ? w : d;
            const float outward = (axis == 0) ? d * 0.5f : w * 0.5f;
            // One opening every 2.6 m, so the count scales with the facade
            // rather than being a constant that happens to suit one building.
            const int32_t bays = std::max(1, static_cast<int32_t>(std::floor(span / 2.6f)));
            const float bay = span / static_cast<float>(bays);
            const float winW = std::min(1.25f, bay * 0.55f);
            const float winH = std::min(1.6f, floorHeight * 0.52f);
            for (int32_t f = 0; f < floors; ++f) {
                // The ground floor of a shop is a shopfront, not a window.
                if (f == 0 && bd.kind == 0) continue;
                const float y = ground + static_cast<float>(f) * floorHeight + floorHeight * 0.28f;
                for (int32_t i = 0; i < bays; ++i) {
                    const float along = -span * 0.5f + (static_cast<float>(i) + 0.5f) * bay;
                    const float wx = (axis == 0) ? x + along : x + outward;
                    const float wz = (axis == 0) ? z + outward : z + along;
                    kit.window(b, wx, y, wz, winW, winH, axis);
                }
            }
        }

        // A door on the long face, and a step.
        const float doorSide = (rng.next() & 1u) ? 1.0f : -1.0f;
        const float dx = x + doorSide * (w * 0.5f);
        kit.door(b, dx, ground, z, 1.1f, 2.15f, 0);
    }

    if (o.detailLevel >= 2) {
        // Roof plant and a parapet, so the skyline has silhouette. A flat roof
        // with nothing on it is a rectangle, and a skyline of rectangles has no
        // skyline.
        kit.parapet(b, x, ground + h, z, w, d, kPale, res.get("wall_stone"));
        const int32_t units = 1 + rng.pick(3);
        for (int i = 0; i < units; ++i) {
            const float ux = x + rng.range(-w * 0.3f, w * 0.3f);
            const float uz = z + rng.range(-d * 0.3f, d * 0.3f);
            kit.acUnit(b, ux, ground + h, uz);
        }
        if (rng.unit() < 0.5f) {
            kit.pipeRun(b, x, ground + h + 0.3f, z, std::min(w, d) * 0.6f, 0.12f,
                        (w > d) ? 0 : 1);
        }
    }

    // Signage for a commercial ground floor. Small, and disproportionately
    // effective: a lit sign on an otherwise blank shopfront is what says
    // "occupied" from a block away.
    if (o.detailLevel >= 1 && bd.kind == 0) {
        const float sy = ground + std::min(3.4f, floorHeight * 0.9f);
        kit.signBoard(b, x, sy, z + d * 0.5f + 0.10f, std::min(w * 0.7f, 5.0f), 0.7f, 1);
    }

    // Street furniture, on the pavement at the front. Off at detail 0 because a
    // bin every eight metres is invisible from anywhere and costs a lot.
    if (o.includeProps && o.detailLevel >= 1) {
        const float front = z + d * 0.5f + 1.4f;
        switch (rng.pick(4)) {
            case 0: kit.bin(b, x + w * 0.3f, ground, front); break;
            case 1: kit.bench(b, x - w * 0.3f, ground, front, static_cast<int>(rng.next() & 1u)); break;
            case 2: kit.bollard(b, x + w * 0.4f, ground, front); break;
            default: kit.crate(b, x - w * 0.4f, ground, front, 0.6f); break;
        }
    }
}

}  // namespace

void SceneMesh::readVertex(uint32_t index, float out[kFloatsPerVertex]) const {
    const size_t base = static_cast<size_t>(index) * kFloatsPerVertex;
    for (int i = 0; i < kFloatsPerVertex; ++i) out[i] = vertices[base + static_cast<size_t>(i)];
}

SceneMesh buildSceneMesh(const World &world, const MaterialTable &materials,
                         const SceneBuildOptions &options) {
    SceneMesh mesh;
    SceneBuildOptions o = options;
    // A detail parameter with no ceiling is a detail parameter with no budget.
    o.detailLevel = std::max(0, std::min(2, o.detailLevel));
    if (!(o.radius > 0.0)) o.radius = 1.0;

    GeometryKit kit(materials);
    Resolver res(materials, kit);
    MeshBuilder b(materials);

    const float groundCol[3] = {0.34f, 0.33f, 0.29f};
    const int32_t terrainMat = res.get("terrain_grass");

    // -- ground -------------------------------------------------------------
    if (o.includeGround) {
        // A grid whose cell size grows with distance from the centre. A uniform
        // grid over a 400 m radius is either 40k quads to get the far detail
        // right, or one quad per 16 m and a horizon made of visible facets.
        // Growing the cells keeps the near ground smooth and the triangle count
        // roughly logarithmic in the radius.
        const double ring = 24.0;
        double prev = 0.0;
        uint32_t quads = 0;
        for (int level = 0; level < 5; ++level) {
            const double next = std::min(o.radius, prev * 2.0 + ring);
            if (!(next > prev)) break;
            const double cell = (level < 2) ? 4.0 : 12.0;
            const int32_t n = static_cast<int32_t>(std::floor((next - prev) / cell));
            for (int32_t i = 0; i < n; ++i) {
                for (int32_t j = 0; j < n; ++j) {
                    const double x0 = o.centerX + prev + static_cast<double>(i) * cell;
                    const double z0 = o.centerZ + prev + static_cast<double>(j) * cell;
                    // Sample the height at the corners and emit two triangles
                    // that follow the slope, so the ground undulates instead of
                    // floating as a staircase of flat quads.
                    const float y00 = static_cast<float>(terrainHeight(x0, z0, world.seed));
                    const float y10 = static_cast<float>(terrainHeight(x0 + cell, z0, world.seed));
                    const float y01 = static_cast<float>(terrainHeight(x0, z0 + cell, world.seed));
                    const float y11 = static_cast<float>(terrainHeight(x0 + cell, z0 + cell, world.seed));
                    const float p00[3] = {static_cast<float>(x0), y00, static_cast<float>(z0)};
                    const float p10[3] = {static_cast<float>(x0 + cell), y10, static_cast<float>(z0)};
                    const float p11[3] = {static_cast<float>(x0 + cell), y11, static_cast<float>(z0 + cell)};
                    const float p01[3] = {static_cast<float>(x0), y01, static_cast<float>(z0 + cell)};
                    // A face normal from the actual slope, not +Y. Flat-shading
                    // the terrain removes every hill.
                    const float nx = (y00 - y10) / static_cast<float>(cell);
                    const float nz = (y00 - y01) / static_cast<float>(cell);
                    const float inv = 1.0f / std::sqrt(nx * nx + 1.0f + nz * nz);
                    const float slope[3] = {nx * inv, inv, nz * inv};
                    b.triangle(p00, p10, p11, slope, groundCol[0], groundCol[1], groundCol[2], terrainMat);
                    b.triangle(p00, p11, p01, slope, groundCol[0], groundCol[1], groundCol[2], terrainMat);
                    ++quads;
                }
            }
            prev = next;
        }
        mesh.stats.groundQuads = quads;
    }

    // -- roads --------------------------------------------------------------
    // Clipped to the slice box. A road is at most a few tens of metres across
    // so clipping it is nearly a no-op, but doing it uniformly means the slice
    // is bounded by construction rather than by the size of whatever content
    // happens to be largest.
    for (const Road &r : world.roads) {
        float x0 = static_cast<float>(r.x) - static_cast<float>(r.w) * 0.5f;
        float x1 = static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f;
        float z0 = static_cast<float>(r.z) - static_cast<float>(r.d) * 0.5f;
        float z1 = static_cast<float>(r.z) + static_cast<float>(r.d) * 0.5f;
        if (!clipToSlice(x0, x1, z0, z1, o)) continue;
        const float y = static_cast<float>(terrainHeight(r.x, r.z, world.seed)) + 0.05f;
        const float col[3] = {0.16f, 0.16f, 0.17f};
        b.plane(0.5f * (x0 + x1), y, 0.5f * (z0 + z1), x1 - x0, z1 - z0, col[0], col[1], col[2],
                res.get("road_asphalt"));
        ++mesh.stats.roads;

        if (o.includeProps && o.detailLevel >= 1 && r.kind == 0) {
            // Lamps at arterial spacing. Every 26 m is roughly the real thing,
            // and the exact number matters less than the fact that it is
            // regular: irregular lamp spacing is the giveaway.
            const float spacing = 26.0f;
            const int32_t count = static_cast<int32_t>((z1 - z0) / spacing);
            for (int32_t i = 0; i < count; ++i) {
                const float t = z0 + static_cast<float>(i) * spacing + spacing * 0.5f;
                if (t > z1) break;
                kit.streetLamp(b, 0.5f * (x0 + x1), y, t, (i % 2 == 0) ? 0 : 1);
                ++mesh.stats.props;
            }
        }
    }

    // -- water --------------------------------------------------------------
    // A river is a single polygon kilometres across. Emitted whole, one of them
    // makes the slice's bounding box kilometres wide, which culls nothing,
    // fits no budget and defeats the point of streaming a slice at all. Clipped
    // to the same box as everything else.
    for (const Water &wtr : world.water) {
        float x0 = static_cast<float>(wtr.x) - static_cast<float>(wtr.w) * 0.5f;
        float x1 = static_cast<float>(wtr.x) + static_cast<float>(wtr.w) * 0.5f;
        float z0 = static_cast<float>(wtr.z) - static_cast<float>(wtr.d) * 0.5f;
        float z1 = static_cast<float>(wtr.z) + static_cast<float>(wtr.d) * 0.5f;
        if (!clipToSlice(x0, x1, z0, z1, o)) continue;
        const float y = static_cast<float>(terrainHeight(wtr.x, wtr.z, world.seed)) - 0.4f;
        const float col[3] = {0.10f, 0.19f, 0.26f};
        b.plane(0.5f * (x0 + x1), y, 0.5f * (z0 + z1), x1 - x0, z1 - z0, col[0], col[1], col[2],
                res.get("glass"));
    }

    // -- buildings ----------------------------------------------------------
    // Collected and sorted by distance first, so the triangle budget drops the
    // *farthest* buildings rather than whichever ones happened to come last in
    // the world list. Dropping in iteration order is how a streaming slice ends
    // up with a hole in the middle of it.
    struct Candidate {
        const Building *bd;
        double distanceSq;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(world.buildings.size());
    for (const Building &bd : world.buildings) {
        const double dx = bd.x - o.centerX;
        const double dz = bd.z - o.centerZ;
        const double d2 = dx * dx + dz * dz;
        // The pad is the building's half-diagonal, which is the exact radius
        // within which at least one corner of its footprint is inside the
        // slice. Using half the width *plus* half the depth instead is looser
        // than the geometry allows, and the looseness compounds: a building
        // admitted that far outside the circle then overhangs the slice's
        // bounds, which is how a streaming radius quietly stops bounding
        // anything.
        const double reach = o.radius + 0.5 * std::hypot(bd.w, bd.d);
        if (d2 > reach * reach) {
            ++mesh.stats.buildingsOutsideRadius;
            continue;
        }
        candidates.push_back({&bd, d2});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &lhs, const Candidate &rhs) { return lhs.distanceSq < rhs.distanceSq; });

    // The budget is measured in triangles actually emitted, read back from the
    // builder, because anything cheaper is a count of the wrong thing: an
    // earlier version incremented a counter once per building and compared it
    // against a triangle ceiling, so the budget never once fired and the test
    // that was supposed to prove it worked proved nothing.
    auto trianglesSoFar = [&]() {
        return static_cast<uint32_t>(b.size() / kFloatsPerVertex / 3);
    };
    for (const Candidate &c : candidates) {
        const Building &bd = *c.bd;
        if (o.maxTriangles > 0 && trianglesSoFar() > o.maxTriangles) {
            ++mesh.stats.buildingsDroppedForBudget;
            mesh.stats.budgetLimited = true;
            continue;
        }
        const size_t before = b.size();
        emitBuilding(b, bd, o, res, world.seed);
        if (b.size() == before) continue;  // nothing emitted; do not count it
        ++mesh.stats.buildings;
    }


    // -- props --------------------------------------------------------------
    if (o.includeProps) {
        for (const Tree &t : world.trees) {
            if (!insideRadius(o, t.x, t.z, 2.0)) continue;
            DetailRng rng(t.variant * 9781u + 17u);
            const float s = static_cast<float>(t.scale);
            const float y = static_cast<float>(terrainHeight(t.x, t.z, world.seed));
            const float bark[3] = {0.30f, 0.24f, 0.18f};
            const float leaf[3] = {0.22f, 0.36f, 0.17f};
            b.cylinder(static_cast<float>(t.x), y, static_cast<float>(t.z), 0.22f * s, 2.6f * s,
                       bark[0], bark[1], bark[2], res.get("bark"), 6);
            // Two crossed boxes rather than a sphere: no sphere primitive exists,
            // and crossed billboards are the standard tree canopy and read
            // correctly from every angle at the distances they are seen from.
            const float cy = y + 3.6f * s;
            for (int i = 0; i < 2; ++i) {
                b.box(static_cast<float>(t.x) + (i == 0 ? 0.0f : rng.range(-0.2f, 0.2f)), cy,
                      static_cast<float>(t.z), 2.4f * s, 1.1f * s, 2.4f * s, leaf[0], leaf[1], leaf[2],
                      res.get("foliage"));
            }
            ++mesh.stats.trees;
        }
        for (const Bush &bu : world.bushes) {
            if (!insideRadius(o, bu.x, bu.z, 1.0)) continue;
            const float s = static_cast<float>(bu.scale);
            const float y = static_cast<float>(terrainHeight(bu.x, bu.z, world.seed));
            const float leaf[3] = {0.20f, 0.31f, 0.16f};
            b.box(static_cast<float>(bu.x), y, static_cast<float>(bu.z), 1.1f * s, 0.75f * s,
                  1.1f * s, leaf[0], leaf[1], leaf[2], res.get("foliage"));
            ++mesh.stats.trees;
        }
    }

    // Read the bounds before the move, so the ordering of this function reads
    // the way it executes.
    if (b.hasBounds()) {
        b.bounds(mesh.boundsMin, mesh.boundsMax);
    }
    mesh.stats.vertices = static_cast<uint32_t>(b.size() / kFloatsPerVertex);
    mesh.stats.triangles = mesh.stats.vertices / 3;
    mesh.vertices = std::move(b.data());
    return mesh;
}

}  // namespace emergent
