#pragma once
#include "ModProfile.hpp"
#include <QHash>
#include <QVector>
#include <optional>

namespace gf::gui {

// Persists the global profile index (mod_profiles/index.json) and per-workspace
// metadata (profile.json). All writes use gf::core::safe_write_text for atomicity.
//
// Index location:  <appDataDir>/mod_profiles/index.json
// Workspace base:  <appDataDir>/mod_profiles/workspaces/<gameId>/<profileId>/
// Profile JSON:    <workspacePath>/profile.json
//
// Schema version 1.
class ModProfileStore {
public:
    // Well-known path helpers
    static QString indexPath();
    static QString defaultWorkspaceRoot();  // base dir for auto-generated workspaces
    static QString workspacePathFor(const QString& gameId, const ModProfileId& id);

    // Global index I/O
    bool loadIndex(QVector<ModProfile>&         outProfiles,
                   QHash<QString, ModProfileId>& outActiveByGame,
                   QString*                      outErr) const;

    bool saveIndex(const QVector<ModProfile>&         profiles,
                   const QHash<QString, ModProfileId>& activeByGame,
                   QString*                            outErr) const;

    // Per-workspace profile.json (written on create/rename)
    bool writeProfileJson(const ModProfile& profile, QString* outErr) const;
    std::optional<ModProfile> readProfileJson(const QString& workspacePath,
                                              QString*       outErr) const;
};

} // namespace gf::gui
