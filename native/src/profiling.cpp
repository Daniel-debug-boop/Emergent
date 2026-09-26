#include "emergent/profiling.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef EMERGENT_WITH_TRACY
// EMERGENT's headers include <cstdio>, which defines the same get/set macros
// Tracy's headers redefine, so Tracy has to come first.
#define TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

namespace emergent {
namespace profile {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nowNanos() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// A fixed table rather than a map: the zone set is a handful of compile-time
// names, and a profiler that can allocate is a profiler that can fail to
// record a frame. A run that overflows says so, via droppedScopes.
constexpr std::size_t kMaxZones = 64;
std::array<ZoneStats, kMaxZones> g_zones{};
std::size_t g_zoneCount = 0;
std::uint64_t g_droppedScopes = 0;
std::string g_error;

ZoneStats *slotFor(const char *name) noexcept {
    for (std::size_t i = 0; i < g_zoneCount; ++i) {
        if (std::strcmp(g_zones[i].name, name) == 0) return &g_zones[i];
    }
    if (g_zoneCount == kMaxZones) {
        ++g_droppedScopes;
        return nullptr;
    }
    ZoneStats &slot = g_zones[g_zoneCount++];
    slot.name = name;
    return &slot;
}

void record(const char *name, std::uint64_t elapsed) noexcept {
    ZoneStats *slot = slotFor(name);
    if (!slot) return;
    ++slot->calls;
    slot->totalNanos += elapsed;
    if (slot->calls == 1 || elapsed < slot->minNanos) slot->minNanos = elapsed;
    if (elapsed > slot->maxNanos) slot->maxNanos = elapsed;
}

// Zone indices are kept sorted by name so two comparable runs list the same
// zones in the same order and a diff between the two JSON files is meaningful.
void sortZones() noexcept {
    for (std::size_t i = 1; i < g_zoneCount; ++i) {
        for (std::size_t j = i; j > 0 && std::strcmp(g_zones[j - 1].name, g_zones[j].name) > 0; --j) {
            std::swap(g_zones[j - 1], g_zones[j]);
        }
    }
}

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string millis(double nanos) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", nanos / 1.0e6);
    return buffer;
}

} // namespace

double ZoneStats::averageMs() const {
    return calls == 0 ? 0.0 : static_cast<double>(totalNanos) / static_cast<double>(calls) / 1.0e6;
}

double ZoneStats::totalMs() const { return static_cast<double>(totalNanos) / 1.0e6; }

Scope::Scope(const char *name) noexcept : name_(name), start_(nowNanos()) {}

Scope::~Scope() noexcept {
    if (name_) record(name_, nowNanos() - start_);
}

std::size_t zoneCount() noexcept { return g_zoneCount; }

const ZoneStats *zone(std::size_t index) noexcept {
    return index < g_zoneCount ? &g_zones[index] : nullptr;
}

const ZoneStats *findZone(std::string_view name) noexcept {
    for (std::size_t i = 0; i < g_zoneCount; ++i) {
        if (name == g_zones[i].name) return &g_zones[i];
    }
    return nullptr;
}

void reset() noexcept {
    for (std::size_t i = 0; i < g_zoneCount; ++i) {
        g_zones[i].calls = 0;
        g_zones[i].totalNanos = 0;
        g_zones[i].minNanos = 0;
        g_zones[i].maxNanos = 0;
    }
    g_droppedScopes = 0;
}

double totalMs() noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < g_zoneCount; ++i) total += g_zones[i].totalNanos;
    return static_cast<double>(total) / 1.0e6;
}

std::string toJson() {
    sortZones();
    std::string out = "{\n  \"zones\": [\n";
    for (std::size_t i = 0; i < g_zoneCount; ++i) {
        const ZoneStats &slot = g_zones[i];
        out += "    {\"name\": \"";
        out += escapeJson(slot.name);
        out += "\", \"calls\": ";
        out += std::to_string(slot.calls);
        out += ", \"totalMs\": ";
        out += millis(static_cast<double>(slot.totalNanos));
        out += ", \"averageMs\": ";
        out += millis(slot.averageMs() * 1.0e6);
        out += ", \"minMs\": ";
        out += millis(static_cast<double>(slot.minNanos));
        out += ", \"maxMs\": ";
        out += millis(static_cast<double>(slot.maxNanos));
        out += "}";
        if (i + 1 < g_zoneCount) out += ',';
        out += '\n';
    }
    out += "  ],\n  \"summary\": {\"zoneCount\": ";
    out += std::to_string(g_zoneCount);
    out += ", \"totalMs\": ";
    out += millis(totalMs() * 1.0e6);
    out += ", \"droppedScopes\": ";
    out += std::to_string(g_droppedScopes);
    out += ", \"tracy\": \"";
    out += escapeJson(tracyStatus());
    out += "\"}\n}\n";
    return out;
}

bool writeJson(std::string_view path) {
    g_error.clear();
    const std::string text = toJson();
    const std::string target(path);
    std::FILE *file = std::fopen(target.c_str(), "wb");
    if (!file) {
        g_error = "cannot open '" + target + "' for writing";
        return false;
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool flushed = std::fflush(file) == 0;
    std::fclose(file);
    if (written != text.size() || !flushed) {
        g_error = "short write on '" + target + "'";
        return false;
    }
    return true;
}

const std::string &error() noexcept { return g_error; }

bool tracyCompiledIn() noexcept {
#ifdef EMERGENT_WITH_TRACY
    return true;
#else
    return false;
#endif
}

std::string tracyStatus() {
#ifdef EMERGENT_WITH_TRACY
    return "compiled in; a capture needs a running Tracy server";
#else
    return "not compiled in (-DEMERGENT_ENABLE_TRACY=ON)";
#endif
}

void setProgramName(std::string_view name) {
#ifdef EMERGENT_WITH_TRACY
    // Tracy's C++ API wants a NUL-terminated name. The caller owns the
    // string_view's lifetime, so it is copied rather than retained.
    const std::string owned(name);
    tracy::GetProfiler().SetProgramName(owned.c_str());
#else
    (void)name;
#endif
}

} // namespace profile
} // namespace emergent
