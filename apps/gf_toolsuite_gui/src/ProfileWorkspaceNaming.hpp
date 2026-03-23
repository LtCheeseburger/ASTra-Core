#pragma once
#include "ModProfile.hpp"
#include <QString>

namespace gf::gui {

// Centralized rules for deriving readable workspace folder names for new profiles.
//
// Layout produced for new profiles:
//   <workspacesRoot>/<gameSlug>/<profileSlug>__<shortProfileId>/
//
// Examples:
//   workspaces/ncaa-football-14/base__f1a41b9c/
//   workspaces/ncaa-football-14/2002-roster__02850235/
//
// Compatibility:
//   Existing profiles keep their original workspacePath forever — this naming is
//   applied only when a caller supplies a non-empty gameDisplayName.  Callers that
//   do not supply a display name receive a path via the legacy
//   ModProfileStore::workspacePathFor(gameId, profileId) function.
//
// This is the single authority for slug/naming logic.  Do not duplicate it.
class ProfileWorkspaceNaming {
public:
    // Converts a human-readable display name to a filesystem-safe, lowercase slug.
    //
    // Algorithm:
    //   1. Trim and lowercase.
    //   2. Replace any run of characters outside [a-z0-9] with a single '-'.
    //   3. Strip leading/trailing '-'.
    //   4. Truncate to kMaxSlugLength, then strip any trailing '-'.
    //   5. If empty after all of the above, return `fallback`.
    //
    // Examples:
    //   "NCAA Football 14"    → "ncaa-football-14"
    //   "CFBR v21 Roster"     → "cfbr-v21-roster"
    //   "Base / Vanilla"      → "base-vanilla"
    //   "  🎮  "              → <fallback>
    static QString makeSlug(const QString& displayName,
                            const QString& fallback = QStringLiteral("profile"));

    // Returns the first hex segment of a UUID (8 characters before the first '-').
    // Used as the short disambiguator in the folder name.
    // Example: "f1a41b9c-7c8d-..." → "f1a41b9c"
    static QString shortId(const ModProfileId& profileId);

    // Builds the workspace path for a newly created named profile.
    //   workspacesRoot/<gameSlug>/<profileSlug>__<shortProfileId>
    //
    // The path is deterministic given the inputs, but the shortId component
    // ensures collision-resistance across profiles with the same display name.
    static QString namedWorkspacePath(const QString&      workspacesRoot,
                                      const QString&      gameDisplayName,
                                      const QString&      profileName,
                                      const ModProfileId& profileId);

    static constexpr int kMaxSlugLength = 32;
};

} // namespace gf::gui
