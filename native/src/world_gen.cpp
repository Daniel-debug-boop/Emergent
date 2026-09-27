// The EMERGENT world generator — an exact port of `world.mjs`.
//
// See `world_gen.hpp` for why this has to be bit-exact rather than close. The
// short version: the native renderer and the reference implementation must
// agree on where every building is, or the parity test is asserting nothing.
//
// The structure below deliberately mirrors the JavaScript line for line. It
// would read better refactored into smaller functions, and it should not be:
// when the two drift, the diff that shows the drift is the useful artefact, and
// a structural rewrite destroys it.

#include "emergent/world_gen.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace emergent {
namespace {

// Math.imul is a signed 32-bit multiply with wraparound. Written out rather
// than relying on `a * b` overflowing, because signed overflow is undefined
// behaviour and the optimiser is entitled to do anything with it -- including
// in a debug build that happens to differ from the release build the game runs.
inline int32_t imul(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

inline uint32_t ushr(uint32_t h, uint32_t n) { return h >> n; }
inline uint32_t ushl(uint32_t h, uint32_t n) { return h << n; }

}  // namespace

double hash2(double x, double y, int seed) {
    const double n = std::sin(x * 127.1 + y * 311.7 + seed * 0.00001) * 43758.5453123;
    return n - std::floor(n);
}

double ihash(int32_t x, int32_t y, int32_t seed) {
    int32_t h = imul(x, 0x27d4eb2d) ^ imul(y, 0x165667b1) ^ imul(seed, 0x9e3779b1);
    h = imul(h ^ static_cast<int32_t>(ushr(static_cast<uint32_t>(h), 15)), 0x85ebca6b);
    uint32_t u = static_cast<uint32_t>(h);
    u ^= ushr(u, 13);
    h = imul(static_cast<int32_t>(u), 0xc2b2ae35);
    u = static_cast<uint32_t>(h);
    u ^= ushr(u, 16);
    return static_cast<double>(u) / 4294967296.0;
}

double valueNoise(double x, double y, int seed) {
    const double xi = std::floor(x);
    const double yi = std::floor(y);
    const double fx = x - xi;
    const double fy = y - yi;
    const double ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10);
    const double uy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    const int32_t xi32 = static_cast<int32_t>(xi);
    const int32_t yi32 = static_cast<int32_t>(yi);
    const double h00 = ihash(xi32, yi32, seed);
    const double h10 = ihash(xi32 + 1, yi32, seed);
    const double h01 = ihash(xi32, yi32 + 1, seed);
    const double h11 = ihash(xi32 + 1, yi32 + 1, seed);
    return (h00 * (1 - ux) + h10 * ux) * (1 - uy) + (h01 * (1 - ux) + h11 * ux) * uy;
}

double fbm(double x, double y, int seed, int octaves) {
    double v = 0, a = 0.5, fx = x, fy = y;
    for (int i = 0; i < octaves; i++) {
        v += a * valueNoise(fx, fy, seed + i * 1013);
        fx *= 2.03;
        fy *= 2.01;
        a *= 0.5;
    }
    return v;
}

double terrainHeight(double x, double z, int seed) {
    const double macro = fbm(x * 0.0018, z * 0.0018, seed, 6);
    const double detail = fbm(x * 0.007, z * 0.007, seed ^ 0x9e3779b9, 4);
    const double r = fbm(x * 0.00075, z * 0.00075, seed ^ 0x45d9f3b, 4) * 2 - 1;
    const double ridge = std::pow(std::fabs(r), 1.5);
    return (macro - 0.5) * 34 + (detail - 0.5) * 7 + ridge * 7;
}

const Road *nearestRoad(const World &world, double x, double z) {
    // Clamp the query point into each road's rectangle and measure to that, not
    // to the rectangle's centre and not by a squared box distance. The first
    // version of this used a box test that is *nearly* the same shape and gives
    // a different answer whenever the point is beside a road rather than inside
    // it, which changes `roadBias`, which changes which buildings get placed.
    // Roads are the anchor for the whole layout, so this has to be the same
    // measurement, not an equivalent one.
    const Road *best = nullptr;
    double bestD = 1e30;
    for (const Road &r : world.roads) {
        const double rx = std::min(std::max(x, r.x), r.x + r.w);
        const double rz = std::min(std::max(z, r.z), r.z + r.d);
        const double dx = x - rx, dz = z - rz;
        const double d = std::sqrt(dx * dx + dz * dz);
        if (d < bestD) {
            bestD = d;
            best = &r;
        }
    }
    return best;
}

