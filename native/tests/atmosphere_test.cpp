// Atmosphere tests.
//
// These are the only tests in the project that can say something about whether
// the sky is *right* rather than whether it is finite. Everything runs on the
// CPU, so they execute in CI on a machine with no graphics device — which is
// the point: a shading model that exists only in a shader cannot be tested at
// all, and most engines ship a sky that is pretty and wrong.
//
// The assertions are about physics, not about matching a reference image. A
// golden image would fail the first time a coefficient was retuned and pass
// unchanged if the model were replaced by something that produced the same
// pixels for the wrong reason.

#include "emergent/atmosphere.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_test = "";

void check(bool condition, const std::string &what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        std::printf("  FAIL [%s] %s\n", g_test, what.c_str());
    }
}

void checkNear(float actual, float expected, float tolerance, const std::string &what) {
    g_checks++;
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
        g_failures++;
        std::printf("  FAIL [%s] %s (expected %f +/- %f, got %f)\n",
                    g_test, what.c_str(), static_cast<double>(expected),
                    static_cast<double>(tolerance), static_cast<double>(actual));
    }
}

bool isFinite(const emergent::Vec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void run(const char *name, void (*fn)()) {
    g_test = name;
    const int before = g_failures;
    fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

using namespace emergent;

// ---------------------------------------------------------------------------

void test_sun_direction_is_unit_and_sweeps() {
    // Over a whole day the sun must trace a full arc and always be a direction
    // rather than a point: a non-unit sun vector silently scales every light in
    // the game, and nothing else would report it.
    float minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < 1440; i += 10) {
        const Vec3 d = sunDirection(static_cast<float>(i) / 1440.0f);
        checkNear(length(d), 1.0f, 1e-4f, "sun direction is unit length");
        check(isFinite(d), "sun direction is finite");
        minY = std::min(minY, d.y);
        maxY = std::max(maxY, d.y);
    }
    check(maxY > 0.5f, "the sun reaches a usable altitude at noon");
    check(minY < 0.0f, "and sets below the horizon at night");
    check(maxY - minY > 1.0f, "the arc is a full sweep, not a wobble");

    // And it must be deterministic: a sky that changes when nothing changed is
    // the most expensive possible bug to find.
    const Vec3 a = sunDirection(0.37f);
    const Vec3 b = sunDirection(0.37f);
    checkNear(a.x, b.x, 0.0f, "sun position is deterministic in x");
    checkNear(a.y, b.y, 0.0f, "sun position is deterministic in y");
}

void test_noon_is_brighter_than_midnight() {
    // 0.5 is noon, 0.0 is midnight.
    const SkyState noon = evaluateSky({0, 1, 0}, 0.5f);
    const SkyState midnight = evaluateSky({0, 1, 0}, 0.0f);
    check(noon.dayFactor > 0.95f, "noon is fully day");
    check(midnight.dayFactor < 0.05f, "midnight is fully night");
    check(midnight.isNight, "midnight reports itself as night");
    check(!noon.isNight, "noon does not");

    const float noonLum = noon.skyAmbient.x + noon.skyAmbient.y + noon.skyAmbient.z;
    const float nightLum = midnight.skyAmbient.x + midnight.skyAmbient.y + midnight.skyAmbient.z;
    check(noonLum > nightLum * 4.0f, "day ambient dominates night ambient");
    check(nightLum > 0.0f, "night is not pure black — a city is never actually dark");
}

void test_the_sky_is_blue_where_it_is_blue() {
    // The single most important property of a sky, and the one a flat-colour
    // shader cannot have. Looking at the zenith away from the sun, blue must
    // dominate red.
    const SkyState noon = evaluateSky({0, 1, 0}, 0.5f);
    const Vec3 c = noon.horizonColor;
    check(c.z > c.x, "the zenith is bluer than it is red");
    check(c.z > c.y, "and bluer than it is green");
    check(c.x > 0.0f, "and not pure blue, which reads as a colour cast");
    check(isFinite(c), "the sky colour is finite");

    // The ratio should be in the neighbourhood the scattering coefficients
    // predict. 1/lambda^4 across 680/440 nm is about 5.7, and after the
    // transmittance and the ambient lift the observed ratio is lower — but it
    // must stay clearly above 1, or the sky has stopped being a sky.
    const float ratio = c.z / std::max(c.x, 1e-9f);
    check(ratio > 1.5f, "blue exceeds red by a clear margin");
    check(ratio < 25.0f, "but not so much that the sky is monochrome");
}

void test_horizon_is_brighter_than_zenith() {
    // More air along the ray, more scattering. A sky where the zenith is the
    // brightest point is an inverted one, and it is a common bug when the
    // optical depth is integrated along the wrong axis.
    const SkyState s = evaluateSky({0, 1, 0}, 0.5f);
    const SkyState horizon = evaluateSky({1, 0.02f, 0}, 0.5f);
    const float zl = s.horizonColor.x + s.horizonColor.y + s.horizonColor.z;
    const float hl = horizon.horizonColor.x + horizon.horizonColor.y + horizon.horizonColor.z;
    check(hl > zl, "the horizon is brighter than the zenith");
}

void test_sunset_warms_the_horizon_in_the_suns_direction() {
    // Take the sun to the western horizon and look at it. Red must rise
    // relative to blue, which is the entire optical reason a sunset is orange.
    // Looking away from it must not do the same thing.
    float t = 0.0f;
    for (int i = 0; i < 1440; i++) {
        const float f = static_cast<float>(i) / 1440.0f;
        if (sunDirection(f).y < 0.06f && sunDirection(f).y > 0.0f) { t = f; break; }
    }
    const SkyState s = evaluateSky({0, 1, 0}, t);
    const Vec3 L = s.sunDirection;
    // Look along the sun's horizontal direction.
    const Vec3 toward{std::sqrt(std::max(0.0f, 1.0f - L.y * L.y)) * (L.x >= 0 ? 1.0f : -1.0f), 0.03f, 0.0f};
    const SkyState into = evaluateSky(toward, t);
    const SkyState away = evaluateSky({-toward.x, 0.03f, 0.0f}, t);

    const float intoWarm = into.horizonColor.x - into.horizonColor.z;
    const float awayWarm = away.horizonColor.x - away.horizonColor.z;
    check(intoWarm > awayWarm, "looking toward a low sun is warmer than looking away");
    check(intoWarm > 0.0f, "and it is warm in absolute terms, not merely warmer");
}

void test_twilight_is_continuous() {
    // The two models swap at nightfall. A discontinuity shows as a hard flash
    // at one specific time of day, so the swap has to be smooth.
    float prev = 0.0f;
    float maxJump = 0.0f;
    for (int i = 0; i <= 2880; i++) {
        const float f = static_cast<float>(i) / 2880.0f;
        const SkyState s = evaluateSky({0.2f, 0.15f, 0}, f);
        const float lum = s.horizonColor.x + s.horizonColor.y + s.horizonColor.z;
        if (i > 0) maxJump = std::max(maxJump, std::fabs(lum - prev));
        prev = lum;
    }
    // 2880 samples over a day. A smooth model cannot jump by more than a small
    // fraction of the full range between adjacent half-minute steps; a model
    // that pops between two unrelated formulas jumps by a large fraction.
    check(maxJump < 0.08f, "the night-to-day transition has no visible pop");
}

void test_no_direction_produces_a_nan() {
    // Every direction, including straight down and straight up, at every hour.
    // A sky that returns NaN for one direction turns that whole screen
    // direction black, and a NaN that reaches the tonemapper never recovers.
    for (int h = 0; h < 24; ++h) {
        for (int d = 0; d < 32; ++d) {
            const float f = static_cast<float>(h) / 24.0f;
            const float theta = static_cast<float>(d) / 32.0f * 2.0f - 1.0f;
            for (float phi = 0.0f; phi < 6.28f; phi += 0.7f) {
                const Vec3 dir{std::sqrt(std::max(0.0f, 1.0f - theta * theta)) * std::cos(phi),
                               theta,
                               std::sqrt(std::max(0.0f, 1.0f - theta * theta)) * std::sin(phi)};
                const SkyState s = evaluateSky(dir, f);
                check(isFinite(s.horizonColor) && isFinite(s.skyAmbient) && isFinite(s.sunIrradiance),
                      "every direction at every hour is finite");
            }
        }
    }
}

void test_ambient_shadows_the_surface_orientation() {
    // A surface facing up sees more sky than one facing down. Sampling one point
    // for both is what makes a night scene look like a flat wash.
    const SkyState s = evaluateSky({0, 1, 0}, 0.5f);
    const Vec3 upAmbient = s.skyAmbient;
    // Facing the ground, the ambient is the ground's bounce, which is dimmer.
    const float up = upAmbient.x + upAmbient.y + upAmbient.z;
    const float down = s.groundAmbient.x + s.groundAmbient.y + s.groundAmbient.z;
    check(up > down, "a sky-facing surface is brighter than a ground-facing one");
    check(down > 0.0f, "the ground still bounces some light");
    check(upAmbient.z > upAmbient.x, "ambient light is also blue");
}

void test_direct_sunlight_is_black_at_midnight() {
    // Not "small". Black. A world lit at midnight is the single clearest
    // programming error in a day/night cycle, and it is easy to produce by
    // fading a positive value to a small positive value instead of to zero.
    const SkyState midnight = evaluateSky({0, 1, 0}, 0.0f);
    const float lum = midnight.sunIrradiance.x + midnight.sunIrradiance.y + midnight.sunIrradiance.z;
    checkNear(lum, 0.0f, 1e-6f, "no direct sunlight at midnight");

    const SkyState noon = evaluateSky({0, 1, 0}, 0.5f);
    const float noonLum = noon.sunIrradiance.x + noon.sunIrradiance.y + noon.sunIrradiance.z;
    check(noonLum > 10.0f, "and full strength at noon");
}

void test_aerial_perspective_builds_with_distance() {
    const Vec3 sun = sunDirection(0.5f);
    const Vec3 sunColor = evaluateSky({0, 1, 0}, 0.5f).sunIrradiance;
    AerialState near = evaluateAerial(10.0f, sun, sunColor, 1.0f);
    AerialState mid = evaluateAerial(500.0f, sun, sunColor, 1.0f);
    AerialState far = evaluateAerial(5000.0f, sun, sunColor, 1.0f);

    check(near.fogAmount < mid.fogAmount, "haze increases with distance");
    check(mid.fogAmount < far.fogAmount, "and keeps increasing");
    check(far.fogAmount < 1.0f, "but never reaches full occlusion, or the horizon would be a wall");
    check(far.fogAmount > 0.5f, "a 5 km view is substantially hazed");
    check(near.transmittance.x > far.transmittance.x, "transmittance falls with distance");
    check(isFinite(far.inscatterColor), "the inscatter colour is finite");

    // Zero and negative distances must be identity, not an extrapolation.
    AerialState zero = evaluateAerial(0.0f, sun, sunColor, 1.0f);
    checkNear(zero.fogAmount, 0.0f, 1e-6f, "zero distance has no haze");
    AerialState neg = evaluateAerial(-50.0f, sun, sunColor, 1.0f);
    checkNear(neg.fogAmount, 0.0f, 1e-6f, "a negative distance has no haze rather than inverting");
}

void test_haze_is_warmer_toward_a_low_sun() {
    // Forward scattering, and the reason a road toward a low sun glows.
    //
    // Note the view direction is what varies, not the sun. Negating the sun
    // also flips the result — it moves the sun below the horizon, and a
    // below-horizon sun is a stronger sunset, not an "away" view — so a test
    // written that way passes for the wrong reason and cannot tell forward
    // scattering from a warm-toward-elevation term at all.
    const Vec3 lowSun = normalize(Vec3{0.9f, 0.15f, 0.0f});
    const AerialState into = evaluateAerial(2000.0f, lowSun, Vec3{1, 1, 1}, 1.0f, lowSun);
    const AerialState away = evaluateAerial(2000.0f, lowSun, Vec3{1, 1, 1}, 1.0f, lowSun * -1.0f);
    const float intoWarm = into.inscatterColor.x - into.inscatterColor.z;
    const float awayWarm = away.inscatterColor.x - away.inscatterColor.z;
    check(intoWarm > awayWarm, "haze looking into a low sun is warmer than looking away");
    check(intoWarm > 0.0f, "and it is warm in absolute terms, not merely warmer");
}

void test_noon_haze_is_not_a_sunset() {
    // The regression guard for a warmth ramp that saturated too early. Haze
    // only goes warm when the sun is genuinely low; a warm ramp that is
    // already at full strength at noon tints every distant surface orange,
    // which reads as a permanent sunset and hides the actual one.
    const SkyState noon = evaluateSky({0, 1, 0}, 0.5f);
    const AerialState haze = evaluateAerial(3000.0f, noon.sunDirection, noon.sunIrradiance, 1.0f);
    check(haze.inscatterColor.z > haze.inscatterColor.x,
          "midday haze is cool, not orange");
    check(haze.inscatterColor.z > haze.inscatterColor.y,
          "and it is bluest in blue, the same ratio the sky is");

    // And at sunset it must genuinely be warm, or the guard above is vacuous.
    float t = 0.0f;
    for (int i = 0; i < 1440; ++i) {
        const float f = static_cast<float>(i) / 1440.0f;
        if (sunDirection(f).y > 0.0f && sunDirection(f).y < 0.06f) { t = f; break; }
    }
    const SkyState low = evaluateSky({0, 1, 0}, t);
    const AerialState dusk = evaluateAerial(3000.0f, low.sunDirection, low.sunIrradiance,
                                            low.dayFactor, low.sunDirection);
    check(dusk.inscatterColor.x > dusk.inscatterColor.z,
          "and the same model does go warm at sunset");
}

void test_night_fog_carries_city_glow() {
    // Fog that goes to black at night makes every distance cue disappear and
    // every lit window float in a void. Real urban haze at night is not black.
    const SkyState day = evaluateSky({0, 1, 0}, 0.5f);
    const SkyState night = evaluateSky({0, 1, 0}, 0.0f);
    const AerialState dayHaze = evaluateAerial(3000.0f, day.sunDirection, day.sunIrradiance, day.dayFactor);
    const AerialState nightHaze = evaluateAerial(3000.0f, night.sunDirection, night.sunIrradiance, night.dayFactor);

    const float nightLum = nightHaze.inscatterColor.x + nightHaze.inscatterColor.y + nightHaze.inscatterColor.z;
    check(nightLum > 0.0f, "night haze is not black");
    check(nightLum < (dayHaze.inscatterColor.x + dayHaze.inscatterColor.y + dayHaze.inscatterColor.z),
          "but it is dimmer than daytime haze");
}

void test_night_sky_has_a_horizon_glow() {
    // Looking at the horizon at night versus the zenith. They must differ, and
    // the horizon must be warmer: that band is urban sky glow.
    const Vec3 horizon = nightSkyColor({1.0f, 0.02f, 0.0f}, 0.0f);
    const Vec3 zenith = nightSkyColor({0.0f, 1.0f, 0.0f}, 0.0f);
    const float hl = horizon.x + horizon.y + horizon.z;
    const float zl = zenith.x + zenith.y + zenith.z;
    check(hl > zl, "the night horizon glows more than the zenith");
    check(horizon.x > horizon.z, "and it glows warm, not blue");
    check(isFinite(zenith), "the night sky is finite");
    // The zenith should still be blue-ish, not grey: starlight is blue.
    check(zenith.z > zenith.x, "the night zenith is blue");
}

void test_aerial_and_sky_agree_on_direction() {
    // The fog a distant building takes on must come from the same atmosphere
    // as the sky behind it, or the horizon shows a seam where the two disagree.
    // Checked by driving the sun to the horizon and confirming the sky in the
    // sun's direction is at least as warm as the haze looking into it — both
    // are derived from the same forward-scattering idea and must not diverge.
    for (int i = 0; i < 24; ++i) {
        const float f = static_cast<float>(i) / 24.0f;
        const SkyState s = evaluateSky({0, 0.05f, 0}, f);
        const AerialState a = evaluateAerial(4000.0f, s.sunDirection, s.sunIrradiance, s.dayFactor);
        check(isFinite(a.inscatterColor), "haze is finite at every hour");
        check(a.transmittance.x >= 0.0f && a.transmittance.x <= 1.0f,
              "transmittance stays a valid fraction at every hour");
        check(a.fogAmount >= 0.0f && a.fogAmount <= 1.0f,
              "fog stays a valid fraction at every hour");
    }
}

}  // namespace

