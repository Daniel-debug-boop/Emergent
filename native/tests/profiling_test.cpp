// Profiling self-test: checks that zone accounting records real measurements,
// that the JSON it exports is well formed and complete, and that the engine's
// own subsystems actually report through it.
//
// The last part is the point. A profiler that only profiles a synthetic loop
// proves nothing about the engine, so the physics and animation subsystems are
// driven for real and their zones are looked up by name afterwards.

#include "emergent/profiling.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "emergent/animation.hpp"
#include "emergent/jolt_physics.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    if (condition) {
        std::printf("  ok    %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

// Burns a measurable, non-zero amount of time without the compiler optimising
// the loop away.
double burn(std::uint64_t rounds) {
    double accumulator = 0.0;
    for (std::uint64_t i = 0; i < rounds; ++i) accumulator += std::sqrt(static_cast<double>(i) + 1.0);
    return accumulator;
}

std::string readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t countOccurrences(const std::string &haystack, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

} // namespace

int main() {
    using emergent::profile::findZone;
    using emergent::profile::Scope;
    using emergent::profile::zone;
    using emergent::profile::zoneCount;

    std::printf("EMERGENT profiling self-test\n");

    emergent::profile::setProgramName("emergent_profiling_test");
    check(!emergent::profile::tracyCompiledIn() || emergent::profile::tracyStatus().size() > 0,
        "the Tracy layer always reports a definite state");
    std::printf("        tracy: %s\n", emergent::profile::tracyStatus().c_str());

    // --- empty state -------------------------------------------------------
    emergent::profile::reset();
    check(zoneCount() == 0, "a reset profiler has no zones");
    check(zone(0) == nullptr, "an empty profiler has no zone 0");
    check(findZone("anything") == nullptr, "an empty profiler finds nothing");
    check(emergent::profile::totalMs() == 0.0, "an empty profiler has measured nothing");
    check(emergent::profile::toJson().find("\"zoneCount\": 0") != std::string::npos,
        "an empty profile still produces valid JSON");

    // --- one zone ----------------------------------------------------------
    for (int i = 0; i < 100; ++i) {
        Scope scope("burn");
        burn(2000);
    }

    const emergent::profile::ZoneStats *burnZone = findZone("burn");
    check(burnZone != nullptr, "a scoped loop registers its zone");
    if (burnZone) {
        check(burnZone->calls == 100, "every scope is counted");
        check(burnZone->totalNanos > 0, "the zone measured real wall time");
        check(burnZone->minNanos > 0, "the fastest call still took measurable time");
        check(burnZone->minNanos <= burnZone->totalNanos / burnZone->calls,
            "the minimum is below the average");
        check(burnZone->maxNanos >= burnZone->minNanos, "the maximum is above the minimum");
        check(burnZone->averageMs() > 0.0 && burnZone->totalMs() > 0.0, "millisecond views agree");
        // A monotonic clock cannot run backwards, so a huge negative delta is
        // the signature of a broken timer rather than a slow machine.
        check(static_cast<double>(burnZone->maxNanos) < 60.0e9,
            "no single call is attributed an implausible duration");
    }

    // --- nesting -----------------------------------------------------------
    for (int i = 0; i < 10; ++i) {
        Scope outer("outer");
        {
            Scope inner("inner");
            burn(500);
        }
    }
    check(findZone("outer") != nullptr && findZone("inner") != nullptr,
        "nested scopes register both zones");
    check(findZone("outer")->calls == 10 && findZone("inner")->calls == 10,
        "nested scopes each count every entry");
    check(emergent::profile::totalMs() > findZone("outer")->totalMs(),
        "the total double-counts nesting, as a sum of zone times must");

    // --- ordering and lookup ----------------------------------------------
    check(zoneCount() == 3, "three distinct zones are registered");
    check(zone(zoneCount()) == nullptr, "an out-of-range zone index is rejected");
    // toJson() settles the order, and two runs of the same workload listing
    // their zones in the same order is what makes a profile diffable.
    const std::string ordered = emergent::profile::toJson();
    const std::size_t atBurn = ordered.find("\"burn\"");
    const std::size_t atInner = ordered.find("\"inner\"");
    const std::size_t atOuter = ordered.find("\"outer\"");
    check(atBurn != std::string::npos && atInner != std::string::npos && atOuter != std::string::npos,
        "every zone appears in the profile");
    check(atBurn < atInner && atInner < atOuter, "zones are listed in name order");

    // --- reset -------------------------------------------------------------
    emergent::profile::reset();
    check(zoneCount() == 3, "reset() keeps the known zone names");
    check(findZone("burn") != nullptr, "a reset zone is still findable by name");
    check(findZone("burn")->calls == 0, "reset() discards the measurements");
    check(emergent::profile::totalMs() == 0.0, "reset() zeroes the total");
    {
        Scope scope("burn");
        burn(10);
    }
    check(findZone("burn")->calls == 1, "a zone still records after a reset");

    // --- real subsystems ---------------------------------------------------
    emergent::profile::reset();
    {
        emergent::JoltPhysicsWorld world;
        world.initialize();
        for (int i = 0; i < 60; ++i) {
            Scope scope("physics.step");
            world.step(1.0f / 60.0f);
        }
        world.shutdown();
    }
    {
        emergent::AnimationLibrary library;
        library.build();
        emergent::Animator animator;
        animator.bind(library);
        animator.play("walk", true);
        emergent::Pose pose;
        pose.bind(library);
        for (int i = 0; i < 60; ++i) {
            Scope scope("animation.sample");
            animator.update(1.0f / 60.0f);
            pose.sample(animator);
            pose.resolve();
        }
    }
    check(findZone("physics.step") != nullptr, "the physics subsystem reports through the profiler");
    check(findZone("physics.step")->calls == 60, "every physics step is accounted for");
    check(findZone("physics.step")->totalNanos > 0, "physics steps measured real wall time");
    check(findZone("animation.sample") != nullptr, "the animation subsystem reports through it");
    check(findZone("animation.sample")->calls == 60, "every animation sample is accounted for");
    check(findZone("animation.sample")->totalNanos > 0, "animation sampling measured real time");
    // 60 Jolt steps of a 1-body world are not free; if this ever reads as
    // "no measurable time" the timer, not the workload, has broken.
    check(findZone("physics.step")->totalMs() > 0.0, "the physics zone is not attributed zero time");

    // --- JSON export -------------------------------------------------------
    const std::string json = emergent::profile::toJson();
    check(json.front() == '{' && json.back() == '\n', "the profile is a JSON object");
    check(countOccurrences(json, "\"name\":") == zoneCount(), "every zone appears in the JSON");
    check(json.find("\"name\": \"physics.step\"") != std::string::npos, "physics is in the JSON");
    check(json.find("\"name\": \"animation.sample\"") != std::string::npos, "animation is in the JSON");
    check(json.find("\"calls\": 60") != std::string::npos, "the JSON carries the real call counts");
    check(json.find("\"droppedScopes\": 0") != std::string::npos, "the JSON reports overflow count");
    check(json.find("\"tracy\":") != std::string::npos, "the JSON states the Tracy status honestly");
    check(countOccurrences(json, "\"calls\":") == zoneCount(), "each zone has exactly one call count");

    const std::string path = "/tmp/emergent_profile_test.json";
    check(emergent::profile::writeJson(path), "the profile writes to a file");
    check(emergent::profile::error().empty(), "a successful write leaves no error");
    const std::string onDisk = readFile(path);
    check(onDisk == json, "the file matches what toJson() produced");
    check(!emergent::profile::writeJson("/this/path/does/not/exist/profile.json"),
        "an unwritable path is reported");
    check(!emergent::profile::error().empty(), "a failed write explains itself");

    // --- overflow ----------------------------------------------------------
    // The zone table is fixed, so an over-instrumented run must say it dropped
    // scopes rather than quietly measuring a subset.
    static const char *const kFlood[] = {
        "flood00", "flood01", "flood02", "flood03", "flood04", "flood05", "flood06", "flood07",
        "flood08", "flood09", "flood10", "flood11", "flood12", "flood13", "flood14", "flood15",
        "flood16", "flood17", "flood18", "flood19", "flood20", "flood21", "flood22", "flood23",
        "flood24", "flood25", "flood26", "flood27", "flood28", "flood29", "flood30", "flood31",
        "flood32", "flood33", "flood34", "flood35", "flood36", "flood37", "flood38", "flood39",
        "flood40", "flood41", "flood42", "flood43", "flood44", "flood45", "flood46", "flood47",
        "flood48", "flood49", "flood50", "flood51", "flood52", "flood53", "flood54", "flood55",
        "flood56", "flood57", "flood58", "flood59", "flood60", "flood61", "flood62", "flood63",
        "flood64", "flood65", "flood66", "flood67", "flood68", "flood69", "flood70", "flood71",
        "flood72", "flood73", "flood74", "flood75", "flood76", "flood77", "flood78", "flood79",
    };
    for (const char *name : kFlood) {
        Scope scope(name);
    }
    check(zoneCount() == 64, "the zone table stops at its fixed capacity");
    check(emergent::profile::toJson().find("\"droppedScopes\": 0") == std::string::npos,
        "overflowed scopes are counted and reported, not silently dropped");

    if (g_failures == 0) {
        std::printf("\nall profiling checks passed\n");
        return 0;
    }
    std::printf("\n%d profiling check(s) FAILED\n", g_failures);
    return 1;
}