const District *districtAt(const World &world, double x, double z) {
    const District *best = nullptr;
    double bestD = 1e30;
    for (const District &d : world.districts) {
        const double dx = x - d.x, dz = z - d.z;
        const double dist = std::sqrt(dx * dx + dz * dz);
        if (dist <= d.r && dist < bestD) {
            bestD = dist;
            best = &d;
        }
    }
    return best;
}

namespace {

District districtForRegion(const Region &reg, const World &w) {
    District d;
    d.id = reg.id;
    d.x = reg.rx * kRegionSize + kRegionSize * 0.5;
    d.z = reg.ry * kRegionSize + kRegionSize * 0.5;
    d.r = 280 + reg.density * 360;
    d.biome = reg.biome;
    d.density = reg.density;
    const double typeRoll = hash2(reg.rx + 41, reg.ry - 23, reg.id + 1009);
    d.type = typeRoll < 0.12   ? DistrictType::Industrial
             : typeRoll < 0.30 ? DistrictType::Commercial
             : typeRoll < 0.82 ? DistrictType::Residential
                                : DistrictType::Mixed;
    d.riverOffset = std::fabs(d.x - w.riverX);
    return d;
}

}  // namespace

World generateWorld(int32_t seed) {
    Rng rand(static_cast<uint32_t>(seed));
    World w;
    w.seed = seed;
    w.riverX = 4800 + (hash2(3, 7, seed) - 0.5) * 700;

    // Regions.
    for (int ry = 0; ry < 8; ry++) {
        for (int rx = 0; rx < 8; rx++) {
            const double b = fbm(rx * 0.17, ry * 0.17, seed, 4);
            Region reg;
            reg.id = ry * 8 + rx;
            reg.rx = rx;
            reg.ry = ry;
            reg.biome = b < 0.27 ? 0 : (b > 0.72 ? 2 : 1);  // 0 dry, 1 temperate, 2 lush
            reg.density = 0.24 + 0.76 * hash2(rx + 9, ry + 13, seed);
            w.regions.push_back(reg);
        }
    }

    // The meandering river, from deterministic control points.
    for (int z = -400; z < static_cast<int>(kWorldSize) + 400; z += 100) {
        const double bend = std::sin(z * 0.0015 + seed * 0.00011) * 170 + std::sin(z * 0.00053) * 85;
        Water water;
        water.id = static_cast<int32_t>(w.water.size());
        water.x = w.riverX + bend;
        water.z = z;
        water.w = 190 + 45 * hash2(z * 0.01, 13, seed);
        water.d = 120;
        w.water.push_back(water);
    }

    // The two arterials, then the grid.
    const double center = kWorldSize * 0.5;
    {
        Road a;
        a.id = 0;
        a.x = center - 45; a.z = -240; a.w = 90; a.d = kWorldSize + 480; a.kind = 3;
        w.roads.push_back(a);
        Road b;
        b.id = 1;
        b.x = -240; b.z = center - 45; b.w = kWorldSize + 480; b.d = 90; b.kind = 3;
        w.roads.push_back(b);
    }
    int32_t roadId = 2;
    for (int x = 300; x < static_cast<int>(kWorldSize); x += 600) {
        for (int z = 300; z < static_cast<int>(kWorldSize); z += 600) {
            if (std::fabs(x - center) < 190 || std::fabs(z - center) < 190) continue;
            Road v;
            v.id = roadId++; v.x = x - 17; v.z = z - 300; v.w = 34; v.d = 600; v.kind = 1;
            w.roads.push_back(v);
            Road h;
            h.id = roadId++; h.x = x - 300; h.z = z - 17; h.w = 600; h.d = 34; h.kind = 1;
            w.roads.push_back(h);
            if (hash2(x * 0.01, z * 0.01, seed) > 0.68) {
                Road lv;
                lv.id = roadId++; lv.x = x - 11; lv.z = z - 190; lv.w = 22; lv.d = 380; lv.kind = 0;
                w.roads.push_back(lv);
                Road lh;
                lh.id = roadId++; lh.x = x - 190; lh.z = z - 11; lh.w = 380; lh.d = 22; lh.kind = 0;
                w.roads.push_back(lh);
            }
        }
    }

    for (const Region &reg : w.regions) {
        if (reg.density > 0.45) w.districts.push_back(districtForRegion(reg, w));
    }

    // Buildings, biased toward district cores and nearby roads, with
    // anti-overlap. The occupancy test is a linear scan, which is O(n^2) over a
    // few thousand buildings; it is done this way rather than with a grid
    // because the scan is exact and a spatial index would change the acceptance
    // order and therefore the world.
    int32_t buildingId = 0;
    // Indices, not pointers. `w.buildings` is a vector and reallocates as it
    // grows, so a stored `&w.buildings.back()` is dangling from the moment the
    // next building is pushed — which is a use-after-free a few hundred
    // buildings in, and it crashed the whole generator. The JavaScript holds
    // object references and never has this problem, which is exactly the class
    // of bug a hand port introduces.
    std::vector<int32_t> occupied;
    for (const District &d : w.districts) {
        const int target = static_cast<int>(std::floor(24 + d.density * 55));
        int made = 0, attempts = 0;
        while (made < target && attempts++ < target * 8) {
            const double a = rand.uniform() * M_PI * 2.0;
            const double r = std::sqrt(rand.uniform()) * d.r;
            const double x = d.x + std::cos(a) * r;
            const double z = d.z + std::sin(a) * r;
            if (x < 90 || x > kWorldSize - 90 || z < 90 || z > kWorldSize - 90) continue;
            if (std::fabs(x - w.riverX) < 170) continue;
            const Road *road = nearestRoad(w, x, z);
            const double roadBias = road ? (road->kind >= 1 ? 0.82 : 0.52) : 0.18;
            if (rand.uniform() > 0.55 + roadBias * 0.35) continue;
            const double bw = 28 + rand.uniform() * 72;
            const double bd = 28 + rand.uniform() * 72;
            bool overlap = false;
            for (int32_t oi : occupied) {
                const Building &o = w.buildings[static_cast<size_t>(oi)];
                if (std::fabs(x - o.x) < (bw + o.w) * 0.47 &&
                    std::fabs(z - o.z) < (bd + o.d) * 0.47) {
                    overlap = true;
                    break;
                }
            }
            if (overlap) continue;
            // Two draws for an industrial district, not one. The JavaScript is
            // `let floors = commercial ? 3 + floor(rand()*9) : 1 + floor(rand()*5);`
            // followed by a *separate* `if (industrial) floors = 1 + floor(rand()*3);`
            // — so an industrial district draws the second expression and then
            // throws it away, drawing a third. Collapsing that into an
            // `else if` is the natural reading and it is wrong: it consumed
            // 1,174 fewer draws across the city, which shifted every subsequent
            // random and changed the tree and business counts.
            int floors;
            if (d.type == DistrictType::Commercial) {
                floors = 3 + static_cast<int>(std::floor(rand.uniform() * 9));
            } else {
                floors = 1 + static_cast<int>(std::floor(rand.uniform() * 5));
            }
            if (d.type == DistrictType::Industrial) {
                floors = 1 + static_cast<int>(std::floor(rand.uniform() * 3));
            }
            const double kindRoll = rand.uniform();
            int kind;
            if (kindRoll < (d.type == DistrictType::Commercial ? 0.33 : 0.12)) kind = 0;      // shop
            else if (kindRoll < 0.16) kind = 1;                                                // tower
            else if (d.type == DistrictType::Industrial) kind = 2;                             // warehouse
            else kind = 3;                                                                     // house
            Building b;
            b.id = buildingId++;
            b.x = x; b.z = z; b.w = bw; b.d = bd;
            b.h = floors * 3.2 + 3 + rand.uniform() * 2;
            b.floors = floors;
            b.kind = kind;
            b.district = d.id;
            b.industrial = d.type == DistrictType::Industrial;
            b.rngState = rand.state();
            // The JavaScript record draws four more values here (seed, facade,
            // roof, sign, doorSide). They are consumed in the same order so the
            // generator's stream stays in step for everything after this loop.
            rand.uniform(); rand.uniform(); rand.uniform();
            rand.uniform();
            rand.uniform();
            w.buildings.push_back(b);
            occupied.push_back(b.id);
            made++;
        }
    }

    // Vegetation.
    int32_t treeId = 0;
    for (int i = 0; i < 5200; i++) {
        const double x = rand.uniform() * kWorldSize;
        const double z = rand.uniform() * kWorldSize;
        if (std::fabs(x - w.riverX) < 150) continue;
        bool nearDistrict = false;
        double districtDensity = 0;
        for (const District &d : w.districts) {
            const double dx = x - d.x, dz = z - d.z;
            const double dd = std::sqrt(dx * dx + dz * dz);
            if (dd < d.r) {
                nearDistrict = true;
                districtDensity = std::max(districtDensity, d.density);
            }
        }
        const int64_t idx = std::min<int64_t>(63, std::max<int64_t>(
            0, static_cast<int64_t>(std::floor(z / 1200.0)) * 8 + static_cast<int64_t>(std::floor(x / 1200.0))));
        const int32_t biome = w.regions[static_cast<size_t>(idx)].biome;
        const double chance = nearDistrict ? 0.16 + (1 - districtDensity) * 0.32
                                           : (biome == 2 ? 0.72 : (biome == 0 ? 0.28 : 0.5));
        if (rand.uniform() > chance) continue;
        Tree t;
        t.id = treeId++;
        t.x = x; t.z = z;
        t.scale = 0.8 + rand.uniform() * 2.1;
        // `rand() < 0.17 ? 'pine' : rand() < 0.12 ? 'broadleaf' : 'tree'` draws
        // *twice* whenever the first comparison fails, because the second draw
        // is only evaluated in the false branch. Reading one draw here looks
        // equivalent and is not: it desynchronises the generator for every
        // remaining tree, and the count came out 1940 instead of 1961.
        const double v1 = rand.uniform();
        if (v1 < 0.17) {
            t.variant = 0;
        } else {
            const double v2 = rand.uniform();
            t.variant = v2 < 0.12 ? 1 : 2;
        }
        rand.uniform();  // the tree's own seed draw
        w.trees.push_back(t);
    }

    // Businesses emerge from commercial and ground-floor buildings.
    int32_t businessId = 0;
    for (const Building &b : w.buildings) {
        const bool isShop = b.kind == 0;
        const bool warehouseRoll = b.kind == 2 && rand.uniform() < 0.45;
        if (!(isShop || warehouseRoll || rand.uniform() < 0.05)) continue;
        Business biz;
        biz.id = businessId++;
        biz.building = b.id;
        biz.x = b.x;
        biz.z = b.z;
        if (b.kind == 2) biz.kind = "supply";
        else {
            // `rand() < 0.35 ? 'market' : rand() < 0.6 ? 'cafe' : 'service'`
            // draws twice whenever the first comparison fails. Reading it as one
            // draw with a reused value is the natural mistake and it silently
            // desynchronises the stream for every building after this one.
            const double t1 = rand.uniform();
            if (t1 < 0.35) {
                biz.kind = "market";
            } else {
                const double t2 = rand.uniform();
                biz.kind = t2 < 0.6 ? "cafe" : "service";
            }
        }
        // The JavaScript record draws five values after `type`: stock, price,
        // reputation, supply, popularity. `open`, `customers` and `revenue` are
        // constants, not draws. This port originally invented three colour
        // draws that do not exist in the reference and padded the rest, which
        // consumed 3,682 extra draws across the city and turned 707 businesses
        // into 722. The colour is derived from the business id instead, so it
        // costs no draws and cannot desynchronise the stream.
        rand.uniform();  // stock
        rand.uniform();  // price
        rand.uniform();  // reputation
        rand.uniform();  // supply
        rand.uniform();  // popularity
        const double cr = hash2(biz.id * 1.7, 3.1, seed);
        const double cg = hash2(biz.id * 2.3, 7.7, seed);
        const double cb = hash2(biz.id * 3.1, 11.3, seed);
        biz.color = 0xff000000u | (static_cast<uint32_t>(cr * 255) << 16) |
                    (static_cast<uint32_t>(cg * 255) << 8) | static_cast<uint32_t>(cb * 255);
        w.businesses.push_back(biz);
    }

    // Inhabitants.
    int32_t npcId = 0;
    for (int i = 0; i < 520; i++) {
        const District *homeD = w.districts.empty()
                                    ? nullptr
                                    : &w.districts[static_cast<size_t>(rand.uniform() * w.districts.size())];
        // Drawn and discarded: the workplace is not a rendered field yet, but
        // the draw must stay or every subsequent random diverges from the
        // reference implementation.
        (void)(w.districts.empty()
                   ? homeD
                   : &w.districts[static_cast<size_t>(rand.uniform() * w.districts.size())]);
        Npc n;
        n.id = npcId++;
        n.x = (homeD ? homeD->x : center) + (rand.uniform() - 0.5) * 260;
        n.z = (homeD ? homeD->z : center) + (rand.uniform() - 0.5) * 260;
        const double jobRoll = rand.uniform();
        n.job = jobRoll < 0.52 ? 0 : (jobRoll < 0.72 ? 1 : (jobRoll < 0.87 ? 2 : 3));
        n.activity = 0;
        // The remaining per-NPC draws, in the JavaScript order, so the stream
        // stays aligned with the reference implementation.
        rand.uniform(); rand.uniform(); rand.uniform(); rand.uniform(); rand.uniform();
        rand.uniform(); rand.uniform(); rand.uniform(); rand.uniform(); rand.uniform();
        w.npcs.push_back(n);
    }

    // Traffic.
    for (int i = 0; i < 150; i++) {
        Car c;
        c.id = i;
        const bool horizontal = rand.uniform() < 0.5;
        const double lane = rand.uniform() < 0.5 ? -7 : 7;
        c.variant = horizontal ? 1 : 0;
        c.x = horizontal ? rand.uniform() * kWorldSize : center + lane;
        c.z = horizontal ? center + lane : rand.uniform() * kWorldSize;
        rand.uniform(); rand.uniform(); rand.uniform(); rand.uniform();
        w.cars.push_back(c);
    }

    return w;
}

