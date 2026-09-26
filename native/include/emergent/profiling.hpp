#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace emergent {
namespace profile {

// Engine instrumentation, in two deliberately separate layers.
//
// 1. Zone accounting, always compiled in. A named scope times itself on entry
//    and exit and accumulates calls, total, minimum and maximum wall time.
//    It is the layer a CI job can assert on, because it produces a file.
//
// 2. Tracy live capture, compiled in only with -DEMERGENT_ENABLE_TRACY=ON.
//    Hot paths add Tracy's own ZoneNamedN scopes under that flag, and the same
//    zone names are used, so a JSON profile and a Tracy capture of the same run
//    line up. Tracy streams to a running Tracy server; there is no such server
//    in a container, so nothing here claims a capture was taken.

struct ZoneStats {
    const char *name = nullptr;
    std::uint64_t calls = 0;
    std::uint64_t totalNanos = 0;
    std::uint64_t minNanos = 0;
    std::uint64_t maxNanos = 0;

    double averageMs() const;
    double totalMs() const;
};

// RAII timing scope. Construction records the start, destruction records the
// elapsed time against the zone. Scopes nest: two Scopes with the same name
// simply accumulate twice.
//
// Zone accounting is not thread safe. It is a per-frame, single-threaded
// facility; concurrent scopes need Tracy's thread-aware mode instead.
class Scope {
public:
    explicit Scope(const char *name) noexcept;
    ~Scope() noexcept;
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *name_;
    std::uint64_t start_;
};

// Number of distinct zones that have recorded at least one scope.
std::size_t zoneCount() noexcept;
// Zone by index, or nullptr when out of range. Ordered by name so two runs of
// the same workload produce comparable files.
const ZoneStats *zone(std::size_t index) noexcept;
const ZoneStats *findZone(std::string_view name) noexcept;
// Discards every recorded measurement. Zone names are retained, so a scope
// that fires after a reset still shows up.
void reset() noexcept;
// Sum of every zone's total wall time.
double totalMs() noexcept;

// The run as JSON: one object with the zones, plus a summary. Stable key
// order, so a diff between two runs is meaningful.
std::string toJson();
// Writes toJson() to `path`. Returns false and sets error() if it cannot.
bool writeJson(std::string_view path);
const std::string &error() noexcept;

// -- Tracy live capture ----------------------------------------------------

// True when the build has Tracy compiled in. False means every function below
// is a documented no-op rather than a silent failure.
bool tracyCompiledIn() noexcept;
// What the Tracy layer can honestly report. There is deliberately no
// "connected" predicate: Tracy's client exposes no connection state, and
// inventing one would be the exact kind of claim this module exists to avoid.
std::string tracyStatus();
// Names the process in a Tracy capture. Does nothing when Tracy is not
// compiled in, and is safe to call more than once.
void setProgramName(std::string_view name);

} // namespace profile
} // namespace emergent
