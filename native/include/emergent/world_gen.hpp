// The EMERGENT world, generated in C++.
//
// This is a direct port of `world.mjs`, and it has to be *exact* rather than
// approximate. The native renderer draws the same city the JavaScript one
// generated, from the same seed, down to the same building on the same
// corner. Anything less and the two front ends become two different games
// wearing one name, and every screenshot comparison between them is worthless.
//
// Three things make bit-exactness achievable rather than aspirational:
//
//   - Every hash is an integer operation. `ihash` is built from `Math.imul`,
//     which is a signed 32-bit multiply, and the C++ side uses the same
//     explicit 32-bit wrap rather than relying on UB-free signed overflow.
//   - The one float function involved, `hash2`, is a `sin`. IEEE-754 `sin` is
//     not correctly rounded and may differ in the last ulp between libms, which
//     is enough to change a `rand() < 0.12` district roll. It is therefore
//     quarantined in `hash2` and the terrain — the only thing sampled hundreds
//     of thousands of times — uses the integer `ihash` instead, exactly as the
//     JavaScript side already does.
//   - The xorshift generator is the same 13/17/5 triple with the same
//     unsigned 32-bit wrap, so a given seed produces the same sequence of
//     draws in the same order.
//
// `tests/world_parity_test.cpp` asserts the parity rather than assuming it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace emergent {

inline constexpr double kWorldSize = 9600.0;
inline constexpr double kRegionSize = 1200.0;

// -- seeded noise ------------------------------------------------------------

/** xorshift32. The same 13/17/5 triple as `world.mjs`, same wrap, same order. */
class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed ? seed : 1u) {}
    uint32_t next() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }
    /** Uniform in [0, 1). */
    double uniform() { return static_cast<double>(next()) / 4294967296.0; }
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    uint32_t state() const { return state_; }
    void setState(uint32_t s) { state_ = s ? s : 1u; }

private:
    uint32_t state_;
};

/** A float hash for scattering. Uses `sin`, so it is not safe to rely on across libms. */
double hash2(double x, double y, int seed = 1);

/** An integer-lattice hash. Exact everywhere; this is what terrain uses. */
double ihash(int32_t x, int32_t y, int32_t seed);

/** Quintic-smoothed value noise. */
double valueNoise(double x, double y, int seed);

/** Fractal sum of value noise. */
double fbm(double x, double y, int seed, int octaves = 5);

/** The height of the ground at a world position. */
double terrainHeight(double x, double z, int seed);

// -- world records -----------------------------------------------------------

enum class DistrictType { Industrial, Commercial, Residential, Mixed };

struct Region {
    int32_t id = 0;
    int32_t rx = 0;
    int32_t ry = 0;
    double x = 0, z = 0;
    double density = 0;
    int32_t biome = 0;
};

struct District {
    int32_t id = 0;
    double x = 0, z = 0, r = 0;
    int32_t biome = 0;
    double density = 0;
    DistrictType type = DistrictType::Mixed;
    double riverOffset = 0;
};

struct Road {
    int32_t id = 0;
    double x = 0, z = 0, w = 0, d = 0;
    int32_t district = 0;
    int32_t kind = 0;  // 0 arterial, 1 street, 2 lane
};

struct Building {
    int32_t id = 0;
    double x = 0, z = 0, w = 0, d = 0, h = 0;
    int32_t district = 0;
    int32_t kind = 0;      // archetype index: 0 shop, 1 tower, 2 warehouse, 3 house
    int32_t floors = 0;
    bool industrial = false;
    int32_t business = -1;  // index into World::businesses, or -1
    uint32_t rngState = 0;  // the generator position, so interiors are stable
};

struct Tree {
    int32_t id = 0;
    double x = 0, z = 0;
    double scale = 1.0;
    uint32_t variant = 0;
};

struct Bush {
    int32_t id = 0;
    double x = 0, z = 0;
    double scale = 1.0;
    uint32_t variant = 0;
};

struct Npc {
    int32_t id = 0;
    double x = 0, z = 0;
    int32_t job = 0;
    int32_t activity = 0;
};

struct Car {
    int32_t id = 0;
    double x = 0, z = 0;
    int32_t variant = 0;
    int32_t road = 0;
};

struct Business {
    int32_t id = 0;
    double x = 0, z = 0;
    std::string name;
    std::string kind;
    int32_t building = -1;
    uint32_t color = 0x808080ff;
};

struct Water {
    int32_t id = 0;
    double x = 0, z = 0, w = 0, d = 0;
};

struct World {
    int32_t seed = 1;
    std::vector<Region> regions;
    std::vector<District> districts;
    std::vector<Road> roads;
    std::vector<Building> buildings;
    std::vector<Tree> trees;
    std::vector<Bush> bushes;
    std::vector<Npc> npcs;
    std::vector<Car> cars;
    std::vector<Business> businesses;
    std::vector<Water> water;
    double riverX = 0;
};

/** Generate the world for a seed. Deterministic and thread-independent. */
World generateWorld(int32_t seed);

/** The nearest road to a point, or nullptr. */
const Road *nearestRoad(const World &world, double x, double z);

/** The district containing a point, or nullptr. */
const District *districtAt(const World &world, double x, double z);

/** A point in the middle of the built-up area, and how dense it is. */
struct WorldCentre {
    double x = 0;
    double z = 0;
    /** Buildings per 10000 square metres, at that point. */
    double density = 0;
};

/**
 * Find the middle of the city.
 *
 * The generated world is not centred on the origin. It is laid out across a
 * large area and the built-up part lands wherever the region noise put it, so
 * for a typical seed the nearest building to (0,0) is several hundred metres
 * away and a slice built at the origin is empty ground.
 *
 * That matters for more than a test's convenience: a streamer that centres on
 * the origin streams nothing, a spawn point at the origin is in a field, and a
 * minimap drawn from the origin shows a blank. So the centre is computed from
 * the geometry rather than assumed.
 *
 * The search is over a coarse grid rather than a gradient walk, because a
 * centroid is the wrong answer: the mean of a city's building positions is
 * pulled toward whichever district happens to be largest, which is regularly
 * the industrial one on the edge. The densest cell is downtown by construction.
 */
WorldCentre worldCentre(const World &world, double searchRadius = 4096.0);

}  // namespace emergent