namespace {

/** Densest cell of a grid, returned rather than accumulated into a caller. */
struct CellHit {
    double x = 0;
    double z = 0;
    int32_t count = 0;
    bool found = false;
};

CellHit densestCell(const std::vector<Building> &buildings, double originX, double originZ,
                    double cell, double halfExtent) {
    CellHit hit;
    const int32_t n = std::max(1, static_cast<int32_t>(2.0 * halfExtent / cell));
    std::vector<int32_t> counts(static_cast<size_t>(n) * static_cast<size_t>(n), 0);
    for (const Building &b : buildings) {
        const int32_t i = static_cast<int32_t>((b.x - originX + halfExtent) / cell);
        const int32_t j = static_cast<int32_t>((b.z - originZ + halfExtent) / cell);
        if (i < 0 || j < 0 || i >= n || j >= n) continue;
        ++counts[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)];
    }
    int32_t bi = 0, bj = 0, best = -1;
    for (int32_t j = 0; j < n; ++j) {
        for (int32_t i = 0; i < n; ++i) {
            const int32_t c =
                counts[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)];
            if (c > best) {
                best = c;
                bi = i;
                bj = j;
            }
        }
    }
    if (best <= 0) return hit;
    hit.found = true;
    hit.count = best;
    hit.x = originX - halfExtent + (static_cast<double>(bi) + 0.5) * cell;
    hit.z = originZ - halfExtent + (static_cast<double>(bj) + 0.5) * cell;
    return hit;
}

}  // namespace

WorldCentre worldCentre(const World &world, double searchRadius) {
    WorldCentre out;
    if (world.buildings.empty()) return out;

    // Two passes: a coarse grid to find the right neighbourhood, then a finer
    // one centred on that result. One pass either quantises the answer visibly
    // or needs a grid fine enough to be slow over a multi-kilometre world.
    constexpr double kCoarse = 128.0;
    const CellHit coarse = densestCell(world.buildings, 0.0, 0.0, kCoarse, searchRadius);
    if (!coarse.found) return out;

    constexpr double kFine = 16.0;
    const CellHit fine = densestCell(world.buildings, coarse.x, coarse.z, kFine, kCoarse);
    const double cell = fine.found ? kFine : kCoarse;
    const int32_t count = fine.found ? fine.count : coarse.count;

    out.x = fine.found ? fine.x : coarse.x;
    out.z = fine.found ? fine.z : coarse.z;
    // Buildings per 10000 square metres, which is the unit a district density
    // is normally quoted in.
    out.density = (static_cast<double>(count) / (cell * cell)) * 10000.0;
    return out;
}

}  // namespace emergent
