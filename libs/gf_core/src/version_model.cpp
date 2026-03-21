#include "gf/core/version_model.hpp"

#include <charconv>
#include <cctype>

namespace gf::core {

// ── classifyPreRelease ────────────────────────────────────────────────────────

ReleaseClassification classifyPreRelease(std::string_view s) noexcept {
    if (s.empty()) return ReleaseClassification::Stable;

    // Case-insensitive prefix match helper.
    auto startsWith = [&](std::string_view prefix) -> bool {
        if (s.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                static_cast<unsigned char>(prefix[i]))
                return false;
        }
        return true;
    };

    if (startsWith("rc"))      return ReleaseClassification::ReleaseCandidate;
    if (startsWith("nightly")) return ReleaseClassification::Nightly;
    if (startsWith("beta"))    return ReleaseClassification::Beta;
    if (startsWith("b"))       return ReleaseClassification::Beta;
    if (startsWith("alpha"))   return ReleaseClassification::Dev;
    if (startsWith("dev"))     return ReleaseClassification::Dev;
    if (startsWith("pre"))     return ReleaseClassification::Dev;
    if (startsWith("a"))       return ReleaseClassification::Dev;
    return ReleaseClassification::Unknown;
}

// ── ParsedVersion::parse ──────────────────────────────────────────────────────

ParsedVersion ParsedVersion::parse(std::string_view tag) noexcept {
    ParsedVersion v;

    // Strip leading 'v' / 'V'.
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
        tag.remove_prefix(1);
    if (tag.empty()) return v;

    // Split on first '-' to separate numeric part from pre-release suffix.
    auto dashPos     = tag.find('-');
    std::string_view numericPart = (dashPos == std::string_view::npos)
                                        ? tag
                                        : tag.substr(0, dashPos);
    std::string_view prePart     = (dashPos == std::string_view::npos)
                                        ? std::string_view{}
                                        : tag.substr(dashPos + 1);

    // Parse up to three dot-separated numeric components.
    int* slots[] = {&v.major, &v.minor, &v.patch};
    int  slotIdx = 0;
    std::size_t pos = 0;
    while (pos < numericPart.size() && slotIdx < 3) {
        const char* begin = numericPart.data() + pos;
        const char* end   = numericPart.data() + numericPart.size();
        int component = 0;
        auto [ptr, ec] = std::from_chars(begin, end, component);
        if (ec != std::errc{}) break;
        *slots[slotIdx++] = component;
        pos = static_cast<std::size_t>(ptr - numericPart.data());
        if (pos < numericPart.size() && numericPart[pos] == '.') ++pos;
    }

    if (!prePart.empty()) {
        v.preRelease    = std::string{prePart};
        v.classification = classifyPreRelease(prePart);
    }

    return v;
}

// ── ParsedVersion comparisons ─────────────────────────────────────────────────

bool ParsedVersion::operator==(const ParsedVersion& o) const noexcept {
    return major      == o.major
        && minor      == o.minor
        && patch      == o.patch
        && preRelease == o.preRelease;
}

bool ParsedVersion::operator<(const ParsedVersion& o) const noexcept {
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    if (patch != o.patch) return patch < o.patch;

    // Same numeric version: a pre-release is LESS THAN a stable release.
    const bool thisPre  = !preRelease.empty();
    const bool otherPre = !o.preRelease.empty();
    if (thisPre != otherPre)
        return thisPre; // pre-release < stable

    if (thisPre && otherPre) {
        // Both pre-release: more stable classification = greater version.
        // (RC > Beta > Nightly > Dev > Unknown)
        const int r1 = classificationRank(classification);
        const int r2 = classificationRank(o.classification);
        if (r1 != r2) return r1 > r2; // higher rank number = less mature = lesser version
        return preRelease < o.preRelease;  // tiebreak lexicographically
    }

    return false; // equal
}

// ── ParsedVersion::toString ───────────────────────────────────────────────────

std::string ParsedVersion::toString() const {
    std::string s = "v";
    s += std::to_string(major);
    s += '.';
    s += std::to_string(minor);
    s += '.';
    s += std::to_string(patch);
    if (!preRelease.empty()) {
        s += '-';
        s += preRelease;
    }
    return s;
}

} // namespace gf::core
