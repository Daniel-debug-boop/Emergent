// Native geometry emitters.
//
// The C++ counterpart of `geometry.mjs` and `city.mjs`: the detail kit and the
// world dressing. Every emitter writes into the same 12-float vertex layout the
// shader reads, through one function, so a material index passed into a colour
// argument is impossible by construction rather than by review.
//
// The emitters are not a transcription of the JavaScript line for line — they
// are a port of the *shapes*. What must match exactly is the output geometry,
// and `tests/geometry_parity_test.cpp` checks that against a fixture from the
// JavaScript, the same way the world does.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "emergent/materials.hpp"

namespace emergent {

/** A vertex accumulator with the single emission choke point. */
class MeshBuilder {
public:
    explicit MeshBuilder(const MaterialTable &materials) : materials_(&materials) {}

    /** The one place a vertex is written. Validates; never silently wrong. */
    void vertex(float px, float py, float pz, float nx, float ny, float nz, float r, float g, float b,
                int32_t material, float u = 0.0f, float v = 0.0f);

    void triangle(const float a[3], const float b[3], const float c[3], const float n[3], float r, float g,
                  float bl, int32_t material);
    void quad(const float a[3], const float b[3], const float c[3], const float d[3], const float n[3], float r,
              float g, float bl, int32_t material);

    /** An axis-aligned box, bottom-anchored at (x, y, z). */
    void box(float x, float y, float z, float w, float h, float d, float r, float g, float b, int32_t material);

    /** A vertical cylinder. Normals are normalised; an unnormalised cone is the
     *  single most common way a generated scene is silently too bright. */
    void cylinder(float x, float y, float z, float radius, float height, float r, float g, float b,
                  int32_t material, int segments = 8);

    void cone(float x, float y, float z, float radius, float height, float r, float g, float b, int32_t material,
              int segments = 8);

    /** A horizontal ground patch. The terrain and road surfaces. */
    void plane(float x, float y, float z, float w, float d, float r, float g, float b, int32_t material);

    /** A unit quad, for the billboards and the water surface. */
    void panel(float x, float y, float z, float w, float h, int axis, float r, float g, float b, int32_t material);

    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }
    const std::vector<float> &data() const { return data_; }
    std::vector<float> &data() { return data_; }

    /** The bounding box of everything emitted, for culling. */
    void bounds(float min[3], float max[3]) const;
    bool hasBounds() const { return hasBounds_; }

    const MaterialTable &materials() const { return *materials_; }
    int32_t material(const std::string &id) const { return materials_->index(id); }

    void clear() {
        data_.clear();
        hasBounds_ = false;
    }

private:
    const MaterialTable *materials_;
    std::vector<float> data_;
    bool hasBounds_ = false;
    float min_[3] = {0, 0, 0};
    float max_[3] = {0, 0, 0};
};

/** The detail kit: the small solids that break up a box. */
class GeometryKit {
public:
    explicit GeometryKit(const MaterialTable &materials) : m_(materials) {}

    int32_t M(const std::string &id) const { return m_.index(id); }

    void box(MeshBuilder &b, float x, float y, float z, float w, float h, float d, const float col[3],
             int32_t mat) const;
    void window(MeshBuilder &b, float x, float y, float z, float w, float h, int axis) const;
    void door(MeshBuilder &b, float x, float y, float z, float w, float h, int axis) const;
    void band(MeshBuilder &b, float x, float y, float z, float w, float d, float h, const float col[3],
              int32_t mat) const;
    void parapet(MeshBuilder &b, float x, float y, float z, float w, float d, const float col[3],
                 int32_t mat) const;
    void railing(MeshBuilder &b, float x, float y, float z, float len, float h, int axis, const float col[3],
                 int32_t mat) const;
    void acUnit(MeshBuilder &b, float x, float y, float z) const;
    void pipeRun(MeshBuilder &b, float x, float y, float z, float len, float r, int axis) const;
    void streetLamp(MeshBuilder &b, float x, float y, float z, int facing) const;
    void trafficLight(MeshBuilder &b, float x, float y, float z, int facing) const;
    void bin(MeshBuilder &b, float x, float y, float z) const;
    void bench(MeshBuilder &b, float x, float y, float z, int facing) const;
    void container(MeshBuilder &b, float x, float y, float z, int facing) const;
    void pallet(MeshBuilder &b, float x, float y, float z) const;
    void crate(MeshBuilder &b, float x, float y, float z, float s) const;
    void bollard(MeshBuilder &b, float x, float y, float z) const;
    void signBoard(MeshBuilder &b, float x, float y, float z, float w, float h, int facing) const;

private:
    const MaterialTable &m_;
};

}  // namespace emergent
