#pragma once
#include <QString>

namespace gf::gui {

// Stable, opaque identifier for a mod profile.
// Persisted as a UUID-without-braces string — never change format between versions.
using ModProfileId = QString;

// Classification of a baseline profile's source content.
enum class BaselineType {
    Vanilla  = 0, // Unmodified retail / vanilla game files
    CFBR_v21 = 1, // College Football Revamped v21 base install
    Custom   = 2  // User-provided or otherwise unlabelled baseline
};

// Returns a human-readable label for a BaselineType.
inline QString baselineTypeLabel(BaselineType t) {
    switch (t) {
    case BaselineType::Vanilla:  return "Vanilla";
    case BaselineType::CFBR_v21: return "CFBR v21";
    case BaselineType::Custom:   return "Custom";
    }
    return "Custom";
}

// A mod profile pairs a named modding workspace with a specific game.
// Immutable fields: id, gameId, workspacePath, createdAt.
// Mutable fields: name, description, updatedAt, isBaseline, baselineType.
struct ModProfile {
    ModProfileId id;            // UUID without braces, e.g. "3e4f5a6b-7c8d-..."
    QString      gameId;        // Matches the cacheId used by NameCache / MainWindow
    QString      name;          // User-visible display name (non-empty)
    QString      description;   // Optional user-supplied note
    QString      workspacePath; // Absolute canonical path to workspace root
    QString      createdAt;     // UTC ISO 8601
    QString      updatedAt;     // UTC ISO 8601

    // Phase 4A: baseline-capture metadata.
    // Default values preserve backward-compatibility with existing persisted profiles.
    bool         isBaseline   = false;
    BaselineType baselineType = BaselineType::Custom;

    bool isValid() const {
        return !id.isEmpty() && !gameId.isEmpty() && !name.isEmpty() && !workspacePath.isEmpty();
    }
};

} // namespace gf::gui
