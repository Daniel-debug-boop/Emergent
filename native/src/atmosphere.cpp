#include "emergent/atmosphere.hpp"

#include <cmath>

namespace emergent {
namespace {

constexpr float kPi = 3.14159265358979f;

// The cosine of the sun's zenith angle at which the day/night test fires, and
// the matching "street lights on" angle. They are different on purpose: it is
// genuinely light out well after the sun has set, and it is genuinely dark
// before it has risen. A single threshold cannot be right for both.
//
// -0.0175 is 1 degree below the horizon and -0.1045 is 6 degrees below, which
// is the standard definition of the end of civil twilight. The day factor is
// the fraction of that band that has elapsed, which is why it reaches exactly
// 1.0 in full daylight and exactly 0.0 in full night regardless of latitude:
// at 52 degrees the sun never reaches the zenith, and a day factor defined
// against the zenith would never leave 0.8.
constexpr float kSunsetCos = -0.0175f;
constexpr float kNightCos = -0.1045f;

// Samples along a view ray. Spaced quadratically rather than uniformly, so the
// first sample is a few hundred metres out and the near field — which is most
// of what a viewer actually looks at — is resolved properly. 32 of them is
// enough that the result is converged to well under a percent; the test asserts
// the physical properties rather than a golden value, so the count is a
// performance knob and not something to tune against an image.
constexpr int kScatterSteps = 32;

// Ozone is its own layer: it does not follow the exponential air density, it
// peaks in the stratosphere. Using the air density for it puts the ozone in the
// wrong place entirely, which is what removes twilight's blue.
float ozoneDensity(float heightMeters) {
    return std::max(0.0f, 1.0f - std::fabs(heightMeters - 25000.0f) / 15000.0f);
}

float rayleighPhase(float cosTheta) {
    // 3/(16*pi) * (1 + cos^2)
    return (3.0f / (16.0f * kPi)) * (1.0f + cosTheta * cosTheta);
}

float miePhase(float cosTheta, float g) {
    // Cornette-Shanks. Chosen over plain Rayleigh for Mie because it has the
    // forward lobe: without it there is no sun glare, and a sky with no glare
    // has no sunset.
    const float g2 = g * g;
    const float num = 3.0f * (1.0f - g2) * (1.0f + cosTheta * cosTheta);
    const float den = 8.0f * kPi * (2.0f + g2) *
                      std::pow(std::max(1e-4f, 1.0f + g2 - 2.0f * g * cosTheta), 1.5f);
    return den > 1e-8f ? num / den : 0.0f;
}

/** Intersect a ray from `origin` with a sphere of `radius` at the origin. */
bool raySphere(const Vec3 &origin, const Vec3 &dir, float radius, float &t0, float &t1) {
    const float b = 2.0f * dot(origin, dir);
    const float c = dot(origin, origin) - radius * radius;
    const float disc = b * b - 4.0f * c;
    if (disc < 0.0f) return false;
    const float s = std::sqrt(disc);
    t0 = (-b - s) * 0.5f;
    t1 = (-b + s) * 0.5f;
    return true;
}

/**
 * Relative air mass along a ray, from Kenny's fit to the Chapman function.
 *
 * This is the closed form that makes the whole thing affordable: the exact
 * spherical-shell integral has no parameter to tune but is a transcendental
 * solve per sample, whereas this is three multiplies. The guard on the base
 * matters more than the accuracy does — `pow` of a negative number by a
 * fractional exponent is NaN, and a NaN here silently blacks out every
 * downward-facing direction in the sky.
 */
float airMass(float cosZenith) {
    // Below the horizon there is no single air mass, because the ray hits the
    // ground before it leaves the air. Clamp to the grazing value: the caller
    // clamps the path length separately.
    const float mu = cosZenith < 0.0175f ? 0.0175f : (cosZenith > 1.0f ? 1.0f : cosZenith);
    const float degrees = std::acos(mu) * 180.0f / kPi;
    const float base = std::max(0.5f, 96.07995f - degrees);
    return 1.0f / (mu + 0.50572f * std::pow(base, -1.6364f));
}

/**
 * Transmittance from a point to the top of the atmosphere along the sun ray.
 *
 * Zero when the planet is in the way. That test is not a formality: it is the
 * only reason the twilight sky is blue rather than black, because the samples
 * at altitude are the ones that still see a sun which has set for the observer.
 */
Vec3 sunTransmittance(const Vec3 &point, const Vec3 &sunDir, const AtmosphereConstants &c) {
    const float radius = length(point);
    const float b = dot(point, sunDir);
    const float cosZenith = b / std::max(radius, 1e-6f);

    // Closest approach of the sun ray to the planet centre, and whether it is
    // inside the planet. Only a ray pointing below the local horizontal can
    // intersect, so the upward case short-circuits.
    if (cosZenith < 0.0f) {
        const float closest = radius * radius - b * b;
        if (closest < c.planetRadius * c.planetRadius) return {0.0f, 0.0f, 0.0f};
    }

    const float a = airMass(cosZenith);
    // Column densities in metres of equivalent uniform atmosphere. Ozone's
    // column is the area of its triangular profile, not a scale height.
    const float columnRayleigh = c.rayleighDensityFalloff * a;
    const float columnMie = c.mieDensityFalloff * a;
    const float columnOzone = c.ozoneAbsorptionWidth * a;

    const Vec3 tau = c.rayleighScattering * columnRayleigh +
                     c.mieAbsorption * columnMie +
                     c.ozoneAbsorption * columnOzone;
    return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
}

/**
 * Single-scattering sky radiance along a view direction.
 *
 * Marches the ray, accumulating in-scattered light and attenuating what is
 * left of the sun behind it. The extinction and the in-scatter use different
 * coefficients on purpose: scattering is what you see, extinction is what is
 * removed, and a model that uses the same number for both produces a sky whose
 * brightness is independent of air thickness.
 */
Vec3 skyRadiance(const Vec3 &origin, const Vec3 &dir, const Vec3 &sunDir,
                 const Vec3 &sunIrradiance, const AtmosphereConstants &c) {
    // How far to march: to the ground if the ray goes down, otherwise until
    // the air has thinned out. Beyond a few hundred kilometres there is
    // nothing left to integrate.
    float t0 = 0.0f, t1 = 0.0f;
    float tMax = 300000.0f;
    if (raySphere(origin, dir, c.atmosphereRadius, t0, t1) && t1 > 0.0f) {
        tMax = std::min(tMax, t1);
    }
    if (raySphere(origin, dir, c.planetRadius, t0, t1) && t1 > 0.0f) {
        // Stop a metre short so the final sample is not inside the ground.
        tMax = std::min(tMax, std::max(0.0f, t1 - 1.0f));
    }
    if (!(tMax > 0.0f)) return {0.0f, 0.0f, 0.0f};

    const float cosTheta = dot(dir, sunDir);
    const float phaseRayleigh = rayleighPhase(cosTheta);
    const float phaseMie = miePhase(cosTheta, 0.76f);

    Vec3 scattered{0.0f, 0.0f, 0.0f};
    Vec3 through{1.0f, 1.0f, 1.0f};
    for (int i = 0; i < kScatterSteps; ++i) {
        // Quadratic spacing: dense near the eye, sparse at the horizon.
        const float f0 = static_cast<float>(i) / static_cast<float>(kScatterSteps);
        const float f1 = static_cast<float>(i + 1) / static_cast<float>(kScatterSteps);
        const float near = tMax * f0 * f0;
        const float far = tMax * f1 * f1;
        const float step = far - near;
        if (!(step > 0.0f)) continue;
        const float t = 0.5f * (near + far);
        const Vec3 p = origin + dir * t;
        const float height = std::max(0.0f, length(p) - c.planetRadius);
        const float densityRayleigh = std::exp(-height / c.rayleighDensityFalloff);
        const float densityMie = std::exp(-height / c.mieDensityFalloff);
        const float densityOzone = ozoneDensity(height);

        // Scattering in, and everything out. Ozone is pure absorption.
        const Vec3 scatterR = c.rayleighScattering * (densityRayleigh * step);
        const float scatterM = c.mieScattering * (densityMie * step);
        const Vec3 extinction = scatterR + scatterM +
                                c.mieAbsorption * (densityMie * step) +
                                c.ozoneAbsorption * (densityOzone * step);

        // Only light that survives the path from the sun to this sample, and
        // that survives the path from this sample to the eye, reaches the eye.
        const Vec3 toSun = sunTransmittance(p, sunDir, c);
        const Vec3 contribution = scatterR * (phaseRayleigh * toSun) +
                                  Vec3{scatterM * (phaseMie * toSun.x),
                                       scatterM * (phaseMie * toSun.y),
                                       scatterM * (phaseMie * toSun.z)};
        scattered = scattered + through * contribution;
        through = {through.x * std::exp(-extinction.x),
                   through.y * std::exp(-extinction.y),
                   through.z * std::exp(-extinction.z)};
    }

    return scattered * sunIrradiance;
}

/** Smoothstep on [edge0, edge1], with edge0 == edge1 treated as a step. */
float smoothstep(float edge0, float edge1, float x) {
    if (!(edge1 > edge0)) return x < edge0 ? 0.0f : 1.0f;
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

Vec3 sunDirection(float dayFraction, float latitudeRadians) {
    // One full sweep of the clock, wrapped, so a caller can pass elapsed seconds
    // and get a stable answer without ever handling a wrap.
    float t = dayFraction;
    t = t - std::floor(t);
    const float hourAngle = (t * 2.0f - 1.0f) * kPi;
    const float declination = 0.21f;  // a mild seasonal offset, ~12 degrees

    const float sinAlt = std::sin(latitudeRadians) * std::sin(declination) +
                         std::cos(latitudeRadians) * std::cos(declination) * std::cos(hourAngle);
    // Clamp to [-1, 1] and take the real arcsine. Clamping to [0, 1] here —
    // which is what a saturate() reflex does — silently floors every
    // below-horizon sun at the horizon, and the world is then lit at midnight.
    const float altitude = std::asin(std::max(-1.0f, std::min(1.0f, sinAlt)));
    const float azimuth = hourAngle;

    // Azimuth 0 is -Z, and Y is up, so the noon sun is high and slightly
    // behind the origin rather than exactly overhead. A sun directly overhead
    // never produces a shadow anyone can see.
    const float ca = std::cos(altitude);
    return normalize({ca * std::sin(azimuth), std::sin(altitude), -ca * std::cos(azimuth)});
}

SkyState evaluateSky(const Vec3 &viewDirection, float dayFraction, const AtmosphereConstants &c) {
    SkyState s;
    s.sunDirection = sunDirection(dayFraction);
    const Vec3 V = normalize(viewDirection);
    const Vec3 L = s.sunDirection;

    const float sunCos = L.y;
    s.sunElevationCos = sunCos;

    // 0 at the end of civil twilight, 1 in full day, with a smooth ramp between
    // the two solar altitudes that actually mean something.
    s.dayFactor = saturate((sunCos - kNightCos) / (kSunsetCos - kNightCos));
    s.isNight = sunCos < kNightCos;

    // Direct sunlight is attenuated by the air between the sun and the point.
    // Below the horizon there is no direct sunlight at all, and letting a small
    // positive value through is what produces a world lit at midnight.
    const float horizonFade = smoothstep(kSunsetCos, kSunsetCos + 0.10f, sunCos);
    const float directStrength = 22.0f * horizonFade;
    s.sunIrradiance = {directStrength, directStrength * 0.96f, directStrength * 0.88f};

    // Observer at the surface.
    const Vec3 origin{0.0f, c.planetRadius + 2.0f, 0.0f};

    // The day model evaluated with a sun that is on or below the horizon. The
    // samples high in the air still see it, which is where the twilight glow
    // and its blue come from — a model that switches the moment the observer's
    // sun sets throws all of that away.
    const Vec3 dayColor = skyRadiance(origin, V, L, s.sunIrradiance, c);

    // The night model, faded in across the twilight band. Cross-fading rather
    // than switching is what stops a hard flash at one specific time of day,
    // and it is a real problem: two unrelated formulas meet at the horizon and
    // a step between them is visible from across a city.
    const float nightBlend = 1.0f - smoothstep(kSunsetCos, kNightCos, sunCos);
    s.horizonColor = lerp(dayColor, nightSkyColor(V, s.dayFactor), nightBlend);

    // Ambient: the sky's own contribution, integrated over the hemisphere the
    // surface can see rather than sampled at a point. A surface facing a dark
    // patch of sky and one facing the bright horizon are not lit the same, and
    // treating them as such is what makes a night scene look like a flat wash.
    //
    // Eight directions is enough for a hemispherical average and costs less
    // than the alternative of getting it visibly wrong.
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; ++i) {
        const float phi = static_cast<float>(i) * (kPi / 4.0f);
        const float e = 0.35f;
        const Vec3 d{std::cos(phi) * e, std::sqrt(std::max(0.0f, 1.0f - e * e)), std::sin(phi) * e};
        const Vec3 c2 = skyRadiance(origin, d, L, s.sunIrradiance, c);
        const float w = nightBlend * 0.0f + 1.0f;
        ambient = ambient + c2 * w;
        const Vec3 night = nightSkyColor(d, s.dayFactor);
        ambient = ambient + (night - c2) * nightBlend;
    }
    s.skyAmbient = ambient * (1.0f / 8.0f) * kAmbientGain;
    const Vec3 groundBounce{0.030f, 0.028f, 0.026f};
    s.groundAmbient = groundBounce * s.dayFactor;

    return s;
}

AerialState evaluateAerial(float distance, const Vec3 &sunDir, const Vec3 &sunColor,
                           float dayFactor, const Vec3 &viewDirection,
                           float cameraHeight, const AtmosphereConstants &c) {
    AerialState a;
    if (!(distance > 0.0f)) {
        a.inscatterColor = sunColor;
        a.transmittance = {1.0f, 1.0f, 1.0f};
        return a;
    }

    // Haze is not a percentage of distance. It is a column of aerosol, and the
    // column along a ray depends on the height that ray climbs through — which
    // is why a valley is hazier than a ridge at the same distance, and why a
    // camera pitched at the sky sees much less of it than one level with the
    // ground. Integrating the exponential along the ray is exact:
    //     integral of exp(-(h0 + c1*t)/H) dt = H/c1 * (1 - exp(-c1*d/H))
    // with a removable singularity at c1 = 0 (a level ray) that has to be taken
    // as a limit rather than divided through.
    const float h = c.mieDensityFalloff;
    const float climb = std::max(1e-3f, normalize(viewDirection).y);
    const float mieColumn =
        std::exp(-std::max(0.0f, cameraHeight) / h) * (h / climb) * (1.0f - std::exp(-climb * distance / h));
    const float rayleighColumn = distance;

    // Turbidity is a gameplay control, not a physical constant: `hazeScale`
    // turns a genuinely pristine 460 km visibility into a legible 20 km one.
    const float tauMie = (c.mieScattering + c.mieAbsorption) * c.hazeScale * mieColumn;
    const Vec3 tau = c.rayleighScattering * rayleighColumn + tauMie;
    a.transmittance = {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};

    // Inscatter grows with distance and saturates at the colour of the air
    // itself, so "very far" is a colour rather than an ever-darker fog.
    const float meanTau = (tau.x + tau.y + tau.z) / 3.0f;
    const float saturation = 1.0f - std::exp(-meanTau);
    const Vec3 L = normalize(sunDir);
    const Vec3 V = normalize(viewDirection);

    // Warmth has two independent causes and they are not the same thing. The
    // first is that a low sun's light crosses more atmosphere before it reaches
    // the aerosol, so what scatters is already red. The second is forward
    // scattering: the aerosol lobe points at the sun, so a view aimed into a
    // low sun collects more of it than a view aimed away.
    //
    // Ramping on the sun's actual elevation rather than on a remapped 0..1 of
    // it matters, and the difference is visible: a ramp that has already
    // saturated by the time the sun is 30 degrees up makes every noon haze
    // orange, which is a sunset.
    const float lowSun = saturate(1.0f - L.y / 0.35f);
    const float forward = saturate(dot(V, L) * 2.0f + 0.5f);
    const float warmth = saturate(lowSun * (0.45f + 0.55f * forward));

    const float sunLum = std::max(1e-4f, sunColor.x + sunColor.y + sunColor.z);
    const Vec3 warm{0.95f, 0.72f, 0.48f};
    const Vec3 cool{0.62f, 0.68f, 0.78f};
    const Vec3 tint = cool + (warm - cool) * warmth;
    a.inscatterColor = tint * saturation * (0.06f + 0.94f * dayFactor) * sunLum * kInscatterGain;
    a.fogAmount = saturation;
    return a;
}

Vec3 nightSkyColor(const Vec3 &viewDirection, float dayFactor) {
    const Vec3 V = normalize(viewDirection);

    // Zenith is deep blue-black; the horizon carries a warm sodium band. A sky
    // that goes to pure black above a lit street is the clearest tell that
    // nobody has ever looked out of a city at night.
    const Vec3 zenith{0.006f, 0.009f, 0.020f};
    const Vec3 horizon{0.085f, 0.062f, 0.040f};
    const float horizonBand = std::pow(1.0f - saturate(V.y), 6.0f);

    Vec3 c = lerp(zenith, horizon, horizonBand * 0.85f);
    // `dayFactor` fades the whole night sky out as dawn approaches so there is
    // no visible pop when the two models cross-fade.
    c = c * (0.25f + 0.75f * (1.0f - saturate(dayFactor)));
    return c;
}

}  // namespace emergent
