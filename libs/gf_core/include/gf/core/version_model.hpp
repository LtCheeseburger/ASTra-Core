#pragma once
#include <string>
#include <string_view>

namespace gf::core {

// ── Release classification ────────────────────────────────────────────────────
// Stability tier inferred from the pre-release suffix in a version tag.
enum class ReleaseClassification {
    Stable,            // e.g. "v1.0.0"               (no suffix)
    ReleaseCandidate,  // e.g. "v1.0.0-rc.1"
    Beta,              // e.g. "v1.0.0-beta.1"
    Nightly,           // e.g. "v1.0.0-nightly.20240115"
    Dev,               // e.g. "v1.0.0-dev", "v1.0.0-pre", "v1.0.0-alpha"
    Unknown            // unrecognised suffix
};

// Numeric stability rank — 0 = most stable.
// Lower number = more stable = eligible for more restricted channels.
inline int classificationRank(ReleaseClassification c) noexcept {
    switch (c) {
        case ReleaseClassification::Stable:           return 0;
        case ReleaseClassification::ReleaseCandidate: return 1;
        case ReleaseClassification::Beta:             return 2;
        case ReleaseClassification::Nightly:          return 3;
        case ReleaseClassification::Dev:              return 4;
        case ReleaseClassification::Unknown:          return 5;
    }
    return 5;
}

// Classify a pre-release suffix string (empty string → Stable).
ReleaseClassification classifyPreRelease(std::string_view preRelease) noexcept;

// ── ParsedVersion ─────────────────────────────────────────────────────────────
struct ParsedVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string preRelease;   // e.g. "beta.1"  or  "" for stable
    ReleaseClassification classification = ReleaseClassification::Stable;

    bool isStable()     const noexcept { return classification == ReleaseClassification::Stable; }
    bool isPreRelease() const noexcept { return !preRelease.empty(); }

    // A parsed version is "valid" if at least one numeric component is non-zero.
    bool valid() const noexcept { return major > 0 || minor > 0 || patch > 0; }

    // Comparisons.
    // Pre-release of the same base version sorts BEFORE stable
    // (i.e. v1.0.0-beta.1 < v1.0.0 < v1.0.1).
    bool operator==(const ParsedVersion& o) const noexcept;
    bool operator< (const ParsedVersion& o) const noexcept;
    bool operator> (const ParsedVersion& o) const noexcept { return o < *this; }
    bool operator<=(const ParsedVersion& o) const noexcept { return !(*this > o); }
    bool operator>=(const ParsedVersion& o) const noexcept { return !(*this < o); }

    // Parse a tag string: "v1.0.0", "1.2.3-beta.1", "v2.0.0-nightly.20240115".
    // Returns ParsedVersion{} (all zeros, Stable) on parse failure.
    static ParsedVersion parse(std::string_view tag) noexcept;

    // Human-readable: "v1.0.0" or "v1.0.0-beta.1"
    std::string toString() const;
};

} // namespace gf::core