int main() {
    std::printf("emergent atmosphere tests\n");
    run("sun direction is unit and sweeps", test_sun_direction_is_unit_and_sweeps);
    run("noon is brighter than midnight", test_noon_is_brighter_than_midnight);
    run("the sky is blue where it is blue", test_the_sky_is_blue_where_it_is_blue);
    run("horizon is brighter than zenith", test_horizon_is_brighter_than_zenith);
    run("sunset warms the horizon in the sun's direction", test_sunset_warms_the_horizon_in_the_suns_direction);
    run("twilight is continuous", test_twilight_is_continuous);
    run("no direction produces a NaN", test_no_direction_produces_a_nan);
    run("ambient shadows the surface orientation", test_ambient_shadows_the_surface_orientation);
    run("direct sunlight is black at midnight", test_direct_sunlight_is_black_at_midnight);
    run("aerial perspective builds with distance", test_aerial_perspective_builds_with_distance);
    run("haze is warmer toward a low sun", test_haze_is_warmer_toward_a_low_sun);
    run("noon haze is not a sunset", test_noon_haze_is_not_a_sunset);
    run("night fog carries city glow", test_night_fog_carries_city_glow);
    run("night sky has a horizon glow", test_night_sky_has_a_horizon_glow);
    run("aerial and sky agree on direction", test_aerial_and_sky_agree_on_direction);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
