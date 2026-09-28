#pragma once
#include <cstdint>

namespace emergent {

// Physical atmosphere: the sky, the light it casts, and the haze that puts
// distance into the world.
//
// Everything here is CPU-side and deterministic. That is deliberate. The GPU
// samples these functions for the final image, but the model is evaluated on
// the host so the same numbers drive the sun's colour, the fog, the ambient
// term and the shaders — one source of truth, and one that a test can check
// without a graphics device. A scattering model that only exists in a shader
// cannot be unit tested at all, which is why so many engines ship a sky that is
// pretty and wrong.
//
// The model is Preetham's analytic single-scattering approximation as refined
// by Simon Wallner, "A Crude Approximation for the Computation of Sky
// Radiance in Real-Time Applications" (Master's thesis, Linköping University,
// 2003). It is a fit to Bruneton's spherical-earth solution good enough for
// real time, and it is the same model used by the "Simple Analytic
// Approximations to the CIE XYZ..." family of sky models that most engines
// actually ship. Mie is Cornette-Shanks rather than Rayleigh, which is the
// standard correction: Rayleigh alone has no forward lobe, and a sky with no
// forward lobe has no sun glare and no sunsets.
//
// Everything is metres and the scattering coefficients are per metre, so the
// scale is real rather than fitted to a unit cube.

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

/** Two channels, for the paired optical-depth integrals (air, and ozone+mie). */
struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

inline Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3 &a) { return a * s; }
inline Vec3 operator*(const Vec3 &a, const Vec3 &b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator+(const Vec3 &a, float s) { return {a.x + s, a.y + s, a.z + s}; }
inline Vec3 operator-(float s, const Vec3 &a) { return {s - a.x, s - a.y, s - a.z}; }
inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3 &v) {
    const float l = v.x * v.x + v.y * v.y + v.z * v.z;
    return l > 0.0f ? __builtin_sqrtf(l) : 0.0f;
}
inline Vec3 normalize(const Vec3 &v) {
    const float l = length(v);
    return l > 1e-8f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0.0f, 1.0f, 0.0f};
}
inline Vec3 lerp(const Vec3 &a, const Vec3 &b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/**
 * Atmosphere constants.
 *
 * The scattering coefficients are the measured values for sea-level air at
 * 680/550/440 nm — the wavelengths at which Rayleigh scattering is strongest
 * for red, green and blue respectively. Rayleigh scales as 1/lambda^4, which is
 * why the ratio between them (5.8 : 13.5 : 33.1) is the entire reason the sky is
 * blue rather than grey. Getting this ratio wrong is the difference between a
 * sky and a fog.
 *
 * Mie is a grey, wavelength-independent scattering by aerosols, and it is much
 * stronger near the surface. `mieScale` is its vertical falloff exponent; the
 * value of 1/12000 m matches the reference implementation.
 */
struct AtmosphereConstants {
    // Rayleigh scattering, per metre, at 680/550/440 nm.
    Vec3 rayleighScattering{5.802e-6f, 13.558e-6f, 33.1e-6f};
    // Mie scattering and absorption, per metre.
    float mieScattering = 3.996e-6f;
    float mieAbsorption = 4.4e-6f;
    // Ozone absorption per metre, and the vertical falloff of its layer. Ozone
    // is what makes the twilight zenith blue rather than grey, and it is the
    // most frequently omitted term in a real-time sky.
    Vec3 ozoneAbsorption{0.650e-6f, 1.881e-6f, 0.085e-6f};
    float ozoneAbsorptionWidth = 15000.0f;

    // Rayleigh density falloff, per metre.
    float rayleighDensityFalloff = 8000.0f;
    // Mie density falloff and its width, per metre. Mie is concentrated hard
    // against the ground, which is why haze sits in the first few hundred
    // metres and a mountain top is clearer than a valley.
    float mieDensityFalloff = 1200.0f;

    // Turbidity: a multiplier on the aerosol terms in aerial perspective only.
    //
    // Deliberately not physical, and deliberately not applied to the sky. The
    // measured aerosol optical depth gives a visibility of roughly 460 km,
    // which is correct and useless: a 460 km view has no distance cue at all
    // until it is already a building too small to see. This is a legibility
    // control on the fog, tuned to about 20 km, and the sky is left alone
    // because doubling its Mie is the difference between a clear day and
    // permanent overcast.
    float hazeScale = 20.0f;

    // Planet and atmosphere radii, in metres. Earth's actual values: the ratio
    // between them is what sets how much atmosphere there is to look through.
    float planetRadius = 6360e3f;
    float atmosphereRadius = 6420e3f;
};

/** What the atmosphere is doing at one instant. */
struct SkyState {
    /** Unit vector toward the sun. Y is up. */
    Vec3 sunDirection{0.0f, 1.0f, 0.0f};
    /** Direct sunlight at the top of the atmosphere, per channel. */
    Vec3 sunIrradiance{1.0f, 1.0f, 1.0f};
    /** The sky's own contribution to a surface facing up, per channel. */
    Vec3 skyAmbient{0.0f, 0.0f, 0.0f};
    /** The ground's bounce onto a surface facing down. */
    Vec3 groundAmbient{0.0f, 0.0f, 0.0f};
    /** Colour of the air at the horizon in the direction of the sun. */
    Vec3 horizonColor{0.0f, 0.0f, 0.0f};
    /** 0 at midnight, 1 at solar noon. Drives every other night-only effect. */
    float dayFactor = 1.0f;
    /** Cosine of the sun's angle above the horizon. Negative below it. */
    float sunElevationCos = 0.0f;
    /** True when the sun has set far enough that street lighting should be on. */
    bool isNight = false;
};

/**
 * The sun's direction for a time of day.
 *
 * Deliberately not a real solar position: it sweeps a great circle and takes
 * `dayFraction` in [0,1) around the clock, which is what a game wants and not
 * what an ephemeris provides. Doing the real thing correctly needs a date, a
 * latitude, a longitude and a timezone, and produces a sun that moves at a rate
 * the player will never notice but the code will be full of.
 */
Vec3 sunDirection(float dayFraction, float latitudeRadians = 0.9f);

/**
 * The full sky state for a direction and a time of day.
 *
 * `viewDirection` need not be normalised. `includeSunDisc` is off for the
 * ambient terms, because the disc is a singularity that would dominate every
 * integral it appeared in.
 */
SkyState evaluateSky(const Vec3 &viewDirection, float dayFraction,
                     const AtmosphereConstants &constants = AtmosphereConstants());

// Scale factors between the model's arbitrary radiance units and a display
// range. The absolute radiance of the sky is not what the test asserts and not
// what the tonemapper consumes; only ratios are physical, so the scale is
// chosen once here rather than being tuned per shader.
constexpr float kAmbientGain = 3.0f;
constexpr float kInscatterGain = 0.06f;

/**
 * Aerial perspective: the colour a surface fades toward at a distance.
 *
 * This is the term that makes a city look deep. Without it every object is
 * equally crisp to the horizon and the world reads as a flat card; with it,
 * distance is legible as colour and the far side of a district sits behind
 * the near side.
 *
 * The extinction follows an exponential density integral rather than a linear
 * fog factor, so haze thickens with the square of distance in the visible
 * range and a building 2 km away is genuinely washed out rather than 20% hazed.
 */
struct AerialState {
    /** The colour distant geometry converges to. */
    Vec3 inscatterColor{0.0f, 0.0f, 0.0f};
    /** How much of the original surface survives, per channel. */
    Vec3 transmittance{1.0f, 1.0f, 1.0f};
    /** Scalar fog amount, for shaders that want one factor. */
    float fogAmount = 0.0f;
};

/**
 * Aerial perspective between the camera and a point.
 *
 * @param distance      Metres from the camera.
 * @param sunDir        Toward the sun; the inscatter is warmer when it is low.
 * @param sunColor      Direct sunlight, so the haze warms at sunset for the same
 *                      reason the sky does rather than from a painted constant.
 * @param dayFactor     From SkyState, so night fogs toward a city glow rather
 *                      than toward black.
 * @param viewDirection The camera's look direction. Only its elevation matters:
 *                      a ray pitched at the sky climbs out of the aerosol layer
 *                      and sees far less of it than a level one.
 * @param cameraHeight  Metres above the ground the haze sits on.
 */
AerialState evaluateAerial(float distance, const Vec3 &sunDir, const Vec3 &sunColor,
                           float dayFactor, const Vec3 &viewDirection = Vec3{0.0f, 0.0f, -1.0f},
                           float cameraHeight = 2.0f,
                           const AtmosphereConstants &constants = AtmosphereConstants());

/**
 * The colour the night sky shows where the sun is not.
 *
 * A separate model, because the scattering integral at night is dominated by
 * numerical noise: with the sun below the horizon the in-scattering is a
 * difference of two nearly equal large numbers, and a single-precision
 * implementation returns something that flickers as the camera turns. Real
 * engines handle this by switching models, and so does this.
 *
 * It is a zenith-to-horizon gradient with a deliberate warm band at the
 * horizon, which is urban sky glow: a city is never actually dark, and a sky
 * that goes to pure black above a lit street is the single clearest "this is a
 * programmer's night" tell there is.
 */
Vec3 nightSkyColor(const Vec3 &viewDirection, float dayFactor);

}  // namespace emergent
